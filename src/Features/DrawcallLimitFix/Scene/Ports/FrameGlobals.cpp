#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPort.h"
#include "Features/DrawcallLimitFix/Engine/Access/Walkers.h"

#include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#include "Features/DrawcallLimitFix/Draws/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Utils/Game.h"

#include <algorithm>
#include <string>

#include <ankerl/unordered_dense.h>
#include <atomic>

namespace DCLF
{
	namespace
	{
		using Engine::Global;

		// AE 1.6.1170, module offsets (the readers' notes name each).
		constexpr std::uintptr_t kTechniqueByte12 = 0x2032fdb;
		constexpr std::uintptr_t kTechniqueByte7 = 0x2035500;
		constexpr std::uintptr_t kShadowGlobal = 0x2033498;
		constexpr std::size_t kAccumulatorDeferredShadow = 0x178;
		constexpr std::uintptr_t kLodRadiusBase = 0x2032e40;
		constexpr std::uintptr_t kLodExponentScale = 0x2032e54;
		constexpr std::uintptr_t kLodPowBase = 0x2032e44;

		// A setting's value: the setting itself looked up once (GetINISetting walks the collection by name), its value every frame.
		struct CachedSetting
		{
			const char* name;
			float fallback;
			RE::Setting* setting = nullptr;
			bool looked = false;
			float Get()
			{
				if (!std::exchange(looked, true))
					setting = RE::GetINISetting(name);
				return setting ? setting->GetFloat() : fallback;
			}
		};
		CachedSetting iniSpecularStart{ "fSpecularLODFadeStart:LightingShader", 0.09f };
		CachedSetting iniSpecularEnd{ "fSpecularLODFadeEnd:LightingShader", 0.10f };
		CachedSetting iniEnvmapStart{ "fEnvmapLODFadeStart:LightingShader", 0.09f };
		CachedSetting iniEnvmapEnd{ "fEnvmapLODFadeEnd:LightingShader", 0.10f };
		// T7d's refraction: GetRenderPasses' refraction LOD fade (the Setting objects' values it reads, AE 0x142033408 and 0x142033420).
		CachedSetting iniRefractionStart{ "fRefractionLODFadeStart:LightingShader", 0.025f };
		CachedSetting iniRefractionEnd{ "fRefractionLODFadeEnd:LightingShader", 0.03f };

		bool Hidden(const RE::NiAVObject* a_object) { return Engine::IsHidden(a_object); }

		/**
		 * @brief T7e: one node of the first-person subtree as a walk of the live tree found it (render thread): its key (an identity,
		 * never dereferenced after the walk), its hidden bit, whether it is a geometry and its name (the diagnostic's, and an identity
		 * check against an address the engine reused for another node).
		 */
		struct SubtreeBit
		{
			const RE::NiAVObject* key = nullptr;
			bool hidden = false, geometry = false;
			std::string name;
		};

		// The subtree's bits at the engine's first-person culls (NoteFirstPersonCulled), by key; render thread. Copies only: an entry's key
		// may be freed by the next frame (an equipment change), so it is only looked up for a node the next frame's live walk found.
		ankerl::unordered_dense::map<const RE::NiAVObject*, SubtreeBit> firstPersonCulled;
		const RE::NiAVObject* firstPersonCulledRoot = nullptr;  // compared, never dereferenced

		/** @brief Every node and geometry under a_root (not a_root), walked now (the tree alive: the render thread, inside the frame). */
		void SubtreeBits(const RE::NiAVObject* a_root, std::vector<SubtreeBit>& a_out)
		{
			a_out.clear();
			if (!const_cast<RE::NiAVObject*>(a_root)->AsNode())
				return;
			Engine::ForEachLive(a_root, [&](RE::NiAVObject& a_object, std::uint32_t a_depth) {
				if (a_depth)
					a_out.push_back({ &a_object, Hidden(&a_object), a_object.AsGeometry() != nullptr, a_object.name.c_str() ? a_object.name.c_str() : "" });
			});
		}

		/**
		 * @brief T7e's diagnostic (render thread): the geometry of the first-person subtree hidden at the frame's start, and at the last
		 * frame's culls (of the nodes still in the tree), logged when the two lists change (at most 24 times). Reads the copies alone.
		 */
		void NoteFirstPersonDifference(const std::vector<SubtreeBit>& a_live, bool a_culled)
		{
			static std::string last;
			static std::uint32_t logged = 0;
			if (logged >= 24)
				return;
			auto names = [&](bool a_atCull) {
				std::string text;
				std::uint32_t geometry = 0, hidden = 0;
				for (const auto& bit : a_live) {
					if (!bit.geometry)
						continue;
					++geometry;
					bool isHidden = bit.hidden;
					if (a_atCull) {
						const auto it = firstPersonCulled.find(bit.key);
						if (it == firstPersonCulled.end() || it->second.name != bit.name)
							continue;
						isHidden = it->second.hidden;
					}
					if (!isHidden)
						continue;
					++hidden;
					if (hidden <= 12)
						text += fmt::format("{}'{}'", text.empty() ? "" : ", ", bit.name);
				}
				return fmt::format("{} geometries, {} hidden{}{}", geometry, hidden, text.empty() ? "" : ": ", text);
			};
			std::string now = "at the frame's start " + names(false) + (a_culled ? "; at the last first-person culls " + names(true) : std::string("; no first-person cull seen"));
			if (now == last)
				return;
			last = now;
			++logged;
			logger::info("[DCLF] first-person subtree (T7e): {}", now);
		}

		// The render thread's latest capture (written by the render thread alone), and the ones before it: an unscoped reader (a
		// defect, counted) holds a reference to one for its read, so a few frames' are kept alive past their replacement.
		std::shared_ptr<const FrameGlobals> latest = std::make_shared<FrameGlobals>();
		std::array<std::shared_ptr<const FrameGlobals>, 8> recent;
		std::uint32_t recentNext = 0;
		std::atomic<std::uint32_t> renderThread{ 0 };
		std::atomic<std::uint64_t> unscopedReads{ 0 };
		thread_local const FrameGlobals* scoped = nullptr;
	}

	void FrameGlobals::NoteFirstPersonCulled(const RE::NiAVObject* a_root)
	{
		// The render thread, at RenderDepth's first-person draw: the tree alive. Copies taken; the keys are compared next frame, never read.
		firstPersonCulledRoot = a_root;
		firstPersonCulled.clear();
		if (!a_root)
			return;
		std::vector<SubtreeBit> bits;
		SubtreeBits(a_root, bits);
		for (auto& bit : bits)
			firstPersonCulled.insert_or_assign(bit.key, std::move(bit));
	}

	std::shared_ptr<const FrameGlobals> FrameGlobals::Capture()
	{
		ZoneScopedN("CS.DCLF.FrameGlobals.Capture");
		renderThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		auto g = std::make_shared<FrameGlobals>();
		g->loading = Scene::FrameView::IsLoadingScreenUp();
		g->interior = Util::IsInterior();
		g->decalBias = { 0u, DecalDepthBiasMode(1), DecalDepthBiasMode(2), DecalDepthBiasMode(3) };
		g->specularStart = iniSpecularStart.Get();
		g->specularEnd = iniSpecularEnd.Get();
		g->envmapStart = iniEnvmapStart.Get();
		g->envmapEnd = iniEnvmapEnd.Get();
		g->refractionStart = iniRefractionStart.Get();
		g->refractionEnd = iniRefractionEnd.Get();
		g->techniqueByte12 = Global<std::uint8_t>(kTechniqueByte12);
		g->techniqueByte7 = Global<std::uint8_t>(kTechniqueByte7);
		g->shadowGlobal = Global<std::uint8_t>(kShadowGlobal);
		if (const auto* accumulator = Global<std::uint8_t*>(Engine::Address::kMainAccumulator)) {
			g->accumulator = true;
			g->accumulatorDeferredShadow = accumulator[kAccumulatorDeferredShadow];
		}
		g->noSunShadowDir = Global<std::uint8_t>(Engine::Address::kNoSunShadowDir);
		g->screenDoorFades = Global<std::uint8_t>(Engine::Address::kScreenDoorFades);
		g->fadesOn = Global<std::uint8_t>(Engine::Address::kFadesOn);
		g->fadeLodUpdates = Global<std::uint8_t>(Engine::Address::kFadeLodUpdates);
		g->fadeSpecialA = Global<float>(Engine::Address::kFadeSpecialA);
		g->fadeSpecialB = Global<float>(Engine::Address::kFadeSpecialB);
		g->fadeDistanceMult = Global<float>(Engine::Address::kFadeDistanceMult);
		g->fadeOutThreshold = Global<float>(Engine::Address::kFadeOutThreshold);
		g->fadeDefaultScale = Global<float>(Engine::Address::kFadeDefaultScale);
		g->metricScale = Global<float>(Engine::Address::kMetricScale);
		for (std::uint32_t type = 0; type < g->fadeTypeDivisors.size(); ++type)
			g->fadeTypeDivisors[type] = Global<float>(Engine::Address::kFadeTypeDivisors + type * 4);
		g->lodRadiusBase = Global<float>(kLodRadiusBase);
		g->lodExponentScale = Global<float>(kLodExponentScale);
		g->lodPowBase = Global<float>(kLodPowBase);
		if (const auto* camera = RE::Main::WorldRootCamera()) {
			g->eye = { camera->world.translate.x, camera->world.translate.y, camera->world.translate.z };
			g->lodAdjust = camera->GetRuntimeData2().lodAdjust;
		}
		if (const auto source = Global<std::uintptr_t>(Engine::Address::kShadowSceneNode))
			g->treeWindMagnitude = *reinterpret_cast<const float*>(source + 0x304);
		g->treeWindFadeStart = Global<float>(Engine::Address::kTreeWindFadeStart);
		g->treeWindFadeEnd = Global<float>(Engine::Address::kTreeWindFadeEnd);
		g->treeWindTimerScale = Global<float>(Engine::Address::kTreeWindTimerScale);
		// The roots whose hidden bit the frame changes around the scene's views (Coordinator::HiddenForWalk), as the views see it,
		// at the point the scene work is kicked from (after Main::Draw's early call).
		if (!g->loading) {
			// Numbered before the reads (a locked add): a store whose event is numbered before it is in them.
			g->cullHiddenSequence = SceneCapture::NextSequence();
			for (auto* sceneNode : RE::BSShaderManager::State::GetSingleton().shadowSceneNode) {
				const auto* graph = sceneNode ? sceneNode->GetRuntimeData().portalGraph : nullptr;
				if (!graph)
					continue;
				for (const auto& child : graph->alwaysRenderChildren)
					if (child)
						g->cullHidden.emplace_back(child.get(), Hidden(child.get()));
				if (graph->portalSharedNode)
					g->cullHidden.emplace_back(graph->portalSharedNode.get(), Hidden(graph->portalSharedNode.get()));
			}
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				// The third-person skeleton as it is now. TESWaterReflections::Update (AE 0x140520570), on the frames a cube-map
				// reflection updates, hides the player's 3D while it renders the faces and then restores it; Main::Draw calls it
				// between the main cull jobs' Begin and Finish.
				const RE::NiAVObject* thirdPerson = player->Get3D(false);
				if (thirdPerson)
					g->cullHidden.emplace_back(thirdPerson, Hidden(thirdPerson));
				// The first-person skeleton: Main::Draw (AE 0x1406444b0) hides it right after the call the scene work is kicked from,
				// keeps it hidden through the main camera's cull and the sun's shadow casters, and shows it only to draw the
				// first-person view with its own camera. For every view the scene serves it is hidden.
				const RE::NiAVObject* firstPerson = player->Get3D(true);
				if (firstPerson && firstPerson != thirdPerson)
					g->cullHidden.emplace_back(firstPerson, true);
				// T7e (CS_DCLF_FIRST_PERSON): every node and geometry under the first-person root, with the bit the engine's first-person
				// culls saw last frame (NoteFirstPersonCulled), else as it is now (this thread, before Main::Draw touches the skeleton). Not
				// every store to them has an event, and the frame start's bits are not the culls' (the runs of 2026-10-11 kept the arms
				// hidden while the engine drew them every frame), so the first-person members' chains read their bits from here.
				if (firstPerson && firstPerson != thirdPerson && ActiveToggles().firstPerson) {
					// The live walk proves each key alive; a culled bit is taken for a key found here with the same name (an address the
					// engine freed and reused for another node since is not the same node), else the live bit.
					std::vector<SubtreeBit> live;
					SubtreeBits(firstPerson, live);
					const bool culled = firstPersonCulledRoot == firstPerson && !firstPersonCulled.empty();
					// The live chain of the first geometry, once (render thread: the tree alive): where the skeleton hangs.
					static bool chainLogged = false;
					if (!chainLogged)
						for (const auto& bit : live)
							if (bit.geometry && !bit.hidden) {
								chainLogged = true;
								std::string chain;
								std::uint32_t at = 0;
								for (const RE::NiAVObject* node = bit.key; node && at < 24; node = node->parent, ++at)
									chain += fmt::format("{}'{}' {}", at ? " <- " : "", node->name.c_str() ? node->name.c_str() : "", static_cast<const void*>(node));
								logger::info("[DCLF] first-person subtree (T7e): the root {} (parent {}); a geometry's live chain: {}", static_cast<const void*>(firstPerson),
									static_cast<const void*>(firstPerson->parent), chain);
								break;
							}
					for (const auto& bit : live) {
						if (bit.geometry)
							g->firstPersonGeometry.push_back(bit.key);
						bool hidden = bit.hidden;
						if (culled)
							if (const auto it = firstPersonCulled.find(bit.key); it != firstPersonCulled.end() && it->second.name == bit.name)
								hidden = it->second.hidden;
						g->cullHidden.emplace_back(bit.key, hidden);
					}
					NoteFirstPersonDifference(live, culled);
					std::sort(g->firstPersonGeometry.begin(), g->firstPersonGeometry.end());
				}
			}
			// The water objects' shapes as they are now: TESWaterReflections::Update hides them around the cube map's faces too, and
			// restores them after.
			if (auto* water = RE::TESWaterSystem::GetSingleton())
				for (const auto& object : water->waterObjects)
					if (object && object->shape)
						g->cullHidden.emplace_back(object->shape.get(), Hidden(object->shape.get()));
			std::sort(g->cullHidden.begin(), g->cullHidden.end());
		}
		g->membershipWitness = PrimaryCull::MembershipWitness(*g);
		g->technique = SampleTechniqueInputs();
		g->shadowMaskHeld.copy_from(g->technique.shadowMask);  // the renderer's target holds it now: alive
		g->material = MaterialPort::SampleFrame();
		recent[recentNext++ % recent.size()] = latest;
		latest = g;
		return g;
	}

	const FrameGlobals& FrameGlobals::Current()
	{
		if (scoped)
			return *scoped;
		if (::GetCurrentThreadId() != renderThread.load(std::memory_order_relaxed) && unscopedReads.fetch_add(1, std::memory_order_relaxed) == 0)
			logger::error("[DCLF] the frame's engine globals were read off the render thread with no frame's capture bound (step 6e F2); it got the render thread's latest");
		return *latest;
	}

	std::uint64_t FrameGlobals::TakeUnscopedReads()
	{
		return unscopedReads.exchange(0, std::memory_order_relaxed);
	}

	FrameGlobals::Scope::Scope(std::shared_ptr<const FrameGlobals> a_globals) :
		held(std::move(a_globals)),
		previous(scoped)
	{
		scoped = held.get();
	}

	FrameGlobals::Scope::~Scope()
	{
		scoped = previous;
	}
}
