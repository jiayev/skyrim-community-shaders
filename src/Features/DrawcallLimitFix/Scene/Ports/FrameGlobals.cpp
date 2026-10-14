// FrameGlobals.h's capture (render thread, the frame's start), the first-person subtree's cull bits and the scoped reads.

#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPort.h"
#include "Features/DrawcallLimitFix/Engine/Access/Walkers.h"

#include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#include "Features/DrawcallLimitFix/Draws/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
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
		// GetRenderPasses' refraction LOD fade (the Setting objects' values it reads, AE 0x142033408 and 0x142033420).
		CachedSetting iniRefractionStart{ "fRefractionLODFadeStart:LightingShader", 0.025f };
		CachedSetting iniRefractionEnd{ "fRefractionLODFadeEnd:LightingShader", 0.03f };

		bool Hidden(const RE::NiAVObject* a_object) { return Engine::IsHidden(a_object); }

		/**
		 * @brief One node of the first-person subtree (render thread): its key (an identity, dereferenced only right after a walk of the
		 * live tree found it), whether it is a geometry, its hidden bit now (the hidden events applied) and, saved when an event changed it
		 * after the engine's last first-person culls, its bit at those culls.
		 */
		struct SubtreeNode
		{
			const RE::NiAVObject* key = nullptr;
			bool geometry = false, hidden = false, culled = false, saved = false;
		};

		/**
		 * @brief The first-person subtree's bits, kept by its events (SceneTracker::SubtreeEvents: hidden stores, detached keys, attaches)
		 * and applied at the two engine moments (the frame's start, the first-person culls); render thread. The live tree is walked only
		 * when its membership changed (an attach, a detached key, a new root): a key still indexed keeps its bits, a new one takes its live
		 * bit. A detached key leaves at once, so an address the engine frees and reuses is a new node.
		 */
		struct Subtree
		{
			const RE::NiAVObject* root = nullptr;  // compared; dereferenced only on the render thread inside the frame
			std::vector<SubtreeNode> nodes;        // by key: an entry's index is stable until the next walk
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> index;
			std::vector<const RE::NiAVObject*> geometry;  // the geometry keys, sorted (FrameGlobals::firstPersonGeometry)
			std::vector<std::uint32_t> saved;             // entries whose culled bit is saved since the last culls
			bool edited = false;                          // an attach or a detached key since the last walk
			bool culled = false;                          // the culls were seen for this root
			std::uint64_t changes = 0;                    // bits or membership changed (the diagnostic's trigger)
			std::uint64_t walks = 0;
		} subtree;

		/** @brief The node's bit at the last culls, else now. */
		bool CulledBit(const SubtreeNode& a_node) { return subtree.culled && a_node.saved ? a_node.culled : a_node.hidden; }

		/** @brief Every node and geometry under a_root (not a_root) as it is now (the tree alive: the render thread, inside the frame). */
		void LiveSubtree(const RE::NiAVObject* a_root, std::vector<SubtreeNode>& a_out)
		{
			a_out.clear();
			if (!const_cast<RE::NiAVObject*>(a_root)->AsNode())
				return;
			Engine::ForEachLive(a_root, [&](RE::NiAVObject& a_object, std::uint32_t a_depth) {
				if (a_depth)
					a_out.push_back({ &a_object, a_object.AsGeometry() != nullptr, Hidden(&a_object) });
			});
			std::ranges::sort(a_out, {}, &SubtreeNode::key);
		}

		/** @brief The membership walked again: keys still indexed keep their bits, new ones take the live bit. */
		void WalkSubtree()
		{
			std::vector<SubtreeNode> next;
			LiveSubtree(subtree.root, next);
			std::vector<std::uint32_t> added;  // keys new to the members: their stores had no event until the members are published
			for (std::uint32_t i = 0; i < next.size(); ++i) {
				auto& node = next[i];
				if (const auto it = subtree.index.find(node.key); it != subtree.index.end()) {
					const auto& kept = subtree.nodes[it->second];
					node.hidden = kept.hidden;
					node.culled = kept.culled;
					node.saved = kept.saved;
				} else
					added.push_back(i);
			}
			subtree.nodes = std::move(next);
			subtree.index.clear();
			subtree.geometry.clear();
			subtree.saved.clear();
			for (std::uint32_t i = 0; i < subtree.nodes.size(); ++i) {
				const auto& node = subtree.nodes[i];
				subtree.index.emplace(node.key, i);
				if (node.geometry)
					subtree.geometry.push_back(node.key);
				if (node.saved)
					subtree.saved.push_back(i);
			}
			subtree.edited = false;
			++subtree.changes;
			++subtree.walks;
			// The members the hidden stores look up (their objects' parents are not read on the hooks' threads).
			std::vector<const void*> members;
			members.reserve(subtree.nodes.size());
			for (const auto& node : subtree.nodes)
				members.push_back(node.key);
			SceneTracker::WatchSubtreeMembers(std::move(members));
			// A new key's bit read again after its publication: a store before it is in this read, one after it has an event.
			for (const std::uint32_t i : added)
				subtree.nodes[i].hidden = Hidden(subtree.nodes[i].key);
		}

		/**
		 * @brief At an engine moment (render thread, the tree alive): the subtree under a_root brought up to now - the events since the last
		 * moment applied, the membership walked again when it changed. A new root (or none) starts over.
		 */
		void SyncSubtree(const RE::NiAVObject* a_root)
		{
			auto& events = SceneTracker::SubtreeEvents();
			if (a_root != subtree.root) {
				// The old root's events dropped, then the new root watched before its walk: a store after the watch has an event, one before
				// it is in the walk's bits.
				events.Discard();
				SceneTracker::WatchSubtree(a_root);
				SceneTracker::WatchSubtreeMembers({});
				subtree = {};
				subtree.root = a_root;
				if (a_root)
					WalkSubtree();
				return;
			}
			if (!a_root)
				return;
			events.Drain([](SceneTracker::SubtreeEvent&& a_event) {
				using Kind = SceneTracker::SubtreeEvent::Kind;
				if (a_event.kind == Kind::Attached) {
					subtree.edited = true;
					return;
				}
				const auto it = subtree.index.find(a_event.node);
				if (it == subtree.index.end())
					return;  // not walked yet: the next walk reads its bit
				auto& node = subtree.nodes[it->second];
				if (a_event.kind == Kind::Removed) {
					node.key = nullptr;
					subtree.index.erase(it);
					subtree.edited = true;
					return;
				}
				const bool hidden = a_event.kind == Kind::Hidden;
				if (node.hidden == hidden)
					return;
				if (subtree.culled && !node.saved) {
					node.culled = node.hidden;
					node.saved = true;
					subtree.saved.push_back(it->second);
				}
				node.hidden = hidden;
				++subtree.changes;
			});
			if (subtree.edited)
				WalkSubtree();
		}

		/**
		 * @brief CS_DCLF_PERSISTENT_PARITY: the subtree's bits by events against a walk of the live tree now (render thread, at an engine
		 * moment, right after SyncSubtree): membership and each node's bit. A periodic line, and the first differences named.
		 */
		struct SubtreeParity
		{
			std::uint64_t checks = 0, compared = 0, membership = 0, bits = 0, walks = 0;
			std::string first;
			std::vector<SubtreeNode> live;
		} subtreeParity;

		void CheckSubtree(const char* a_moment)
		{
			auto& p = subtreeParity;
			++p.checks;
			LiveSubtree(subtree.root, p.live);
			std::size_t i = 0, j = 0;
			auto note = [&](const RE::NiAVObject* a_key, const char* a_what) {
				if (std::ranges::count(p.first, ';') < 6)
					p.first += fmt::format("{}{} '{}' {} ({})", p.first.empty() ? "" : "; ", a_what, a_key->name.c_str() ? a_key->name.c_str() : "", static_cast<const void*>(a_key), a_moment);
			};
			std::vector<SubtreeNode> kept;
			for (const auto& node : subtree.nodes)
				if (node.key)
					kept.push_back(node);
			while (i < p.live.size() || j < kept.size()) {
				if (j == kept.size() || (i < p.live.size() && p.live[i].key < kept[j].key)) {
					++p.membership;
					note(p.live[i].key, "not kept");
					++i;
				} else if (i == p.live.size() || kept[j].key < p.live[i].key) {
					++p.membership;
					++j;  // gone from the tree: not dereferenced
				} else {
					++p.compared;
					if (p.live[i].hidden != kept[j].hidden) {
						++p.bits;
						note(p.live[i].key, p.live[i].hidden ? "hidden, kept shown" : "shown, kept hidden");
					}
					++i;
					++j;
				}
			}
			if (p.checks % 600)
				return;
			logger::info("[DCLF] first-person bits by events (C3 parity): {} checks, {} nodes compared, {} membership and {} bit differences against the live walk; {} walks{}{}",
				p.checks, p.compared, p.membership, p.bits, subtree.walks - std::exchange(p.walks, subtree.walks), p.membership || p.bits ? " <- FLAG" : " <- OK",
				p.first.empty() ? "" : "; first: " + p.first);
			p.compared = p.membership = p.bits = 0;
			p.first.clear();
		}

		/**
		 * @brief The first-person diagnostic (render thread, the frame's start): the geometry of the first-person subtree hidden now and at the
		 * last culls, logged when the two lists change (at most 24 times). Evaluated only when the bits or the membership changed.
		 */
		void NoteFirstPersonDifference()
		{
			static std::string last;
			static std::uint32_t logged = 0;
			static std::uint64_t seen = ~0ull;
			if (logged >= 24 || std::exchange(seen, subtree.changes) == subtree.changes)
				return;
			auto names = [&](bool a_atCull) {
				std::string text;
				std::uint32_t geometry = 0, hidden = 0;
				for (const auto& node : subtree.nodes) {
					if (!node.key || !node.geometry)
						continue;
					++geometry;
					if (!(a_atCull ? CulledBit(node) : node.hidden))
						continue;
					++hidden;
					if (hidden <= 12)
						text += fmt::format("{}'{}'", text.empty() ? "" : ", ", node.key->name.c_str() ? node.key->name.c_str() : "");
				}
				return fmt::format("{} geometries, {} hidden{}{}", geometry, hidden, text.empty() ? "" : ": ", text);
			};
			std::string now = "at the frame's start " + names(false) + (subtree.culled ? "; at the last first-person culls " + names(true) : std::string("; no first-person cull seen"));
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
		// The render thread, at RenderDepth's first-person draw: the tree alive. The events since the frame's start applied; the bits now are
		// the culls', saved from here only for the nodes an event changes before the next culls.
		SyncSubtree(a_root);
		if (!subtree.saved.empty() || subtree.culled != (a_root != nullptr))
			++subtree.changes;
		for (const std::uint32_t i : subtree.saved)
			subtree.nodes[i].saved = false;
		subtree.saved.clear();
		subtree.culled = a_root != nullptr;
		if (a_root && Diagnostics::Observe().scene)
			CheckSubtree("at the culls");
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
		// at the frame's start (after Main::Draw's early call).
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
			const RE::NiAVObject* subtreeRoot = nullptr;
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				// The third-person skeleton as it is now. TESWaterReflections::Update (AE 0x140520570), on the frames a cube-map
				// reflection updates, hides the player's 3D while it renders the faces and then restores it; Main::Draw calls it
				// between the main cull jobs' Begin and Finish.
				const RE::NiAVObject* thirdPerson = player->Get3D(false);
				if (thirdPerson)
					g->cullHidden.emplace_back(thirdPerson, Hidden(thirdPerson));
				// The first-person skeleton: Main::Draw (AE 0x1406444b0) hides it right after the call the frame's start runs from,
				// keeps it hidden through the main camera's cull and the sun's shadow casters, and shows it only to draw the
				// first-person view with its own camera. For every view the scene serves it is hidden.
				const RE::NiAVObject* firstPerson = player->Get3D(true);
				if (firstPerson && firstPerson != thirdPerson)
					g->cullHidden.emplace_back(firstPerson, true);
				if (firstPerson && firstPerson != thirdPerson && ActiveToggles().firstPerson)
					subtreeRoot = firstPerson;
			}
			// The water objects' shapes as they are now: TESWaterReflections::Update hides them around the cube map's faces too, and
			// restores them after.
			if (auto* water = RE::TESWaterSystem::GetSingleton())
				for (const auto& object : water->waterObjects)
					if (object && object->shape)
						g->cullHidden.emplace_back(object->shape.get(), Hidden(object->shape.get()));
			std::sort(g->cullHidden.begin(), g->cullHidden.end());
			// CS_DCLF_FIRST_PERSON: every node and geometry under the first-person root, with the bit the engine's first-person culls saw
			// last frame (NoteFirstPersonCulled), else as it is now (this thread, before Main::Draw touches the skeleton). The frame start's
			// bits are not the culls' (the arms are hidden at the frame's start while the engine draws them), so the first-person members'
			// chains read their bits from here. Kept by the subtree's events (SyncSubtree), not walked; merged in key order.
			SyncSubtree(subtreeRoot);
			if (subtreeRoot) {
				if (Diagnostics::Observe().scene)
					CheckSubtree("at the frame's start");
				// The live chain of the first geometry, once (render thread: the tree alive, its membership just synced): where the
				// skeleton hangs.
				static bool chainLogged = false;
				if (!chainLogged)
					for (const auto& node : subtree.nodes)
						if (node.key && node.geometry && !node.hidden) {
							chainLogged = true;
							std::string chain;
							std::uint32_t at = 0;
							for (const RE::NiAVObject* object = node.key; object && at < 24; object = object->parent, ++at)
								chain += fmt::format("{}'{}' {}", at ? " <- " : "", object->name.c_str() ? object->name.c_str() : "", static_cast<const void*>(object));
							logger::info("[DCLF] first-person subtree (T7e): the root {} (parent {}); a geometry's live chain: {}", static_cast<const void*>(subtreeRoot),
								static_cast<const void*>(subtreeRoot->parent), chain);
							break;
						}
				const auto listed = static_cast<std::ptrdiff_t>(g->cullHidden.size());
				g->cullHidden.reserve(g->cullHidden.size() + subtree.nodes.size());
				for (const auto& node : subtree.nodes)
					if (node.key)
						g->cullHidden.emplace_back(node.key, CulledBit(node));
				std::inplace_merge(g->cullHidden.begin(), g->cullHidden.begin() + listed, g->cullHidden.end());
				g->firstPersonGeometry = subtree.geometry;
				NoteFirstPersonDifference();
			}
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
