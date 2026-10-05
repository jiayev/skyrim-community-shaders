#include "LocalLightCull.h"
#include "Features/DrawcallLimitFix/Common/FrameTrace.h"

#include "EngineAccess.h"
#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Common/KeptState.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "PassCapture.h"
#include "PrimaryCull.h"
#include "SunAccumulation.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <mutex>

// The point lights' shadow culls without DCLF's entries: LocalLightCull.h describes it.

namespace DCLF::LocalLightCull
{
	namespace
	{
		constexpr std::uintptr_t kParabolicProcess1 = 0x1519a90;  // BSParabolicCullingProcess::Process1, vtable slot 0x16
		constexpr std::uint32_t kParabolicMode = 0xF - PassCapture::kFirstShadowMode;

		bool installed = false;
		bool probe = false;
		// The exclusion the next frame applies (render thread), and this frame's, held until the next selection: the culls
		// that read it run between the two on the render thread.
		std::shared_ptr<SunExclusion> pending;
		std::shared_ptr<SunExclusion> frameHeld;
		std::atomic<const SunExclusion*> frameExclusion{ nullptr };
		std::atomic<bool> parityFrame{ false };
		std::mutex firstMutex;  // the parities' first lost pass and first missed caster
		std::string firstLost;
		std::array<std::uint64_t, 16> lostByReject{};

		struct Stats
		{
			std::atomic<std::uint64_t> visited{ 0 }, skipped{ 0 }, wouldSkip{ 0 }, parityPasses{ 0 }, parityLost{ 0 };
			std::uint64_t frames = 0, live = 0, noExclusion = 0, noClaims = 0, stale = 0, parityFrames = 0;
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
			std::vector<RE::NiPointer<RE::NiAVObject>> children;
			std::uint32_t cut = 0;               // the excluded entries left out
			std::uint32_t empty = 0;             // and the children holding no geometry (a tracked category node's)
			std::atomic<bool> dirty{ false };    // a child attached or detached since: the node is walked natively
			// The point lights whose cull reached the node's children this frame, by reach kind (stamped words: NoteReach).
			mutable std::array<std::atomic<std::uint64_t>, 3> reach{};
		};
		/** @brief A snapshot: built on the render thread at a selection, read by the light culls and the detours until retired. */
		struct Filter
		{
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::shared_ptr<CategoryFilter>> nodes;
			const SunCandidates* candidates = nullptr;
			std::uint64_t exclusionVersion = ~0ull;
			std::uint64_t entriesAppeared = 0;  // SceneStore::GetLightEntriesAppeared at the build
		};
		std::shared_ptr<const Filter> filterBuilt;
		std::atomic<const Filter*> filterCurrent{ nullptr };  // the detours' (any thread), the built one
		std::atomic<const Filter*> frameFilter{ nullptr };    // this frame's light culls': null on parity frames
		std::atomic<std::uint64_t> dirtied{ 0 };              // nodes made dirty, ever
		std::uint64_t dirtiedSeen = 0;
		// Retired snapshots, released two Presents later: a detour that loaded one just before it was retired may still read it.
		std::array<std::vector<std::shared_ptr<const Filter>>, 2> retired;
		thread_local const Filter* lightFilter = nullptr;  // set for the call by the Accumulate thunk

		struct FilterStats
		{
			std::atomic<std::uint64_t> lights{ 0 }, filtered{ 0 }, walkedOwn{ 0 }, walkedNotLive{ 0 };
			std::atomic<std::uint64_t> nodes{ 0 }, children{ 0 }, cut{ 0 }, dirtyNodes{ 0 };
			std::atomic<std::uint64_t> parityChecked{ 0 }, parityMissed{ 0 };
			std::uint64_t builds = 0, nodesRebuilt = 0;
			std::int64_t buildTicks = 0;
		};
		FilterStats filterStats;
		std::string filterFirstMissed;  // under firstMutex

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
		constexpr std::array<const char*, kReachKinds + 1> kReachNames{ "untested", "sphere", "portals", "not reached" };

		/** @brief A point light of the frame, by Accumulate order: its mask bit, and the sphere its process tested. */
		struct FrameLight
		{
			std::uint32_t bit = 0;
			float centre[3]{};
			float radius = 0.0f;
			bool captured = false;
		};
		std::array<FrameLight, kMaxLights> frameLights;
		std::atomic<std::uint32_t> frameLightNext{ 0 };      // the next light's ordinal (from the selection)
		std::atomic<std::uint32_t> frameLightBits{ 0 };      // every light's bit this frame
		std::atomic<std::uint32_t> lightStamp{ 1 };          // the frame's: the reach words' and the consumed geometries'
		std::atomic<bool> lightsReady{ false };              // a light accumulated since the selection
		thread_local std::uint32_t currentLight = kNoLight;  // the ordinal of the light whose Accumulate runs on this thread

		struct BitStats
		{
			std::atomic<std::uint64_t> lights{ 0 }, overflow{ 0 }, given{ 0 }, withBits{ 0 };
			std::atomic<std::uint64_t> compared{ 0 }, agree{ 0 }, engineOnly{ 0 }, dclfOnly{ 0 }, unhandled{ 0 };
			// By the reach of the light that differs (the last: it did not reach the entry).
			std::array<std::atomic<std::uint64_t>, kReachKinds + 1> engineOnlyBy{}, dclfOnlyBy{};
		};
		BitStats bitStats;
		std::string bitsFirst;  // under firstMutex
		std::uint32_t bitsLogged = 0;

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
			for (auto& light : frameLights)
				light.captured = false;
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

		bool Hidden(const RE::NiAVObject* a_object) { return a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden); }

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
		bool HoldsNoGeometry(const RE::NiAVObject* a_object, std::uint32_t a_depth = 0)
		{
			if (!a_object)
				return true;
			if (a_depth > 64 || const_cast<RE::NiAVObject*>(a_object)->AsGeometry())
				return false;
			if (const auto* node = const_cast<RE::NiAVObject*>(a_object)->AsNode())
				for (const auto& child : node->GetChildren())
					if (!HoldsNoGeometry(child.get(), a_depth + 1))
						return false;
			return true;
		}

		std::shared_ptr<CategoryFilter> BuildCategory(const RE::NiNode& a_node, const SunExclusion& a_exclusion)
		{
			auto filter = std::make_shared<CategoryFilter>();
			const auto& children = a_node.GetChildren();
			const bool tracked = SceneStore::Get().IsCategoryNode(&a_node);
			filter->children.reserve(children.size());
			for (std::uint16_t i = 0; i < children.free_idx(); ++i) {
				auto* child = children[i].get();
				if (!child)
					continue;
				if (Excluded(a_exclusion, child))
					++filter->cut;
				else if (tracked && !SceneStore::Get().IsLightEntry(child) && HoldsNoGeometry(child))
					++filter->empty;
				else
					filter->children.emplace_back(child);
			}
			return filter;
		}

		/**
		 * @brief Render thread, at the selection: the filter for this exclusion. Built whole for a new exclusion (every category
		 * node that is the parent of an excluded entry); otherwise only the nodes a detour made dirty are built again.
		 */
		void UpdateFilter(const SunExclusion* a_exclusion)
		{
			if (!a_exclusion) {
				filterCurrent.store(nullptr, std::memory_order_release);
				if (filterBuilt)
					retired[0].push_back(std::move(filterBuilt));
				return;
			}
			const std::uint64_t appeared = SceneStore::Get().GetLightEntriesAppeared();
			const bool whole = !filterBuilt || filterBuilt->candidates != a_exclusion->candidates.get() || filterBuilt->exclusionVersion != a_exclusion->version;
			// An entry appeared: the nodes that cut a child for holding no geometry judge it again (it may hold one now).
			const bool appearedNow = filterBuilt && filterBuilt->entriesAppeared != appeared;
			const std::uint64_t dirtyNow = dirtied.load(std::memory_order_acquire);
			if (!whole && !appearedNow && dirtyNow == dirtiedSeen)
				return;
			LARGE_INTEGER start{}, end{};
			QueryPerformanceCounter(&start);
			auto next = std::make_shared<Filter>();
			next->candidates = a_exclusion->candidates.get();
			next->exclusionVersion = a_exclusion->version;
			next->entriesAppeared = appeared;
			if (whole) {
				for (const auto& [entry, index] : a_exclusion->candidates->entries) {
					if (!a_exclusion->excluded[index] || !entry->parent || !ExactNiNode(entry->parent))
						continue;
					const RE::NiAVObject* parent = entry->parent;
					if (!next->nodes.contains(parent)) {
						next->nodes.emplace(parent, BuildCategory(*entry->parent, *a_exclusion));
						++filterStats.nodesRebuilt;
					}
				}
			} else {
				// The nodes shared with the last snapshot, the dirty ones built again.
				next->nodes = filterBuilt->nodes;
				for (auto& [node, category] : next->nodes)
					if (category->dirty.load(std::memory_order_acquire) || (appearedNow && category->empty)) {
						category = BuildCategory(*static_cast<const RE::NiNode*>(node), *a_exclusion);
						++filterStats.nodesRebuilt;
					}
			}
			dirtiedSeen = dirtyNow;
			if (filterBuilt)
				retired[0].push_back(std::move(filterBuilt));
			filterBuilt = std::move(next);
			filterCurrent.store(filterBuilt.get(), std::memory_order_release);
			QueryPerformanceCounter(&end);
			++filterStats.builds;
			filterStats.buildTicks += end.QuadPart - start.QuadPart;
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
				DCLF_FRAME_TRACE("LocalLightCull.cpp:316");  // TEMP frame trace
				// The frame's filter (built on parity frames too): the light reaching its nodes is what its bits follow.
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
				if (((a_reach[kReachAll] >> light) & 1) || InSphere(frameLights[light], a_geometry.worldBound))
					bits |= frameLights[light].bit;
			}
			return bits;
		}

		/**
		 * @brief Parity frames (nothing filtered): whether a registered geometry hangs from a filtered category node's kept child,
		 * when its chain passes through one (a_through).
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

		// BSShadowParabolicLight::Accumulate (vtable slot 9, 0x14151b960): the point lights' cull and registration, timed; the
		// frame's category filter applies for the call.
		std::atomic<std::int64_t> accumulateTicks{ 0 };
		std::atomic<std::uint64_t> accumulateCalls{ 0 };
		struct Accumulate
		{
			static void thunk(void* a_light, std::uint32_t* a_count, std::uint32_t* a_arg2, RE::NiAVObject* a_arg3)
			{
				DCLF_FRAME_TRACE("LocalLightCull.cpp:395");  // TEMP frame trace
				LARGE_INTEGER start{}, end{};
				QueryPerformanceCounter(&start);
				// A light DCLF cannot give its bits to (past the mask's 32) culls everything.
				std::uint32_t ordinal = kNoLight;
				if (installed) {
					ordinal = frameLightNext.fetch_add(1, std::memory_order_relaxed);
					if (ordinal >= kMaxLights) {
						bitStats.overflow.fetch_add(1, std::memory_order_relaxed);
						ordinal = kNoLight;
					}
				}
				const Filter* filter = nullptr;
				if (filterInstalled) {
					filterStats.lights.fetch_add(1, std::memory_order_relaxed);
					filter = ordinal != kNoLight ? frameFilter.load(std::memory_order_acquire) : nullptr;
					(filter ? filterStats.filtered : filterStats.walkedNotLive).fetch_add(1, std::memory_order_relaxed);
				}
				currentLight = ordinal;
				lightFilter = filter;
				func(a_light, a_count, a_arg2, a_arg3);
				lightFilter = nullptr;
				currentLight = kNoLight;
				if (ordinal != kNoLight) {
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
				DCLF_FRAME_TRACE("LocalLightCull.cpp:442");  // TEMP frame trace
				if (a_object && currentLight != kNoLight)
					if (const auto* exclusion = frameExclusion.load(std::memory_order_acquire)) {
						stats.visited.fetch_add(1, std::memory_order_relaxed);
						if (const std::uint32_t index = ExcludedIndex(*exclusion, a_object); index != ~0u) {
							// The light reached it, in the state its geometries' tests would have run in.
							if (exclusion->lightReach)
								NoteReach(a_process, &exclusion->lightReach[index * kReachKinds]);
							if (!probe && !parityFrame.load(std::memory_order_relaxed)) {
								stats.skipped.fetch_add(1, std::memory_order_relaxed);
								return;
							}
							stats.wouldSkip.fetch_add(1, std::memory_order_relaxed);
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
		if (installed || SwitchValue(Switch::LightExclude) == "0")
			return;
		// A cut entry's geometries take the lights' bits from SunAccumulation's registration and mask-clear hooks.
		if (!SunAccumulation::Get().Installed()) {
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
		probe = SwitchValue(Switch::LightExclude) == "probe";
		installed = true;
		if (!probe && SwitchValue(Switch::LightList) != "0") {
			REL::Relocation<std::uintptr_t> nodeVtable{ RE::VTABLE_NiNode[0] };
			if (reinterpret_cast<const std::uintptr_t*>(nodeVtable.address())[0x34] != base + kNodeOnVisible) {
				logger::warn("[DCLF] point lights' shadow culls: NiNode::OnVisible is not the engine's; the category nodes are walked whole");
			} else {
				stl::write_vfunc<0x34, NodeOnVisible>(RE::VTABLE_NiNode[0]);
				filterInstalled = true;
			}
		}
		logger::info("[DCLF] point lights' shadow culls without DCLF's entries{}{}", probe ? " (probe: nothing skipped)" : "",
			filterInstalled ? ", their category nodes filtered" : "");
	}

	void Publish(std::shared_ptr<SunExclusion> a_exclusion)
	{
		if (installed)
			pending = std::move(a_exclusion);
	}

	void SelectFrame(std::uint32_t a_frame)
	{
		if (!installed)
			return;
		ResetFrameLights();
		++stats.frames;
		// The culls of the frame read this one; the previous frame's culls are over (they ran before its shadow epoch).
		std::shared_ptr<SunExclusion> next = pending;
		if (!next || !next->candidates)
			++stats.noExclusion, next.reset();
		else if (!PassCapture::Get().CastersWithheld(kSetCasterPoint))
			++stats.noClaims, next.reset();  // the registration withholds nothing of the mode: the engine draws its casters
		else if (next->candidates->generation != SceneStore::Get().GetLightCandidatesGeneration())
			++stats.stale, next.reset();  // built for other candidates: an entry may hold a caster no epoch has drawn yet
		const bool parity = next && SwitchEnabled(Switch::PersistentParity) && ParityDue(a_frame);
		parityFrame.store(parity, std::memory_order_relaxed);
		stats.parityFrames += parity ? 1 : 0;
		stats.live += next ? 1 : 0;
		frameExclusion.store(next.get(), std::memory_order_release);
		if (filterInstalled) {
			UpdateFilter(next.get());
			frameFilter.store(next && !parity ? filterBuilt.get() : nullptr, std::memory_order_release);
		}
		frameHeld = std::move(next);
	}

	void NoteStructure(const RE::NiNode* a_parent, RE::NiAVObject*, bool)
	{
		// A child of a filtered category node attached or detached: the node is walked natively until the next selection.
		const auto* filter = filterInstalled && a_parent ? filterCurrent.load(std::memory_order_acquire) : nullptr;
		if (!filter)
			return;
		if (const auto it = filter->nodes.find(a_parent); it != filter->nodes.end() && !it->second->dirty.exchange(true, std::memory_order_acq_rel))
			dirtied.fetch_add(1, std::memory_order_release);
	}

	void NoteMainRegistration(RE::BSGeometry* a_geometry)
	{
		if (!installed || !a_geometry || !lightsReady.load(std::memory_order_acquire))
			return;
		const auto* exclusion = frameExclusion.load(std::memory_order_acquire);
		if (!exclusion)
			return;
		const auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
		void* lightData = property ? Engine::At<void*>(property, Engine::kPropertyLightData) : nullptr;
		if (!lightData)
			return;
		const bool parity = parityFrame.load(std::memory_order_relaxed);
		const auto& candidates = *exclusion->candidates;
		std::uint32_t entry = ~0u;
		if (const auto it = candidates.geometries.find(a_geometry); it != candidates.geometries.end() && exclusion->excluded[candidates.geometryEntry[it->second]]) {
			// Once a frame: the first main registration reads the mask and clears it, and the later ones read 0 (engine notes).
			const std::uint32_t stamp = lightStamp.load(std::memory_order_relaxed);
			if (exclusion->cleared[it->second].exchange(stamp, std::memory_order_relaxed) == stamp)
				return;
			entry = candidates.geometryEntry[it->second];
		}
		if (!parity && entry == ~0u)
			return;
		std::array<std::uint32_t, kReachKinds> reach{};
		const std::uint32_t bits = entry != ~0u ? BitsOf(*exclusion, entry, *a_geometry, reach) : 0u;
		auto& mask = Engine::At<std::uint32_t>(lightData, Engine::kLightDataActiveMask);
		if (!parity) {
			mask |= bits;
			bitStats.given.fetch_add(1, std::memory_order_relaxed);
			bitStats.withBits.fetch_add(bits ? 1 : 0, std::memory_order_relaxed);
			return;
		}
		// Parity frames cut nothing: where a cut would have taken the geometry out (an excluded ancestor, or a filtered node's child
		// it does not keep), the engine's bits are compared with DCLF's.
		const std::uint32_t engine = mask & frameLightBits.load(std::memory_order_relaxed);
		if (entry == ~0u) {
			bool cut = false;
			for (const RE::NiAVObject* object = a_geometry; object && !cut; object = object->parent)
				cut = Excluded(*exclusion, object);
			if (const auto* filter = !cut && filterInstalled ? filterCurrent.load(std::memory_order_acquire) : nullptr) {
				bool through = false;
				cut = !UnderKeptChild(*filter, a_geometry, through) && through;
			}
			if (!cut || !engine)
				return;
			// Cut, but not as a geometry of an excluded entry: its bits would be lost.
			bitStats.unhandled.fetch_add(1, std::memory_order_relaxed);
			std::scoped_lock lock(firstMutex);
			if (bitsLogged++ < 8) {
				std::string chain;
				for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
					chain += fmt::format(" <- {}:{}{}", object->GetRTTI() ? object->GetRTTI()->name : "?", object->name.c_str() ? object->name.c_str() : "",
						candidates.entries.contains(object) ? (Excluded(*exclusion, object) ? " [excluded entry]" : " [entry]") : "");
				logger::info("[DCLF] point lights' bits: cut but not an excluded entry's geometry, engine bits {:#x}:{}", engine, chain);
			}
			return;
		}
		bitStats.compared.fetch_add(1, std::memory_order_relaxed);
		if (engine == bits) {
			bitStats.agree.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		(engine & ~bits ? bitStats.engineOnly : bitStats.dclfOnly).fetch_add(1, std::memory_order_relaxed);
		const std::uint32_t lights = std::min(frameLightNext.load(std::memory_order_relaxed), kMaxLights);
		std::string detail;
		for (std::uint32_t light = 0; light < lights; ++light) {
			const std::uint32_t bit = frameLights[light].bit;
			if (!bit || !((engine ^ bits) & bit))
				continue;
			std::uint32_t kind = kReachKinds;
			for (std::uint32_t k = 0; k < kReachKinds && kind == kReachKinds; ++k)
				kind = (reach[k] >> light) & 1 ? k : kReachKinds;
			((engine & bit) ? bitStats.engineOnlyBy : bitStats.dclfOnlyBy)[kind].fetch_add(1, std::memory_order_relaxed);
			const auto& sphere = frameLights[light];
			const auto& bound = a_geometry->worldBound;
			const float distance = std::sqrt((bound.center.x - sphere.centre[0]) * (bound.center.x - sphere.centre[0]) +
											 (bound.center.y - sphere.centre[1]) * (bound.center.y - sphere.centre[1]) +
											 (bound.center.z - sphere.centre[2]) * (bound.center.z - sphere.centre[2]));
			detail += fmt::format(" light {} ({}, {} only, distance {:.0f}, bound {:.0f}, radius {:.0f})", light, kReachNames[kind], (engine & bit) ? "engine" : "DCLF", distance,
				bound.radius, sphere.radius);
		}
		std::scoped_lock lock(firstMutex);
		if (bitsLogged++ < 8)
			logger::info("[DCLF] point lights' bits differ: '{}' (entry '{}'), engine {:#x}, DCLF {:#x}:{}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "?",
				candidates.entryNodes[entry]->name.c_str() ? candidates.entryNodes[entry]->name.c_str() : "?", engine, bits, detail);
	}

	void EndFrame()
	{
		retired[1].clear();
		std::swap(retired[0], retired[1]);
	}

	void NoteRegistration(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, bool a_withheld, std::uint32_t a_source)
	{
		if (!installed || !parityFrame.load(std::memory_order_relaxed) || !a_pass || !a_pass->geometry)
			return;
		const auto* exclusion = frameExclusion.load(std::memory_order_acquire);
		std::uint32_t mode = 0;
		if (!exclusion || !PassCapture::Get().ShadowModeOfBatch(a_batch, mode) || mode != kParabolicMode)
			return;
		// Under an excluded entry: any of its ancestors, as the skip takes it out (Process1 returns there). A caster of one need not
		// be a candidate geometry: an entry is excluded by its candidates alone (an effect under a cell entry, for one).
		bool underExcluded = false;
		for (const RE::NiAVObject* object = a_pass->geometry; object && !underExcluded; object = object->parent)
			underExcluded = Excluded(*exclusion, object);
		// The category filter: a caster under a filtered category node must hang from a child it keeps, withheld or not (the
		// registration also writes the engine's geometry's light masks).
		if (const auto* filter = filterInstalled && !underExcluded ? filterCurrent.load(std::memory_order_acquire) : nullptr) {
			bool through = false;
			if (!UnderKeptChild(*filter, a_pass->geometry, through) && through) {
				filterStats.parityMissed.fetch_add(1, std::memory_order_relaxed);
				std::string chain;
				for (const RE::NiAVObject* object = a_pass->geometry; object; object = object->parent)
					chain += fmt::format(" <- {}:{}{}{}", object->GetRTTI() ? object->GetRTTI()->name : "?", object->name.c_str() ? object->name.c_str() : "", Hidden(object) ? " hidden" : "",
						exclusion->candidates->entries.contains(object) ? (Excluded(*exclusion, object) ? " [excluded entry]" : " [entry]") : "");
				std::scoped_lock lock(firstMutex);
				if (filterFirstMissed.empty())
					filterFirstMissed = chain;
			}
			filterStats.parityChecked.fetch_add(through ? 1 : 0, std::memory_order_relaxed);
		}
		if (a_withheld)
			return;
		stats.parityPasses.fetch_add(1, std::memory_order_relaxed);
		if (underExcluded) {
			stats.parityLost.fetch_add(1, std::memory_order_relaxed);
			{
				const auto& tables = SceneStore::Get().GetTables();
				std::uint32_t reject = 15;
				std::string what = fmt::format("'{}' (insert {})", a_pass->geometry->name.c_str(), a_source);
				for (std::uint32_t o = 0; o < tables.objectGeometry.size(); ++o)
					if (tables.objectGeometry[o] == a_pass->geometry) {
						reject = tables.shadowReject[o];
						what += fmt::format(" object {} flags {:#x} reject {} technique {:#x}", o, tables.objects[o].flags, tables.shadowReject[o], tables.shadowTechnique[o]);
						break;
					}
				{
					const auto& candidates = *exclusion->candidates;
					const auto it = candidates.geometries.find(a_pass->geometry);
					what += it == candidates.geometries.end() ? std::string(" not a candidate geometry") :
					                                            fmt::format(" candidate of entry {} (excluded {})", candidates.geometryEntry[it->second],
																	Excluded(*exclusion, candidates.entryNodes[candidates.geometryEntry[it->second]]));
					// The exclusion's build, and each candidate geometry of the entry now (object, flags, claimed).
					what += fmt::format(" [exclusion built frame {}{}, selected at {}]", exclusion->builtFrame, exclusion->reused ? " (reused)" : "", SceneStore::Get().GetFrame());
					if (it != candidates.geometries.end()) {
						const auto entry = candidates.geometryEntry[it->second];
						const auto claims = PassCapture::Get().CurrentSet();
						for (const auto& [geometry, index] : candidates.geometries) {
							if (candidates.geometryEntry[index] != entry)
								continue;
							std::uint32_t object = ~0u;
							for (std::uint32_t o = 0; o < tables.objectGeometry.size() && object == ~0u; ++o)
								object = tables.objectGeometry[o] == geometry && !(tables.objects[o].flags & kObjectFree) ? o : object;
							what += fmt::format(" {{'{}' object {} flags {:#x} claimed {}}}", geometry->name.c_str() ? geometry->name.c_str() : "", static_cast<std::int32_t>(object),
								object != ~0u ? tables.objects[object].flags : 0u, claims && (claims->PhasesOf(geometry) & kSetCasterPoint));
						}
					}
				}
				// How the cull reached it: the chain up, with the entries and the excluded ones marked, and each switch's selection.
				const RE::NiAVObject* below = nullptr;
				for (const RE::NiAVObject* object = a_pass->geometry; object; below = object, object = object->parent) {
					what += fmt::format(" <- {}:{}{}{}", object->GetRTTI() ? object->GetRTTI()->name : "?", object->name.c_str() ? object->name.c_str() : "", Hidden(object) ? " hidden" : "",
						exclusion->candidates->entries.contains(object) ? (Excluded(*exclusion, object) ? " [excluded entry]" : " [entry]") : "");
					if (const auto* switchNode = below ? const_cast<RE::NiAVObject*>(object)->AsSwitchNode() : nullptr) {
						const auto state = SceneStore::ReadSwitch(*switchNode);
						std::int32_t path = -1;
						const auto& children = switchNode->GetChildren();
						for (std::uint16_t c = 0; c < children.size(); ++c)
							path = children[c].get() == below ? static_cast<std::int32_t>(c) : path;
						what += fmt::format(" [index {}, path child {}, selects it {}]", state.index, path, SceneStore::SwitchSelects(*switchNode, below));
					}
				}
				std::scoped_lock lock(firstMutex);
				++lostByReject[std::min<std::uint32_t>(reject, 15)];
				if (firstLost.empty())
					firstLost = std::move(what);
			}
		}
	}

	std::string Report()
	{
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		const std::uint64_t calls = accumulateCalls.exchange(0);
		const std::int64_t ticks = accumulateTicks.exchange(0);
		const auto timing = fmt::format("[DCLF] point lights' Accumulate: {} calls, {:.3f} ms in all", calls, ticks * 1000.0 / static_cast<double>(frequency.QuadPart));
		if (!installed)
			return timing;
		auto& s = stats;
		const auto lost = s.parityLost.exchange(0);
		const auto text = fmt::format("[DCLF] point lights' shadow culls: {} frames, {} with the exclusion ({} none built, {} the mode's claims not live, {} stale); {} entries visited, {} skipped{}; parity {} frames: {} paraboloid passes not withheld, {} of them under an excluded entry{}",
			s.frames, s.live, s.noExclusion, s.noClaims, s.stale, s.visited.exchange(0), s.skipped.exchange(0), probe ? fmt::format(" (probe: {} would be)", s.wouldSkip.load()) : std::string(),
			s.parityFrames, s.parityPasses.exchange(0), lost, s.parityFrames ? (lost ? " <- LIGHT EXCLUSION" : " <- OK") : "");
		std::scoped_lock lock(firstMutex);
		std::string by;
		for (std::size_t r = 0; r < lostByReject.size(); ++r)
			if (lostByReject[r])
				by += fmt::format(" reject {}={}", r, std::exchange(lostByReject[r], 0));
		const auto withFirst = firstLost.empty() ? text : text + "; lost by" + by + "; first lost " + std::exchange(firstLost, {});
		s.wouldSkip = 0;
		s.frames = s.live = s.noExclusion = s.noClaims = s.stale = s.parityFrames = 0;
		std::string list;
		if (filterInstalled) {
			auto& f = filterStats;
			const auto checked = f.parityChecked.exchange(0);
			const auto missed = f.parityMissed.exchange(0);
			const auto nodes = f.nodes.exchange(0);
			const std::string first = std::exchange(filterFirstMissed, {});  // firstMutex is held (above)
			list = fmt::format("\n[DCLF] point lights' category filter: {} lights, {} filtered, {} walked whole (frames without it); {:.1f} filtered nodes a light ({} now), {:.0f} children culled and {:.0f} cut a light, {} dirty nodes walked; {} builds ({} nodes, {:.3f} ms each); parity {} casters under a filtered node checked, {} not under a child it keeps{}{}",
				f.lights.exchange(0), f.filtered.load(), f.walkedNotLive.exchange(0), f.filtered.load() ? static_cast<double>(nodes) / static_cast<double>(f.filtered.load()) : 0.0,
				filterBuilt ? filterBuilt->nodes.size() : 0, f.filtered.load() ? static_cast<double>(f.children.exchange(0)) / static_cast<double>(f.filtered.load()) : 0.0,
				f.filtered.load() ? static_cast<double>(f.cut.exchange(0)) / static_cast<double>(f.filtered.load()) : 0.0, f.dirtyNodes.exchange(0), f.builds, f.nodesRebuilt,
				f.builds ? static_cast<double>(f.buildTicks) * 1000.0 / static_cast<double>(frequency.QuadPart) / static_cast<double>(f.builds) : 0.0,
				checked, missed, checked || missed ? (missed ? " <- LIGHT FILTER" : " <- OK") : "", first.empty() ? "" : "; first:" + first);
			f.filtered = 0;
			f.children = 0;
			f.cut = 0;
			f.builds = f.nodesRebuilt = 0;
			f.buildTicks = 0;
		}
		{
			auto& b = bitStats;
			auto byReach = [](auto& a_counts) {
				std::string text;
				for (std::uint32_t k = 0; k <= kReachKinds; ++k)
					if (const auto count = a_counts[k].exchange(0))
						text += fmt::format(" {} {}", kReachNames[k], count);
				return text.empty() ? std::string() : " (" + text.substr(1) + ")";
			};
			const auto engineOnly = b.engineOnly.exchange(0);
			const auto unhandled = b.unhandled.exchange(0);
			const auto compared = b.compared.exchange(0);
			list += fmt::format("\n[DCLF] point lights' bits for engine main passes under cut entries: {} lights ({} past the mask's 32), {} geometries given them ({} any); parity {} compared, {} agree, {} with engine-only bits{}, {} with DCLF-only bits{}, {} cut but not an excluded entry's{}",
				b.lights.exchange(0), b.overflow.exchange(0), b.given.exchange(0), b.withBits.exchange(0), compared, b.agree.exchange(0), engineOnly, byReach(b.engineOnlyBy),
				b.dclfOnly.exchange(0), byReach(b.dclfOnlyBy), unhandled, compared || unhandled ? (engineOnly || unhandled ? " <- LIGHT BITS" : " <- OK") : "");
			bitsLogged = 0;  // firstMutex is held (above)
		}
		return timing + "\n" + withFirst + list;
	}
}
