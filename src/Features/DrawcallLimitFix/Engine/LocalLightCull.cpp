#include "LocalLightCull.h"

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
#include <map>
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

		bool Excluded(const SunExclusion& a_exclusion, const void* a_object)
		{
			const auto& entries = a_exclusion.candidates->entries;
			const auto it = entries.find(static_cast<const RE::NiAVObject*>(a_object));
			return it != entries.end() && a_exclusion.excluded[it->second];
		}

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
		// Parity frames: point lights' mask writes on engine-drawn Lighting geometry, and those under an excluded entry (lost
		// while the entry is skipped).
		struct MaskStats
		{
			std::atomic<std::uint64_t> writes{ 0 }, underExcluded{ 0 };
		};
		MaskStats maskStats;
		std::string maskFirst;  // under firstMutex

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

		// ---- TEMP (CS_DCLF_LIGHT_CENSUS): what the point lights' culls still reach, by category and why it is not excluded.
		bool censusOn = false;
		std::mutex censusMutex;
		struct CensusRow
		{
			std::uint64_t entries = 0, passes = 0, withheld = 0, masks = 0;
			std::string example;
		};
		std::map<std::string, CensusRow> census;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::string> censusClass;  // entry -> class (render thread; read on parity frames)

		/** @brief The first geometries under a node, for the census: none, or the first one's classes. */
		std::string GeometryOf(const RE::NiAVObject* a_object)
		{
			std::uint32_t count = 0;
			std::string first;
			const std::function<void(const RE::NiAVObject*, std::uint32_t)> visit = [&](const RE::NiAVObject* a_node, std::uint32_t a_depth) {
				if (!a_node || count >= 64 || a_depth > 16)
					return;
				if (auto* geometry = const_cast<RE::NiAVObject*>(a_node)->AsGeometry()) {
					if (count++ == 0) {
						const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
						first = fmt::format("{} {}{}", geometry->GetRTTI() ? geometry->GetRTTI()->name : "?", property && property->GetRTTI() ? property->GetRTTI()->name : "no property",
							SceneStore::Get().IsTracked(geometry) ? " tracked" : "");
					}
					return;
				}
				if (const auto* node = const_cast<RE::NiAVObject*>(a_node)->AsNode())
					for (const auto& child : node->GetChildren())
						visit(child.get(), a_depth + 1);
			};
			visit(a_object, 0);
			return count ? fmt::format("geometry ({})", first) : "no geometry";
		}

		std::string ClassOf(const RE::NiAVObject* a_entry, std::uint32_t a_category, bool a_whole, const SunExclusion& a_exclusion)
		{
			if (Excluded(a_exclusion, a_entry))
				return "excluded";
			const auto* ref = a_entry->GetUserData();
			const bool actor = ref && ref->IsActor();
			const std::string where = a_whole ? fmt::format("whole entry {}", a_category) : fmt::format("category {}", a_category);
			if (actor)
				return where + ": actor";
			constexpr std::array<std::uint32_t, 4> kTracked{ 0, 3, 4, 5 };
			if (!a_whole && std::find(kTracked.begin(), kTracked.end(), a_category) == kTracked.end())
				return where + " (untracked): " + GeometryOf(a_entry);
			const auto verdict = SceneStore::Get().EntryCensus(a_entry);
			if (verdict.kind == 0)
				return where + ": candidate kept in";
			if (verdict.kind == 1)
				return where + ": not a sun entry, " + GeometryOf(a_entry);
			const auto reason = verdict.reason == Ineligible::Count ? std::string("not classified") :
			                                                           std::string(kIneligibleNames[static_cast<std::size_t>(verdict.reason)]);
			return where + ": blocked by " + reason;
		}

		void TakeCensus(const SunExclusion& a_exclusion)
		{
			const auto* sceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
			const auto* objectRoot = sceneNode && sceneNode->GetChildren().free_idx() > 3 ? sceneNode->GetChildren()[3].get() : nullptr;
			const auto* root = objectRoot ? const_cast<RE::NiAVObject*>(objectRoot)->AsNode() : nullptr;
			if (!root)
				return;
			std::scoped_lock lock(censusMutex);
			for (auto& [name, row] : census)
				row.entries = 0;
			censusClass.clear();
			auto note = [&](const RE::NiAVObject* a_entry, std::string a_class) {
				auto& row = census[a_class];
				++row.entries;
				if (row.example.empty())
					row.example = a_entry->name.c_str() ? a_entry->name.c_str() : "";
				censusClass.insert_or_assign(a_entry, std::move(a_class));
			};
			const auto& cells = root->GetChildren();
			for (std::uint16_t i = 0; i < cells.free_idx(); ++i) {
				const auto* cell = cells[i] ? const_cast<RE::NiAVObject*>(cells[i].get())->AsNode() : nullptr;
				if (!cell)
					continue;
				for (std::uint16_t j = 0; j < cell->GetChildren().free_idx(); ++j) {
					const auto* child = cell->GetChildren()[j].get();
					if (!child)
						continue;
					if (i < 2) {
						note(child, ClassOf(child, i, true, a_exclusion));  // a whole entry's children
						continue;
					}
					const auto* category = const_cast<RE::NiAVObject*>(child)->AsNode();
					if (!category) {
						note(child, fmt::format("category {}: not a node", j));
						continue;
					}
					for (const auto& entry : category->GetChildren())
						if (entry)
							note(entry.get(), ClassOf(entry.get(), j, false, a_exclusion));
				}
			}
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
			if (whole && censusOn)
				TakeCensus(*a_exclusion);
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
				if (const auto* filter = lightFilter)
					if (const auto it = filter->nodes.find(a_node); it != filter->nodes.end()) {
						const auto& category = *it->second;
						if (!category.dirty.load(std::memory_order_acquire)) {
							if (a_node->worldBound.radius != 0.0f || (a_node->GetFlags().underlying() & kAlwaysDraw)) {
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
						filterStats.dirtyNodes.fetch_add(1, std::memory_order_relaxed);
					}
				func(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

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
				LARGE_INTEGER start{}, end{};
				QueryPerformanceCounter(&start);
				const Filter* filter = nullptr;
				if (filterInstalled) {
					filterStats.lights.fetch_add(1, std::memory_order_relaxed);
					filter = frameFilter.load(std::memory_order_acquire);
					(filter ? filterStats.filtered : filterStats.walkedNotLive).fetch_add(1, std::memory_order_relaxed);
				}
				lightFilter = filter;
				func(a_light, a_count, a_arg2, a_arg3);
				lightFilter = nullptr;
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
				if (a_object)
					if (const auto* exclusion = frameExclusion.load(std::memory_order_acquire)) {
						stats.visited.fetch_add(1, std::memory_order_relaxed);
						if (Excluded(*exclusion, a_object)) {
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
		censusOn = SwitchEnabled(Switch::LightCensus);
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
		++stats.frames;
		// The culls of the frame read this one; the previous frame's culls are over (they ran before its shadow epoch).
		std::shared_ptr<SunExclusion> next = pending;
		if (!next || !next->candidates)
			++stats.noExclusion, next.reset();
		else if (!PassCapture::Get().ShadowModeWithheld(kParabolicMode))
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

	void NoteMaskWrite(const void* a_accumulator, const RE::BSGeometry* a_geometry, bool a_owned)
	{
		if (!installed || !parityFrame.load(std::memory_order_relaxed) || a_owned || !a_geometry || !a_accumulator)
			return;
		// A paraboloid light's accumulator (render mode 0xF), and a geometry whose main pass reads the mask (GetRenderPasses of
		// a Lighting property with light data).
		if (Engine::At<std::uint32_t>(a_accumulator, 0x150) != 0xF)
			return;
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get());
		if (!lighting || !Engine::At<void*>(lighting, 0x70))
			return;
		const auto* exclusion = frameExclusion.load(std::memory_order_acquire);
		bool underExcluded = false;
		for (const RE::NiAVObject* object = a_geometry; exclusion && object && !underExcluded; object = object->parent)
			underExcluded = Excluded(*exclusion, object);
		if (const auto* filter = !underExcluded && filterInstalled ? filterCurrent.load(std::memory_order_acquire) : nullptr) {
			bool through = false;
			underExcluded = !UnderKeptChild(*filter, a_geometry, through) && through;
		}
		maskStats.writes.fetch_add(1, std::memory_order_relaxed);
		if (underExcluded) {
			maskStats.underExcluded.fetch_add(1, std::memory_order_relaxed);
			std::scoped_lock lock(firstMutex);
			if (maskFirst.empty())
				for (const RE::NiAVObject* object = a_geometry; object; object = object->parent) {
					const auto* filter = filterCurrent.load(std::memory_order_acquire);
					const auto node = filter && object->parent ? filter->nodes.find(object->parent) : decltype(filter->nodes)::const_iterator{};
					const bool filtered = filter && object->parent && node != filter->nodes.end();
					const bool kept = filtered && std::find_if(node->second->children.begin(), node->second->children.end(), [&](const auto& a_child) { return a_child.get() == object; }) != node->second->children.end();
					maskFirst += fmt::format(" <- {}:{}{}{}{}", object->GetRTTI() ? object->GetRTTI()->name : "?", object->name.c_str() ? object->name.c_str() : "",
						exclusion && exclusion->candidates->entries.contains(object) ? (Excluded(*exclusion, object) ? " [excluded entry]" : " [entry]") : "",
						filtered ? (kept ? " [kept]" : " [cut]") : "", SceneStore::Get().IsCategoryNode(object) ? " [category]" : "");
				}
		}
		if (censusOn) {
			std::scoped_lock lock(censusMutex);
			for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
				if (const auto it = censusClass.find(object); it != censusClass.end()) {
					++census[it->second].masks;
					break;
				}
		}
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
		if (censusOn) {
			std::scoped_lock lock(censusMutex);
			for (const RE::NiAVObject* object = a_pass->geometry; object; object = object->parent)
				if (const auto it = censusClass.find(object); it != censusClass.end()) {
					auto& row = census[it->second];
					++row.passes;
					row.withheld += a_withheld ? 1 : 0;
					break;
				}
		}
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
			const auto lostMasks = maskStats.underExcluded.exchange(0);
			list += fmt::format("\n[DCLF] point lights' masks (parity frames): {} written on engine-drawn Lighting geometry, {} of them under an excluded entry or a cut child{}{}",
				maskStats.writes.exchange(0), lostMasks, lostMasks ? " <- LIGHT MASKS LOST" : "", maskFirst.empty() ? "" : "; first '" + std::exchange(maskFirst, {}) + "'");
		}
		std::string censusText;
		if (censusOn) {
			std::scoped_lock censusLock(censusMutex);
			censusText = "\n[DCLF] TEMP point lights' census (entries at the last build; paraboloid passes on parity frames, withheld):";
			for (auto& [name, row] : census) {
				censusText += fmt::format("\n    {}: {} entries, {} passes ({} withheld), {} mask writes on engine-drawn Lighting geometry; e.g. '{}'", name, row.entries, row.passes, row.withheld,
					row.masks, row.example);
				row.passes = row.withheld = row.masks = 0;
			}
		}
		return timing + "\n" + withFirst + list + censusText;
	}
}
