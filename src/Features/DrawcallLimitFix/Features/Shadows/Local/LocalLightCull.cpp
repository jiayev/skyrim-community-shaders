// LocalLightCull: the point lights' shadow culls without DCLF's entries (LocalLightCull.h describes it): the frame's exclusion and
// category filter (render thread), the Process1 skip and NiNode::OnVisible filter inside the lights' culls, and the lights' bits.
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/FrameAddressSet.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/SunExclusion.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/Portals/StrictGates.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <Tracy/Tracy.hpp>
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/LocalLightCullView.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/StrictLightParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"

namespace DCLF::Features::Shadows::Local::LocalLightCull
{
	const std::array<const char*, kReachKindCount + 1> kReachNames{ "untested", "sphere", "portals", "not reached" };

	namespace
	{
		constexpr std::uintptr_t kParabolicProcess1 = 0x1519a90;  // BSParabolicCullingProcess::Process1, vtable slot 0x16
		constexpr std::uint32_t kParabolicMode = 0xF - PassCapture::kFirstShadowMode;

		bool installed = false;
		// This frame's exclusion (the one the next frame applies waits in Core's ParabolicExclusionSlot), held until the next
		// selection: the culls that read it run between the two on the render thread.
		std::shared_ptr<SunExclusion> frameHeld;
		std::atomic<const SunExclusion*> frameExclusion{ nullptr };
		std::atomic<bool> cullsWhole{ false };  // this frame's culls skip nothing (Policy::FilterLightCulls)

		struct Stats
		{
			std::atomic<std::uint64_t> visited{ 0 }, skipped{ 0 };
			std::uint64_t frames = 0, live = 0, noExclusion = 0, noClaims = 0, stale = 0, masked = 0;
		};
		Stats stats;

		/** @brief The candidate index of a_object when it is an entry the exclusion excludes, else ~0u. */
		std::uint32_t ExcludedIndex(const SunExclusion& a_exclusion, const void* a_object)
		{
			const auto& entries = a_exclusion.candidates->entries;
			const auto it = entries.find(static_cast<const RE::NiAVObject*>(a_object));
			return it != entries.end() && a_exclusion.excluded[it->second] ? it->second : ~0u;
		}
		bool Excluded(const SunExclusion& a_exclusion, const void* a_object) { return ExcludedIndex(a_exclusion, a_object) != ~0u; }

		// ---- The category filter (LocalLightCull.h).
		constexpr std::uintptr_t kNodeOnVisible = 0xd1e2c0;  // NiNode::OnVisible, VTABLE_NiNode slot 0x34
		constexpr std::uintptr_t kCull = 0xd1c570;           // NiAVObject::Cull(object, process, arg)
		constexpr std::uint32_t kAlwaysDraw = 1u << 11;

		bool filterInstalled = false;

		/** @brief One category node's children less its excluded entries, in child order. */
		struct CategoryFilter
		{
			// The node, held: the snapshots key it and a later selection builds it again from here, so it must not be freed (nor its
			// address reused by another node) while a snapshot names it. Released with the snapshot, at Present.
			RE::NiPointer<RE::NiNode> node;
			std::vector<RE::NiPointer<RE::NiAVObject>> children;
			std::uint32_t cut = 0;               // the excluded entries left out
			std::uint32_t empty = 0;             // and the children holding no geometry (a tracked category node's)
			std::atomic<bool> dirty{ false };    // a child attached or detached since: the node is walked natively
			// The point lights whose cull reached the node's children this frame, by reach kind (stamped words: NoteReach).
			mutable std::array<std::atomic<std::uint64_t>, 3> reach{};
		};
		/** @brief A snapshot: built on DCLF's pool for a selection, read by the light culls and the detours until retired. */
		struct Filter
		{
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::shared_ptr<CategoryFilter>> nodes;
			const EntrySpace* candidates = nullptr;
			std::uint64_t exclusionVersion = ~0ull;
			std::uint64_t entriesAppeared = 0;  // FrameView::GetLightEntriesAppeared at the build
		};
		std::shared_ptr<const Filter> filterBuilt;
		std::atomic<const Filter*> filterCurrent{ nullptr };  // the detours' (any thread), the built one
		std::atomic<const Filter*> frameFilter{ nullptr };    // this frame's light culls': null on frames they cull whole
		std::atomic<std::uint64_t> dirtied{ 0 };              // nodes made dirty, ever
		std::uint64_t dirtiedSeen = 0;
		// Retired snapshots, released two Presents later: a detour that loaded one just before it was retired may still read it.
		std::array<std::vector<std::shared_ptr<const Filter>>, 2> retired;
		thread_local const Filter* lightFilter = nullptr;  // set for the call by the Accumulate thunk

		struct FilterStats
		{
			std::atomic<std::uint64_t> lights{ 0 }, filtered{ 0 }, walkedOwn{ 0 }, walkedNotLive{ 0 };
			std::atomic<std::uint64_t> nodes{ 0 }, children{ 0 }, cut{ 0 }, dirtyNodes{ 0 };
			std::uint64_t builds = 0, nodesRebuilt = 0, stale = 0, refused = 0;
			std::int64_t buildTicks = 0;
		};
		FilterStats filterStats;

		// ---- The point lights' bits for the engine's main passes under cut entries (NoteMainRegistration).
		constexpr std::size_t kProcessCullMode = 0x30198;         // BSCullingProcess::cullMode
		constexpr std::size_t kProcessCompoundFrustum = 0x301A0;  // BSCullingProcess::compoundFrustum
		constexpr std::size_t kFrustumFreeOp = 0xBC;              // BSCompoundFrustum::freeOp
		constexpr std::size_t kProcessLightCentre = 0x30218;      // FUN_14151a1e0's sphere: the light's centre, then its radius
		constexpr std::size_t kProcessLightRadius = 0x30224;
		constexpr std::size_t kAccumulatorLightIndex = 0x160;     // the shadow-light count + 1 (0: no mask write, 0xFFFF: a clear)
		constexpr std::size_t kAccumulatorLightBit = 0x164;
		constexpr std::uint32_t kMaxLights = 32;
		constexpr std::uint32_t kNoLight = ~0u;
		// How a light's cull reached a node's children: untested (cull mode 1, under a kAllPass multibound), against its sphere
		// (FUN_14151a1e0), or against that and the portals' compound frustum (mode 4 with one), which DCLF does not test.
		enum ReachKind : std::uint32_t
		{
			kReachAll,
			kReachTested,
			kReachPortal,
			kReachKinds
		};
		static_assert(kReachKinds == kReachKindCount);

		/** @brief A point light of the frame, by Accumulate order: its mask bit, and the sphere its process tested. */
		struct FrameLight
		{
			std::uint32_t bit = 0;
			float centre[3]{};
			float radius = 0.0f;
			bool captured = false;
			// The sphere its cull tests, known before it (FUN_141519d20: the first view's camera position, the light's radius): an empty
			// cull's bits test it, and the parity frames compare it with the one the cull captured.
			float predictedCentre[3]{};
			float predictedRadius = 0.0f;
			bool predicted = false;
			// The light, and whether it is portal-strict: its bits pass its strict gates too (Portals/StrictGates.h), from the selection's
			// context of it (made after its Accumulate, before any main registration).
			const RE::BSShadowLight* light = nullptr;
			bool strict = false;
		};
		std::array<FrameLight, kMaxLights> frameLights;
		std::atomic<std::uint32_t> frameLightNext{ 0 };      // the next light's ordinal (from the selection)
		std::atomic<std::uint32_t> frameLightBits{ 0 };      // every light's bit this frame
		std::atomic<std::uint32_t> lightStamp{ 1 };          // the frame's: the reach words' and the consumed geometries'
		std::atomic<bool> lightsReady{ false };              // a light accumulated since the selection
		thread_local std::uint32_t currentLight = kNoLight;  // the ordinal of the light whose Accumulate runs on this thread

		// ---- The empty culls (LocalLightCull.h): a light whose every paraboloid view DCLF draws is not culled at all.
		constexpr std::size_t kLightPortalStrict = 0x47;    // BSLight::portalStrict: its cull tests the portals' compound frustum
		bool strictCulls = true;                            // CS_DCLF_STRICT_LIGHT_CULLS: portal-strict lights take the empty culls too
		std::atomic<bool> frameCullsEmpty{ false };         // the selection's: the exclusion live and not a parity frame
		std::atomic<std::uint32_t> emptyLights{ 0 };        // the ordinals of the frame's lights culled empty
		FrameAddressSet mainSeen;                           // the geometries whose first clearing main registration this frame was seen
		struct EmptyStats
		{
			std::atomic<std::uint64_t> lights{ 0 }, notCovered{ 0 }, portalStrict{ 0 }, given{ 0 }, withBits{ 0 }, unstamped{ 0 };
			std::atomic<std::uint64_t> spheresChecked{ 0 }, spheresDiffer{ 0 };
			// Portal-strict lights culled empty; their sphere bits tested against their gates, and refused by them; tested without a portal
			// graph (no rooms: nothing to gate, by design); tested without a context otherwise (not selected, or not walked: given, ungated).
			std::atomic<std::uint64_t> strictLights{ 0 }, strictTested{ 0 }, strictRefused{ 0 }, strictNoGraph{ 0 }, strictNoContext{ 0 };
		};
		EmptyStats emptyStats;

		struct BitStats
		{
			std::atomic<std::uint64_t> lights{ 0 }, overflow{ 0 }, given{ 0 }, withBits{ 0 };
		};
		BitStats bitStats;

		/**
		 * @brief Render thread, at the selection (every frame, before Main::Draw): a new frame of lights. Not Main::Draw's mask
		 * clear (FUN_1414cb640), which it calls only while the player's third-person 3D is drawn: in first person the lights
		 * accumulate with no clear before them.
		 */
		void ResetFrameLights()
		{
			lightsReady.store(false, std::memory_order_relaxed);
			frameLightNext.store(0, std::memory_order_relaxed);
			frameLightBits.store(0, std::memory_order_relaxed);
			emptyLights.store(0, std::memory_order_relaxed);
			for (auto& light : frameLights) {
				light.captured = light.predicted = light.strict = false;
				light.light = nullptr;
			}
			// No registration reads it now: the last frame's main registrations ended at its Main::Draw.
			mainSeen.NewRound();
			if (lightStamp.fetch_add(1, std::memory_order_relaxed) + 1 == 0)
				lightStamp.store(1, std::memory_order_relaxed);
		}

		/** @brief Inside a light's cull: it reached the children of a node (a_words: the node's or the entry's reach words). */
		void NoteReach(const RE::NiCullingProcess* a_process, std::atomic<std::uint64_t>* a_words)
		{
			const std::uint32_t light = currentLight;
			if (light == kNoLight || !a_words)
				return;
			const auto mode = Engine::At<std::uint32_t>(a_process, kProcessCullMode);
			if (mode == 2)  // kAllFail
				return;
			const void* frustum = Engine::At<void*>(a_process, kProcessCompoundFrustum);
			const std::uint32_t kind = mode == 1                                                                     ? kReachAll :
			                           frustum && Engine::At<std::uint32_t>(frustum, kFrustumFreeOp) != 0 && mode != 3 ? kReachPortal :
			                                                                                                          kReachTested;
			// The static process is every point light's: its sphere is this light's only during its cull.
			auto& sphere = frameLights[light];
			if (!sphere.captured) {
				std::memcpy(sphere.centre, &Engine::At<float>(a_process, kProcessLightCentre), sizeof(sphere.centre));
				sphere.radius = Engine::At<float>(a_process, kProcessLightRadius);
				sphere.captured = true;
			}
			const std::uint64_t stamp = static_cast<std::uint64_t>(lightStamp.load(std::memory_order_relaxed)) << 32;
			auto& word = a_words[kind];
			std::uint64_t old = word.load(std::memory_order_relaxed);
			for (;;) {
				const std::uint64_t next = ((old & ~0xFFFFFFFFull) == stamp ? old : stamp) | (1ull << light);
				if (next == old || word.compare_exchange_weak(old, next, std::memory_order_relaxed))
					break;
			}
		}

		/** @brief The lights of a reach word, this frame's. */
		std::uint32_t ReachOf(const std::atomic<std::uint64_t>& a_word)
		{
			const std::uint64_t word = a_word.load(std::memory_order_relaxed);
			return (word >> 32) == lightStamp.load(std::memory_order_relaxed) ? static_cast<std::uint32_t>(word) : 0u;
		}

		/** @brief FUN_14151a1e0, a bound against a light's sphere (its half-space test culls nothing inside the sphere). */
		bool InSphere(const FrameLight& a_light, const RE::NiBound& a_bound)
		{
			const float dx = a_bound.center.x - a_light.centre[0];
			const float dy = a_bound.center.y - a_light.centre[1];
			const float dz = a_bound.center.z - a_light.centre[2];
			return (std::sqrt(dx * dx + dy * dy + dz * dz) - a_bound.radius) - a_light.radius < 0.0f;
		}

		/**
		 * @brief A portal-strict light's gates for a geometry in its sphere (Portals/StrictGates.h): its chain's verdict in the light's context and
		 * its bound in the room frustum. Without a portal graph there is nothing to gate; without a context otherwise (not selected, or not
		 * walked this frame) the bit stands, counted apart. Any thread: the selection's frame is not written between its Select and the next
		 * frame's.
		 */
		bool StrictAdmits(const FrameLight& a_light, const RE::BSGeometry& a_geometry)
		{
			if (!a_light.strict)
				return true;
			emptyStats.strictTested.fetch_add(1, std::memory_order_relaxed);
			Portals::ContextHold context;
			std::uint32_t walk = PortalViews::kLightUntouched;
			if (const auto* selection = LightSelection::Get().Current(Scene::FrameView::Get().GetFrame()))
				if (const auto* entry = selection->LightOf(a_light.light)) {
					context = entry->portalContext;
					walk = entry->portalWalk;
				}
			if (!context) {
				(walk == PortalViews::kLightNoGraph ? emptyStats.strictNoGraph : emptyStats.strictNoContext).fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			thread_local Portals::GateChain chain;
			Portals::ChainOf(&a_geometry, chain);
			const float centre[3]{ a_geometry.worldBound.center.x, a_geometry.worldBound.center.y, a_geometry.worldBound.center.z };
			const bool admitted = Portals::Admit(*context, chain, centre, a_geometry.worldBound.radius);
			emptyStats.strictRefused.fetch_add(admitted ? 0 : 1, std::memory_order_relaxed);
			return admitted;
		}

		bool Hidden(const RE::NiAVObject* a_object) { return Engine::IsHidden(a_object); }

		/** @brief A node whose OnVisible is NiNode's own through the vtable: exactly NiNode (the category nodes). */
		bool ExactNiNode(const RE::NiAVObject* a_object)
		{
			static const REL::Relocation<const RE::NiRTTI*> niNode{ RE::NiNode::Ni_RTTI };
			return a_object->GetRTTI() == niNode.get();
		}

		/**
		 * @brief Whether a subtree holds no geometry: nothing a light's registration would take, and no light entry. Under a
		 * tracked category node every geometry attached later is tracked, and its entry appearing builds the filter again.
		 */
		bool HoldsNoGeometry(const RE::NiAVObject* a_object)
		{
			// Past 64 levels it counts as holding one.
			return Engine::ForEachLive(a_object, [](RE::NiAVObject& a_node, std::uint32_t a_depth) { return a_depth <= 64 && !a_node.AsGeometry(); });
		}

		std::shared_ptr<CategoryFilter> BuildCategory(const RE::NiNode& a_node, const SunExclusion& a_exclusion)
		{
			auto filter = std::make_shared<CategoryFilter>();
			filter->node.reset(const_cast<RE::NiNode*>(&a_node));
			const auto& children = a_node.GetChildren();
			const bool tracked = Scene::FrameView::Get().IsCategoryNode(&a_node);
			filter->children.reserve(children.size());
			for (std::uint16_t i = 0; i < children.free_idx(); ++i) {
				auto* child = children[i].get();
				if (!child)
					continue;
				if (Excluded(a_exclusion, child))
					++filter->cut;
				else if (tracked && !Scene::FrameView::Get().IsLightEntry(child) && HoldsNoGeometry(child))
					++filter->empty;
				else
					filter->children.emplace_back(child);
			}
			return filter;
		}

		/** @brief A filter build on the pool and what it was made from; its result goes back to the render thread whole. */
		struct FilterBuild
		{
			std::shared_ptr<SunExclusion> exclusion;  // held for the build, dropped on the render thread
			std::shared_ptr<const Filter> base;       // the nodes a partial build shares; null for a whole one
			std::uint64_t appeared = 0, dirtied = 0;
			bool appearedNow = false;
			std::shared_ptr<const Filter> built;      // null: a lease was refused (the window closed); built again
			std::uint64_t nodesRebuilt = 0;
			std::int64_t ticks = 0;
		};
		std::atomic<FilterBuild*> filterReady{ nullptr };  // the pool's result, taken at the next selection
		bool filterInFlight = false;                        // render thread: a build is on the pool

		/**
		 * @brief The pool: the filter a_build asks for. Whole for a new verdict (every category node that is the parent of an excluded
		 * entry); otherwise the base's nodes, those a detour made dirty built again. A lease per entry or node read.
		 */
		void BuildFilter(FilterBuild& a_build)
		{
			ZoneScopedN("CS.DCLF.LightCullFilter");
			LARGE_INTEGER start{}, end{};
			QueryPerformanceCounter(&start);
			const auto& exclusion = *a_build.exclusion;
			auto next = std::make_shared<Filter>();
			next->candidates = exclusion.candidates.get();
			next->exclusionVersion = exclusion.version;
			next->entriesAppeared = a_build.appeared;
			if (!a_build.base) {
				for (const auto& [entry, index] : exclusion.candidates->entries) {
					if (!exclusion.excluded[index])
						continue;
					const EngineReadWindow::Lease lease;
					if (!lease)
						return;
					// The parent read once: a loader thread may detach the entry meanwhile.
					auto* parent = entry->parent;
					if (!parent || !ExactNiNode(parent) || next->nodes.contains(parent))
						continue;
					next->nodes.emplace(parent, BuildCategory(*parent, exclusion));
					++a_build.nodesRebuilt;
				}
			} else {
				next->nodes = a_build.base->nodes;
				for (auto& [node, category] : next->nodes)
					if (category->dirty.load(std::memory_order_acquire) || (a_build.appearedNow && category->empty)) {
						const EngineReadWindow::Lease lease;
						if (!lease)
							return;
						category = BuildCategory(*category->node, exclusion);
						++a_build.nodesRebuilt;
					}
			}
			a_build.built = std::move(next);
			QueryPerformanceCounter(&end);
			a_build.ticks = end.QuadPart - start.QuadPart;
		}

		/**
		 * @brief Render thread, at the selection: the filter for this exclusion. A finished build is installed if it is still for the
		 * frame's verdict; a new one goes to the pool when the verdict, an entry's appearance or a dirty node moved (one at a time). The
		 * frame's light culls use the installed filter only when it is for the frame's verdict (its dirty nodes walked natively);
		 * otherwise they walk every category node natively until the build lands.
		 */
		void UpdateFilter(const std::shared_ptr<SunExclusion>& a_exclusion)
		{
			if (std::unique_ptr<FilterBuild> done{ filterReady.exchange(nullptr, std::memory_order_acq_rel) }) {
				filterInFlight = false;
				filterStats.nodesRebuilt += done->nodesRebuilt;
				if (!done->built) {
					++filterStats.refused;
				} else if (a_exclusion && done->built->exclusionVersion == a_exclusion->version) {
					dirtiedSeen = done->dirtied;
					if (filterBuilt)
						retired[0].push_back(std::move(filterBuilt));
					filterBuilt = std::move(done->built);
					++filterStats.builds;
					filterStats.buildTicks += done->ticks;
				} else {
					++filterStats.stale;
					retired[0].push_back(std::move(done->built));
				}
				// The exclusion and base (engine references through the snapshot) go here, on the render thread.
				if (done->base)
					retired[0].push_back(std::move(done->base));
			}
			if (!a_exclusion) {
				filterCurrent.store(nullptr, std::memory_order_release);
				if (filterBuilt)
					retired[0].push_back(std::move(filterBuilt));
				return;
			}
			const std::uint64_t appeared = Scene::FrameView::Get().GetLightEntriesAppeared();
			// Whole for a new verdict; for one translated to newer candidates (the same version) the nodes of the entries that moved were
			// made dirty (SelectFrame), and the rest stand.
			const bool current = filterBuilt && filterBuilt->exclusionVersion == a_exclusion->version;
			// An entry appeared: the nodes that cut a child for holding no geometry judge it again (it may hold one now).
			const bool appearedNow = current && filterBuilt->entriesAppeared != appeared;
			const std::uint64_t dirtyNow = dirtied.load(std::memory_order_acquire);
			filterCurrent.store(current ? filterBuilt.get() : nullptr, std::memory_order_release);
			if ((current && !appearedNow && dirtyNow == dirtiedSeen) || filterInFlight)
				return;
			auto build = std::make_unique<FilterBuild>();
			build->exclusion = a_exclusion;
			build->base = current ? filterBuilt : nullptr;
			build->appeared = appeared;
			build->dirtied = dirtyNow;
			build->appearedNow = appearedNow;
			filterInFlight = true;
			if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "CS.DCLF.LightCullFilter",
					[build = build.release()](const auto&) {
						BuildFilter(*build);
						filterReady.store(build, std::memory_order_release);
					}))
				stl::report_and_fail("Drawcall Limit Fix: the point lights' category filter was refused by DCLF's executor");
		}

		/**
		 * @brief NiNode::OnVisible (AE 0x140d1e2c0, VTABLE_NiNode slot 0x34): when the node's bound radius is not 0 or it is
		 * kAlwaysDraw, NiAVObject::Cull on each child (which skips a hidden one, else calls the process's Process1). Inside a
		 * point light's cull, a filtered category node does the same over its children less the excluded entries.
		 */
		struct NodeOnVisible
		{
			static void thunk(RE::NiNode* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				// The frame's filter (built on the frames the culls keep whole too): the light reaching its nodes is what its bits follow.
				if (const auto* filter = currentLight != kNoLight ? filterCurrent.load(std::memory_order_acquire) : nullptr)
					if (const auto it = filter->nodes.find(a_node); it != filter->nodes.end()) {
						const auto& category = *it->second;
						const bool children = a_node->worldBound.radius != 0.0f || (a_node->GetFlags().underlying() & kAlwaysDraw);
						if (children)
							NoteReach(a_process, category.reach.data());
						if (lightFilter == filter && !category.dirty.load(std::memory_order_acquire)) {
							if (children) {
								using Cull = void (*)(RE::NiAVObject*, RE::NiCullingProcess*, std::int32_t);
								static const REL::Relocation<Cull> cull{ REL::Offset(kCull) };
								for (const auto& child : category.children)
									cull(child.get(), a_process, a_arg);
								filterStats.children.fetch_add(category.children.size(), std::memory_order_relaxed);
								filterStats.cut.fetch_add(category.cut + category.empty, std::memory_order_relaxed);
							}
							filterStats.nodes.fetch_add(1, std::memory_order_relaxed);
							return;
						}
						if (lightFilter == filter)
							filterStats.dirtyNodes.fetch_add(1, std::memory_order_relaxed);
					}
				func(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief The point lights' bits for a geometry of a cut entry: per light that reached the entry (its category node's reach,
		 * or its own when Process1 skipped it), the light's bit, untested or when the geometry's bound is in the light's sphere.
		 * a_reach: the lights by reach kind.
		 */
		std::uint32_t BitsOf(const SunExclusion& a_exclusion, std::uint32_t a_entry, const RE::BSGeometry& a_geometry, std::array<std::uint32_t, kReachKinds>& a_reach)
		{
			a_reach = {};
			if (a_exclusion.lightReach)
				for (std::uint32_t k = 0; k < kReachKinds; ++k)
					a_reach[k] = ReachOf(a_exclusion.lightReach[a_entry * kReachKinds + k]);
			const auto& nodes = a_exclusion.candidates->entryNodes;
			const auto* node = a_entry < nodes.size() ? nodes[a_entry] : nullptr;
			if (const auto* filter = node && node->parent ? filterCurrent.load(std::memory_order_acquire) : nullptr)
				if (const auto it = filter->nodes.find(node->parent); it != filter->nodes.end())
					for (std::uint32_t k = 0; k < kReachKinds; ++k)
						a_reach[k] |= ReachOf(it->second->reach[k]);
			std::uint32_t bits = 0;
			for (std::uint32_t lights = a_reach[kReachAll] | a_reach[kReachTested] | a_reach[kReachPortal]; lights; lights &= lights - 1) {
				const auto light = static_cast<std::uint32_t>(std::countr_zero(lights));
				// A portal-strict light reached the entry through its gates: the geometry's own bound still meets its room frustum.
				if (((a_reach[kReachAll] >> light) & 1) || (InSphere(frameLights[light], a_geometry.worldBound) && StrictAdmits(frameLights[light], a_geometry)))
					bits |= frameLights[light].bit;
			}
			return bits;
		}

		/**
		 * @brief Whether a registered geometry hangs from a filtered category node's kept child, when its chain passes through one
		 * (a_through): the observers' check on the frames the culls skip nothing.
		 */
		bool UnderKeptChild(const Filter& a_filter, const RE::NiAVObject* a_object, bool& a_through)
		{
			a_through = false;
			for (const auto* object = a_object; object && object->parent; object = object->parent) {
				const auto it = a_filter.nodes.find(object->parent);
				if (it == a_filter.nodes.end() || it->second->dirty.load(std::memory_order_acquire))
					continue;
				a_through = true;
				const auto& children = it->second->children;
				return std::find_if(children.begin(), children.end(), [&](const auto& a_child) { return a_child.get() == object; }) != children.end();
			}
			return false;
		}

		/**
		 * @brief The bits a_lights (ordinals) give a_geometry: each light's whose predicted sphere its bound is in, a portal-strict light's
		 * through its gates too.
		 */
		std::uint32_t SphereBits(std::uint32_t a_lights, const RE::BSGeometry& a_geometry)
		{
			const auto& bound = a_geometry.worldBound;
			std::uint32_t bits = 0;
			for (; a_lights; a_lights &= a_lights - 1) {
				const auto& light = frameLights[std::countr_zero(a_lights)];
				if (!light.predicted)
					continue;
				FrameLight sphere;
				std::memcpy(sphere.centre, light.predictedCentre, sizeof(sphere.centre));
				sphere.radius = light.predictedRadius;
				if (InSphere(sphere, bound) && StrictAdmits(light, a_geometry))
					bits |= light.bit;
			}
			return bits;
		}

		/** @brief The ordinals of the frame's lights with a predicted sphere and a bit (the parity frames' comparison). */
		std::uint32_t PredictedLights()
		{
			std::uint32_t lights = 0;
			const std::uint32_t count = std::min(frameLightNext.load(std::memory_order_relaxed), kMaxLights);
			for (std::uint32_t light = 0; light < count; ++light)
				lights |= frameLights[light].predicted && frameLights[light].bit ? 1u << light : 0u;
			return lights;
		}

		/** @brief The sphere a light's cull will test (FUN_141519d20, from Accumulate's descriptor: its camera and the light's radius). */
		void Predict(RE::BSShadowLight& a_light, FrameLight& a_frameLight)
		{
			auto& descriptors = a_light.GetRuntimeData().shadowmapDescriptors;
			const auto* camera = descriptors.empty() ? nullptr : descriptors[0].camera.get();
			auto* niLight = a_light.light.get();
			if (!camera || !niLight)
				return;
			a_frameLight.predictedCentre[0] = camera->world.translate.x;
			a_frameLight.predictedCentre[1] = camera->world.translate.y;
			a_frameLight.predictedCentre[2] = camera->world.translate.z;
			a_frameLight.predictedRadius = niLight->GetLightRuntimeData().radius.x;
			a_frameLight.predicted = true;
		}

		/**
		 * @brief Whether a light's Accumulate culls nothing (its views and their casters DCLF's, its cull's bits DCLF's to give): then its
		 * bookkeeping as BSShadowParabolicLight::Accumulate does it - the first accumulator's light index and bit, the shadow-light count
		 * - and no cull, no registration (which would take the accumulation context's lock against the main camera's list jobs).
		 */
		bool CullEmpty(RE::BSShadowLight& a_light, std::uint32_t a_ordinal, std::uint32_t* a_count)
		{
			auto& descriptors = a_light.GetRuntimeData().shadowmapDescriptors;
			// Every view of the light DCLF's this frame and its casters the set's (KeepLight covered them, before this Accumulate): nothing
			// the engine's cull would register is drawn by them.
			std::uint32_t views = 0;
			bool covered = true;
			for (const auto& view : ShadowViews::Get().All()) {
				if (view.light != &a_light || view.focus)
					continue;
				++views;
				covered = covered && view.kind == ShadowViews::Kind::Parabolic && view.covered && PassCapture::Get().CastersForBatch(view.batch);
			}
			auto* accumulator = descriptors.empty() ? nullptr : descriptors[0].shaderAccumulator.get();
			if (!covered || views < descriptors.size() || !accumulator || !frameLights[a_ordinal].predicted) {
				emptyStats.notCovered.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			// Portal-strict: its cull's rooms, shared nodes and room frusta are DCLF's strict gates (Portals/StrictGates.h), on its views'
			// casters (BuildDrawsLatch::strictContext), DCLF's receivers (GpuShadowVolume::portalContext) and the engine's receivers' bits
			// (StrictAdmits). With CS_DCLF_STRICT_LIGHT_CULLS=0 culled as before.
			if (Engine::At<bool>(&a_light, kLightPortalStrict)) {
				if (!strictCulls) {
					emptyStats.portalStrict.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
				emptyStats.strictLights.fetch_add(1, std::memory_order_relaxed);
			}
			const std::uint32_t count = *a_count;
			Engine::At<std::uint32_t>(accumulator, kAccumulatorLightIndex) = count + 1;
			Engine::At<std::uint32_t>(accumulator, kAccumulatorLightBit) = count < 0x1F ? 1u << count : 0u;
			*a_count = count + (a_light.shadowMapCount == 2 ? 2 : 1);
			emptyLights.fetch_or(1u << a_ordinal, std::memory_order_relaxed);
			emptyStats.lights.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		// BSShadowParabolicLight::Accumulate (vtable slot 9, 0x14151b960): the point lights' cull and registration, timed; the
		// frame's category filter applies for the call, or none runs (CullEmpty).
		std::atomic<std::int64_t> accumulateTicks{ 0 };
		std::atomic<std::uint64_t> accumulateCalls{ 0 };
		struct Accumulate
		{
			static void thunk(void* a_light, std::uint32_t* a_count, std::uint32_t* a_arg2, RE::NiAVObject* a_arg3)
			{
				LARGE_INTEGER start{}, end{};
				QueryPerformanceCounter(&start);
				// A light DCLF cannot give its bits to (past the mask's 32) culls everything, as does every light while DCLF is switched off
				// (no selection ran: the frame's exclusion, filter and empty culls are a running frame's).
				std::uint32_t ordinal = kNoLight;
				if (installed && ShellState::Running()) {
					ordinal = frameLightNext.fetch_add(1, std::memory_order_relaxed);
					if (ordinal >= kMaxLights) {
						bitStats.overflow.fetch_add(1, std::memory_order_relaxed);
						ordinal = kNoLight;
					}
				}
				auto& light = *static_cast<RE::BSShadowLight*>(a_light);
				if (ordinal != kNoLight) {
					Predict(light, frameLights[ordinal]);
					frameLights[ordinal].light = &light;
					frameLights[ordinal].strict = Engine::At<bool>(&light, kLightPortalStrict);
				}
				const bool empty = ordinal != kNoLight && !a_arg3 && frameCullsEmpty.load(std::memory_order_relaxed) && CullEmpty(light, ordinal, a_count);
				if (!empty) {
					const Filter* filter = nullptr;
					if (filterInstalled) {
						filterStats.lights.fetch_add(1, std::memory_order_relaxed);
						filter = ordinal != kNoLight ? frameFilter.load(std::memory_order_acquire) : nullptr;
						(filter ? filterStats.filtered : filterStats.walkedNotLive).fetch_add(1, std::memory_order_relaxed);
					}
					currentLight = ordinal;
					lightFilter = filter;
					// CS_DCLF_STRICT_LIGHT_PARITY: a portal-strict light's cull traced where nothing is skipped or filtered.
					const auto frame = Scene::FrameView::Get().GetFrame();
					const bool whole = !filter && (!frameExclusion.load(std::memory_order_acquire) || cullsWhole.load(std::memory_order_relaxed));
					const bool traced = StrictLightParity::Begin(light, whole, frame);
					func(a_light, a_count, a_arg2, a_arg3);
					if (traced)
						StrictLightParity::End(light, a_arg3, frame);
					lightFilter = nullptr;
					currentLight = kNoLight;
				}
				if (ordinal != kNoLight) {
					// The parity frames: the sphere the cull tested (captured where it reached a node or entry DCLF notes) is the predicted one.
					if (const auto& frameLight = frameLights[ordinal]; cullsWhole.load(std::memory_order_relaxed) && frameLight.captured && frameLight.predicted) {
						bool same = std::abs(frameLight.radius - frameLight.predictedRadius) <= 1e-3f;
						for (std::uint32_t i = 0; i < 3; ++i)
							same = same && std::abs(frameLight.centre[i] - frameLight.predictedCentre[i]) <= 1e-2f;
						emptyStats.spheresChecked.fetch_add(1, std::memory_order_relaxed);
						emptyStats.spheresDiffer.fetch_add(same ? 0 : 1, std::memory_order_relaxed);
					}
					// The bit its registration ORed into every geometry it reached (FUN_1414b2140: the accumulator's +0x164).
					std::uint32_t bit = 0;
					for (const auto& descriptor : static_cast<RE::BSShadowLight*>(a_light)->GetRuntimeData().shadowmapDescriptors)
						if (const auto* accumulator = descriptor.shaderAccumulator.get()) {
							const auto index = Engine::At<std::uint32_t>(accumulator, kAccumulatorLightIndex);
							if (index != 0 && index != 0xFFFF)
								bit |= Engine::At<std::uint32_t>(accumulator, kAccumulatorLightBit);
						}
					frameLights[ordinal].bit = bit;
					frameLightBits.fetch_or(bit, std::memory_order_relaxed);
					bitStats.lights.fetch_add(1, std::memory_order_relaxed);
					lightsReady.store(true, std::memory_order_release);
				}
				QueryPerformanceCounter(&end);
				accumulateTicks.fetch_add(end.QuadPart - start.QuadPart, std::memory_order_relaxed);
				accumulateCalls.fetch_add(1, std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Process1
		{
			static void thunk(RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg)
			{
				if (a_object && currentLight != kNoLight)
					if (const auto* exclusion = frameExclusion.load(std::memory_order_acquire)) {
						stats.visited.fetch_add(1, std::memory_order_relaxed);
						if (const std::uint32_t index = ExcludedIndex(*exclusion, a_object); index != ~0u) {
							// The light reached it, in the state its geometries' tests would have run in.
							if (exclusion->lightReach)
								NoteReach(a_process, &exclusion->lightReach[index * kReachKinds]);
							if (!cullsWhole.load(std::memory_order_relaxed)) {
								stats.skipped.fetch_add(1, std::memory_order_relaxed);
								return;
							}
						}
					}
				func(a_process, a_object, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		static bool timed = false;
		if (!timed) {
			// Timed whether or not the skip is on: the A/B's measure.
			stl::write_vfunc<0x9, Accumulate>(RE::VTABLE_BSShadowParabolicLight[0]);
			timed = true;
		}
		if (installed)
			return;
		// A cut entry's geometries take the lights' bits from the Sun source's registration and mask-clear hooks (Features/Shadows/Sun).
		if (!Extensions::SourceCullsInstalled(Api::ShadowSource::Sun)) {
			logger::warn("[DCLF] point lights' shadow culls: the sun's accumulation hooks are not installed; they keep culling everything");
			return;
		}
		const auto base = REL::Module::get().base();
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSParabolicCullingProcess[0] };
		if (reinterpret_cast<const std::uintptr_t*>(vtable.address())[0x16] != base + kParabolicProcess1) {
			logger::warn("[DCLF] point lights' shadow culls: BSParabolicCullingProcess::Process1 is not the engine's; they keep culling everything");
			return;
		}
		stl::write_vfunc<0x16, Process1>(RE::VTABLE_BSParabolicCullingProcess[0]);
		installed = true;
		{
			REL::Relocation<std::uintptr_t> nodeVtable{ RE::VTABLE_NiNode[0] };
			if (reinterpret_cast<const std::uintptr_t*>(nodeVtable.address())[0x34] != base + kNodeOnVisible) {
				logger::warn("[DCLF] point lights' shadow culls: NiNode::OnVisible is not the engine's; the category nodes are walked whole");
			} else {
				stl::write_vfunc<0x34, NodeOnVisible>(RE::VTABLE_NiNode[0]);
				filterInstalled = true;
			}
		}
		logger::info("[DCLF] point lights' shadow culls without DCLF's entries{}", filterInstalled ? ", their category nodes filtered" : "");
	}

	bool Installed() { return installed; }

	void SetStrictCulls(bool a_on) { strictCulls = a_on; }

	void SelectFrame(std::uint32_t a_frame, std::shared_ptr<SunExclusion> a_pending)
	{
		if (!installed)
			return;
		// The switches are read after the sources' hooks: the parity's trace goes in at the first frame, before any light's cull.
		StrictLightParity::Install();
		ResetFrameLights();
		++stats.frames;
		// The culls of the frame read this one; the previous frame's culls are over (they ran before its shadow epoch).
		std::shared_ptr<SunExclusion> next = std::move(a_pending);
		if (!next || !next->candidates)
			++stats.noExclusion, next.reset();
		else if (!PassCapture::Get().CastersWithheld(kSetCasterPoint))
			++stats.noClaims, next.reset();  // the registration withholds nothing of the mode: the engine draws its casters
		else if (const auto candidates = Scene::FrameView::Get().GetLightCandidates(); !candidates)
			++stats.stale, next.reset();  // no candidates this frame (a load, a reset)
		else if (next->candidates != candidates) {
			// A verdict on the frame's candidates, as the builder translated it (ForwardExclusion): the entries changed since its snapshot
			// are the engine's (the lights cull them natively), the rest stand. The filter's nodes holding a changed entry are walked
			// natively until they are built again (dirty: UpdateFilter); the version is kept, so the rest of the filter stands. None made
			// for these candidates: no exclusion this frame.
			std::vector<std::uint32_t> changed;
			auto translated = ForwardExclusion(next, candidates, &changed);
			if (!translated)
				++stats.stale;
			else if (filterInstalled && filterBuilt)
				for (const std::uint32_t e : changed)
					for (const auto* snapshot : { next->candidates.get(), candidates.get() }) {
						// The parent as a key only (the entry is held by its snapshot; its parent may be detached meanwhile).
						const auto* root = e < snapshot->Capacity() ? snapshot->entryNodes[e] : nullptr;
						if (const auto node = root ? filterBuilt->nodes.find(root->parent) : filterBuilt->nodes.end();
							node != filterBuilt->nodes.end() && !node->second->dirty.exchange(true, std::memory_order_acq_rel))
							dirtied.fetch_add(1, std::memory_order_release);
					}
			if (translated)
				++stats.masked;
			next = std::move(translated);
		}
		const bool whole = next && !Policy::FilterLightCulls(a_frame);
		cullsWhole.store(whole, std::memory_order_relaxed);
		// The empty culls with the exclusion live (the mode's casters withheld, its candidates current) and not on a parity frame.
		frameCullsEmpty.store(next && !whole, std::memory_order_relaxed);
		Diagnostics::Notify(Diagnostics::LightCullsSelected{ whole });
		stats.live += next ? 1 : 0;
		frameExclusion.store(next.get(), std::memory_order_release);
		if (filterInstalled) {
			UpdateFilter(next);
			frameFilter.store(next && !whole ? filterCurrent.load(std::memory_order_relaxed) : nullptr, std::memory_order_release);
		}
		frameHeld = std::move(next);
	}

	void NoteStructure(const void* a_node)
	{
		// A child of a filtered category node attached or detached: the node is walked natively until the next selection.
		const auto* a_parent = static_cast<const RE::NiNode*>(a_node);
		const auto* filter = filterInstalled && a_parent ? filterCurrent.load(std::memory_order_acquire) : nullptr;
		if (!filter)
			return;
		if (const auto it = filter->nodes.find(a_parent); it != filter->nodes.end() && !it->second->dirty.exchange(true, std::memory_order_acq_rel))
			dirtied.fetch_add(1, std::memory_order_release);
	}

	void NoteMainRegistration(RE::BSGeometry* a_geometry)
	{
		if (!installed || !a_geometry || !lightsReady.load(std::memory_order_acquire) || !ShellState::Running())
			return;
		const auto* exclusion = frameExclusion.load(std::memory_order_acquire);
		const std::uint32_t empty = emptyLights.load(std::memory_order_relaxed);
		if (!exclusion && !empty)
			return;
		const auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
		void* lightData = property ? Engine::At<void*>(property, Engine::kPropertyLightData) : nullptr;
		if (!lightData)
			return;
		const bool whole = cullsWhole.load(std::memory_order_relaxed);
		// The empty culls' lights reached nothing: at a geometry's first clearing main registration of the frame (the later ones read the
		// cleared mask) it takes the bit of each whose sphere it is in, as their culls' registration would have ORed it. The parity frames
		// (none culled empty) compute the same over every light, for the observer to compare with the engine's.
		bool first = false;
		std::uint32_t sphereBits = 0;
		if (empty || whole) {
			const auto found = mainSeen.Test(a_geometry, true);
			if (found == FrameAddressSet::Found::kFull)
				emptyStats.unstamped.fetch_add(1, std::memory_order_relaxed);
			first = found != FrameAddressSet::Found::kYes;
			if (first)
				sphereBits = SphereBits(empty ? empty : PredictedLights(), *a_geometry);
			if (first && empty) {
				Engine::At<std::uint32_t>(lightData, Engine::kLightDataActiveMask) |= sphereBits;
				emptyStats.given.fetch_add(1, std::memory_order_relaxed);
				emptyStats.withBits.fetch_add(sphereBits ? 1 : 0, std::memory_order_relaxed);
			}
		}
		if (!exclusion)
			return;
		const auto& candidates = *exclusion->candidates;
		std::uint32_t entry = ~0u;
		if (const auto it = candidates.geometries.find(a_geometry); it != candidates.geometries.end() && exclusion->excluded[candidates.geometryEntry[it->second]]) {
			// Once a frame: the first main registration reads the mask and clears it, and the later ones read 0 (engine notes).
			const std::uint32_t stamp = lightStamp.load(std::memory_order_relaxed);
			if (exclusion->cleared[it->second].exchange(stamp, std::memory_order_relaxed) == stamp)
				return;
			entry = candidates.geometryEntry[it->second];
		}
		if (!whole && entry == ~0u)
			return;
		std::array<std::uint32_t, kReachKinds> reach{};
		const std::uint32_t bits = entry != ~0u ? BitsOf(*exclusion, entry, *a_geometry, reach) : 0u;
		auto& mask = Engine::At<std::uint32_t>(lightData, Engine::kLightDataActiveMask);
		if (!whole) {
			mask |= bits;
			bitStats.given.fetch_add(1, std::memory_order_relaxed);
			bitStats.withBits.fetch_add(bits ? 1 : 0, std::memory_order_relaxed);
			return;
		}
		Diagnostics::Notify(Diagnostics::PointLightBits{ a_geometry, entry, bits, reach, mask & frameLightBits.load(std::memory_order_relaxed), first, sphereBits });
	}

	void EndFrame()
	{
		retired[1].clear();
		std::swap(retired[0], retired[1]);
	}

	Counters Take()
	{
		Counters c;
		c.accumulateCalls = accumulateCalls.exchange(0);
		c.accumulateTicks = accumulateTicks.exchange(0);
		c.installed = installed;
		if (!installed)
			return c;
		auto& s = stats;
		c.frames = s.frames, c.live = s.live, c.noExclusion = s.noExclusion, c.noClaims = s.noClaims, c.stale = s.stale, c.masked = s.masked;
		c.visited = s.visited.exchange(0), c.skipped = s.skipped.exchange(0);
		s.frames = s.live = s.noExclusion = s.noClaims = s.stale = s.masked = 0;
		c.filterInstalled = filterInstalled;
		if (filterInstalled) {
			auto& f = filterStats;
			c.nodes = f.nodes.exchange(0);
			c.lights = f.lights.exchange(0), c.filtered = f.filtered.load(), c.walkedNotLive = f.walkedNotLive.exchange(0);
			c.filterNodes = filterBuilt ? filterBuilt->nodes.size() : 0;
			c.children = f.children.exchange(0), c.cut = f.cut.exchange(0), c.dirtyNodes = f.dirtyNodes.exchange(0);
			c.builds = f.builds, c.nodesRebuilt = f.nodesRebuilt, c.buildTicks = f.buildTicks;
			f.filtered = 0;
			c.buildsStale = f.stale, c.buildsRefused = f.refused;
			f.builds = f.nodesRebuilt = f.stale = f.refused = 0;
			f.buildTicks = 0;
		}
		auto& b = bitStats;
		c.bitLights = b.lights.exchange(0), c.overflow = b.overflow.exchange(0), c.given = b.given.exchange(0), c.withBits = b.withBits.exchange(0);
		auto& e = emptyStats;
		c.emptyLights = e.lights.exchange(0), c.emptyNotCovered = e.notCovered.exchange(0), c.emptyPortalStrict = e.portalStrict.exchange(0);
		c.emptyGiven = e.given.exchange(0), c.emptyWithBits = e.withBits.exchange(0), c.emptyUnstamped = e.unstamped.exchange(0);
		c.spheresChecked = e.spheresChecked.exchange(0), c.spheresDiffer = e.spheresDiffer.exchange(0);
		c.strictLights = e.strictLights.exchange(0), c.strictTested = e.strictTested.exchange(0), c.strictRefused = e.strictRefused.exchange(0);
		c.strictNoGraph = e.strictNoGraph.exchange(0), c.strictNoContext = e.strictNoContext.exchange(0);
		return c;
	}
}

namespace DCLF::Features::Shadows::Local
{
	using namespace LocalLightCull;

	bool LocalLightCullView::Installed() { return installed; }
	bool LocalLightCullView::CullsWhole() { return cullsWhole.load(std::memory_order_relaxed); }
	const SunExclusion* LocalLightCullView::FrameExclusion() { return frameExclusion.load(std::memory_order_acquire); }
	bool LocalLightCullView::Excluded(const SunExclusion& a_exclusion, const void* a_object) { return LocalLightCull::Excluded(a_exclusion, a_object); }

	bool LocalLightCullView::UnderKeptChild(const RE::NiAVObject* a_object, bool& a_through)
	{
		a_through = false;
		const auto* filter = filterInstalled ? filterCurrent.load(std::memory_order_acquire) : nullptr;
		return filter && LocalLightCull::UnderKeptChild(*filter, a_object, a_through);
	}

	bool LocalLightCullView::FilterLive() { return filterInstalled && filterCurrent.load(std::memory_order_acquire); }
	bool LocalLightCullView::ParaboloidRenderer(const RE::BSBatchRenderer* a_batch)
	{
		std::uint32_t mode = 0;
		return PassCapture::Get().ShadowModeOfBatch(a_batch, mode) && mode == kParabolicMode;
	}

	std::uint32_t LocalLightCullView::FrameLights() { return std::min(frameLightNext.load(std::memory_order_relaxed), kMaxLights); }

	LocalLightCullView::Light LocalLightCullView::FrameLight(std::uint32_t a_index)
	{
		const auto& light = frameLights[a_index];
		return { light.bit, { light.centre[0], light.centre[1], light.centre[2] }, light.radius };
	}
}
