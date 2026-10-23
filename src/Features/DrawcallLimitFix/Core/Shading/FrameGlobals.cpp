// FrameGlobals.h's capture (render thread, the frame's start), the first-person subtree's cull bits (announced for its feature's
// diagnostics: Diagnostics::RootSubtreeSynced) and the scoped reads.

#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"

#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/RootViewEvents.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
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

		// Module offsets, AE 1.6.1170 and SE 1.5.97 (the readers' notes name each).
		constexpr Engine::Offset kTechniqueByte12{ 0x2032fdb, 0x1e0de39 };
		constexpr Engine::Offset kTechniqueByte7{ 0x2035500, 0x1e102a0 };
		constexpr Engine::Offset kShadowGlobal{ 0x2033498, 0x1e0e2e8 };
		constexpr std::size_t kAccumulatorDeferredShadow = 0x178;
		constexpr Engine::Offset kLodRadiusBase{ 0x2032e40, 0x1e0dca0 };
		constexpr Engine::Offset kLodExponentScale{ 0x2032e54, 0x1e0dcb4 };
		constexpr Engine::Offset kLodPowBase{ 0x2032e44, 0x1e0dca4 };

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

		// One node of the first-person subtree (FrameGlobals.h).
		using SubtreeNode = RootSubtreeNode;

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
			std::uint64_t changes = 0;                    // bits or membership changed (the diagnostics' trigger: RootSubtreeSynced)
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

		/** @brief The subtree at an engine moment, for the root view's diagnostics (Features/FirstPerson's: its parity, chain and differences). */
		void NoteSubtreeSynced(const char* a_moment, bool a_frameStart)
		{
			Diagnostics::Notify(Diagnostics::RootSubtreeSynced{ a_moment, a_frameStart, subtree.root, subtree.nodes, subtree.culled, subtree.walks, subtree.changes });
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
		if (a_root)
			NoteSubtreeSynced("at the culls", false);
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
		Extensions::CaptureFrameGlobals(g->features);
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
			// The features' nodes whose bit the engine changes around the views (Api::CullHiddenSource: Features/Reflection's player 3D and
			// water shapes, Features/FirstPerson's skeleton), sorted with the rest below.
			Extensions::CollectCullHidden(g->cullHidden);
			// A CategoryRoot view's root while its toggle is on, as its feature names it (Api::ViewContribution::captureRoot): the first
			// with a root. The subtree machinery below holds one root (the first person's, the one CategoryRoot view today).
			const RE::NiAVObject* subtreeRoot = nullptr;
			const auto& views = Extensions::Views();
			for (std::uint32_t i = 0; i < views.rootCount && !subtreeRoot; ++i) {
				const auto view = views.rootViews[i];
				if (!Extensions::ViewOn(SceneToggles(), view))
					continue;
				Api::ViewRootCapture root;
				Extensions::ViewEntryOf(view).captureRoot(root);
				subtreeRoot = root.root;
			}
			std::sort(g->cullHidden.begin(), g->cullHidden.end());
			// CS_DCLF_FIRST_PERSON: every node and geometry under the first-person root, with the bit the engine's first-person culls saw
			// last frame (NoteFirstPersonCulled), else as it is now (this thread, before Main::Draw touches the skeleton). The frame start's
			// bits are not the culls' (the arms are hidden at the frame's start while the engine draws them), so the first-person members'
			// chains read their bits from here. Kept by the subtree's events (SyncSubtree), not walked; merged in key order.
			SyncSubtree(subtreeRoot);
			if (subtreeRoot) {
				const auto listed = static_cast<std::ptrdiff_t>(g->cullHidden.size());
				g->cullHidden.reserve(g->cullHidden.size() + subtree.nodes.size());
				for (const auto& node : subtree.nodes)
					if (node.key)
						g->cullHidden.emplace_back(node.key, CulledBit(node));
				std::inplace_merge(g->cullHidden.begin(), g->cullHidden.begin() + listed, g->cullHidden.end());
				g->firstPersonGeometry = subtree.geometry;
				// The root view's diagnostics (render thread: the tree alive, its membership just synced; the merge above reads nothing
				// they log).
				NoteSubtreeSynced("at the frame's start", true);
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
