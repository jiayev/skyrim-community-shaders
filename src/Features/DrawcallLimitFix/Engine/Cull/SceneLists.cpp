// PrimaryCull's scene lists (drawcall-limit-fix.md, "The scene lists without DCLF's roots"): the filter published on the render
// thread (PublishListFilter, RestoreSceneLists), the build's job (DrawWorld_BuildSceneLists' detour: kept, rebuilt or the engine's
// build filtered), and the attach/detach detours' structure events (any thread). PrimaryCull.h, PublishListFilter, describes the modes.
#include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Engine/Views/PortalViews.h"

#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PassCapture.h"
#include "Features/DrawcallLimitFix/Runtime/Switches.h"
#include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"

#include <algorithm>
#include <atomic>
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"

namespace DCLF
{
	namespace
	{
		using namespace Engine;

		// AE 1.6.1170 (module offsets).
		constexpr std::uintptr_t kBuildSceneLists = 0x64bc20;  // DrawWorld_BuildSceneLists, a job of Main::Draw's
		constexpr std::uintptr_t kClearList = 0x64cf20;        // FUN_14064cf20(list): Main::Draw's ClearLists job, one list
		constexpr std::uintptr_t kCullList = 0xe28f70;         // FUN_140e28f70(process, list, camera, skipHidden, jobs): every list cull
		// In DrawWorld_BuildSceneLists: `mov rax, [rcx]; call [rax + 0x18]`, the object root's AsNode (ShadowSceneNode child 3).
		constexpr std::uintptr_t kObjectRootAsNode = 0x64be5a;
		constexpr std::array<std::uint8_t, 6> kObjectRootAsNodeBytes{ 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18 };
		constexpr std::size_t kTesWorldSpace = 0x148;         // TES: the current worldspace (FUN_14019d200)
		constexpr std::size_t kWorldSpaceFlags = 0xa0;        // byte; 0x40: the build skips category node 2

		using SceneList = RE::BSTArray<RE::NiPointer<RE::NiAVObject>>;

		bool Hidden(const RE::NiAVObject* a_object) { return IsHidden(a_object); }

		const RE::ShadowSceneNode* SceneNode()
		{
			return globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		}

		/** @brief The engine's build's category rule: cell children 2, 3, 5, 6, 7 and on (node 2 not when the worldspace says so). */
		bool CategoryListed(std::uint32_t a_index, bool a_skipCategory2)
		{
			return ((a_index & ~5u) != 0 || a_index == 5) && !(a_skipCategory2 && a_index == 2);
		}
	}

	template <class List, class Structural>
	void PrimaryCull::EnumerateSceneLists(const RE::NiAVObject* a_objectRoot, bool a_skipCategory2, List&& a_list, Structural&& a_structural)
	{
		// DrawWorld_BuildSceneLists (AE 0x14064bc20), the exterior branch's object root: every hidden test on an entry itself is
		// the list cull's (CullList), and the ones on the nodes above are structure.
		if (!a_objectRoot)
			return;
		a_structural(a_objectRoot);
		auto* objectRoot = const_cast<RE::NiAVObject*>(a_objectRoot)->AsNode();
		if (objectRoot && !Hidden(objectRoot)) {
			const auto& cells = objectRoot->GetChildren();
			for (std::uint16_t i = 0; i < cells.free_idx(); ++i) {
				auto* cell = cells[i] ? cells[i]->AsNode() : nullptr;
				if (!cell)
					continue;
				// Children 0 and 1 are entries, whole, in list 0.
				if (i < 2) {
					a_list(cell, true);
					continue;
				}
				a_structural(cell);
				if (Hidden(cell))
					continue;
				const auto& categories = cell->GetChildren();
				for (std::uint16_t j = 0; j < categories.free_idx(); ++j) {
					auto* category = categories[j].get();
					if (!category || !CategoryListed(j, a_skipCategory2))
						continue;
					auto* node = category->AsNode();
					if (!node) {
						a_list(category, false);
						continue;
					}
					a_structural(node);
					if (Hidden(node))
						continue;
					const auto& roots = node->GetChildren();
					for (std::uint16_t k = j == 6 ? 1 : 0; k < roots.free_idx(); ++k)
						if (roots[k])
							a_list(roots[k].get(), false);
				}
			}
		}
	}

	bool PrimaryCull::IsSceneList(const void* a_list, bool a_extra)
	{
		if (a_extra && a_list == &Global<SceneList>(Engine::Address::kExtraList))
			return true;
		const auto* lists = Global<SceneList*>(Engine::Address::kSceneLists);
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		return lists && a_list >= lists && a_list < lists + count;
	}

	bool PrimaryCull::ListsKeepable(std::uint64_t& a_witness)
	{
		const auto fail = [&](std::size_t a_reason) {
			++listStats.notKeepableBy[a_reason];
			return false;
		};
		if (!Scene::HiddenStoresLive() || !listsKeepInstalled)
			return fail(0);
		const auto* sceneNode = SceneNode();
		auto** processes = Global<RE::NiCullingProcess**>(Engine::Address::kListProcesses);
		const auto* tes = RE::TES::GetSingleton();
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		if (!sceneNode || !processes || !processes[0] || !tes || !count || count > listSentinels.size())
			return fail(1);
		// The interior branch (and with no unbound space, the water's) does not walk the object root. Whether the walk reached
		// unbound space is DCLF's walk's (PortalViews, after Main::Update's).
		if (!PortalViews::Get().VisibleUnbound())
			return fail(2);
		if (tes->interiorCell)
			return fail(3);
		const auto& scene = sceneNode->GetChildren();
		if (scene.free_idx() < 4 || !scene[3])
			return fail(4);
		const auto* worldSpace = At<const std::byte*>(tes, kTesWorldSpace);
		const bool skipCategory2 = worldSpace && (At<std::uint8_t>(worldSpace, kWorldSpaceFlags) & 0x40);
		a_witness = (std::uint64_t(count) << 1) ^ (skipCategory2 ? 1u : 0u) ^ (reinterpret_cast<std::uintptr_t>(scene[3].get()) << 8) ^
		            (std::uint64_t(scene.free_idx()) << 56);
		return true;
	}

	std::shared_ptr<const PrimaryCull::ListFilter> PrimaryCull::CurrentListFilter()
	{
		const auto toggles = ActiveToggles();
		if (!Policy::WithholdFromEngine() || !toggles.excludePrimaryEntries) {
			++listStats.notToggled;
			return nullptr;
		}
		// Precipitation::SetupMask culls every list when the engine draws an occlusion map: both must be DCLF's.
		if (!OcclusionEnabled(kOcclusionSky) || !OcclusionEnabled(kOcclusionPrecipitation)) {
			++listStats.noOcclusion;
			return nullptr;
		}
		// The cut as the last frame left it, brought up to the frame's candidates (it plans again only the entries that moved).
		if (const auto candidates = Scene::FrameView::Get().GetSunCandidates(); !candidates) {
			++listStats.notCurrent;
			return nullptr;
		} else if (candidates != cut.candidates) {
			std::vector<std::uint32_t> changed;
			ChangedEntries(cut.candidates.get(), *candidates, changed);
			SyncCut(candidates, changed);
			cutStats.entriesPlanned += changed.size();
		}
		// The exclusion this frame's full-frustum cull applies, for the same snapshot: a root out of the lists is out of
		// the sun's cascades only when that exclusion takes it out too. With the sun off (an interior) no full-frustum cull
		// reads the lists, and no exclusion is needed.
		const bool sunOff = Global<std::uint8_t>(Engine::Address::kSunOff) != 0;
		const auto exclusion = sunOff ? nullptr : SunAccumulation::Get().PendingExclusion();
		if (!sunOff && (!exclusion || exclusion->candidates != cut.candidates || exclusion->excluded.size() != cut.plans.size())) {
			++listStats.noExclusion;
			return nullptr;
		}
		const std::uint64_t exclusionVersion = sunOff ? ~0ull : exclusion->version;
		// Built again only when an entry's verdict may have moved: the cut's admissions and walks, or the exclusion's content.
		const auto entries = static_cast<std::uint32_t>(cut.plans.size());
		if (listFilterBuilt && listFilterBuilt->candidates == cut.candidates && filterCutVersion == cutVersion && filterExclusionVersion == exclusionVersion &&
			listRemovable.size() == entries)
			return listFilterBuilt;
		filterCutVersion = cutVersion;
		filterExclusionVersion = exclusionVersion;
		bool changed = !listFilterBuilt || listFilterBuilt->candidates != cut.candidates || listRemovable.size() != entries;
		listRemovable.resize(entries, 0);
		for (std::uint32_t e = 0; e < entries; ++e) {
			const std::uint8_t removable =
				cut.plans[e] != EntryPlan::Rejected && cut.admitted[e] && !cut.walk[e] && !cut.mixed[e] && (sunOff || (e < exclusion->excluded.size() && exclusion->excluded[e])) ? 1 : 0;
			changed |= removable != listRemovable[e];
			listRemovable[e] = removable;
		}
		if (changed) {
			auto filter = std::make_shared<ListFilter>();
			filter->candidates = cut.candidates;
			for (std::uint32_t e = 0; e < entries; ++e)
				if (listRemovable[e])
					filter->roots.insert(cut.roots[e]);
			listFilterBuilt = std::move(filter);
			++listStats.built;
		}
		return listFilterBuilt;
	}

	void PrimaryCull::PublishListFilter()
	{
		++listStats.frames;
		listsFiltered.store(false, std::memory_order_release);
		listFilter.store(nullptr, std::memory_order_release);
		listMode.store(ListMode::Engine, std::memory_order_release);
		Diagnostics::Notify<Diagnostics::EnginePoint::SceneListsPublishing>();
		if (!installed) {
			++listStats.notToggled;
			listsDirty.store(true, std::memory_order_relaxed);
			return;
		}
		auto filter = CurrentListFilter();
		std::uint64_t witness = 0;
		const bool keepable = ListsKeepable(witness);
		// The engine's build, whole: its observers look at the roots the filter would have left out, and at the engine's lists.
		if (!Policy::FilterSceneLists(Scene::FrameView::Get().GetFrame())) {
			if (Diagnostics::Armed<Diagnostics::SceneListsWhole>())
				Diagnostics::Notify(Diagnostics::SceneListsWhole{ filter, filter ? &filter->roots : nullptr, keepable });
			++listStats.dryRuns;
			++listStats.engineFrames;
			listsDirty.store(true, std::memory_order_relaxed);
			return;
		}
		ListMode mode = ListMode::Engine;
		if (keepable) {
			const bool dirty = listsDirty.load(std::memory_order_relaxed);
			const bool structure = listStructure.load(std::memory_order_acquire) != listStructureBuilt;
			const bool witnessMoved = witness != listWitnessBuilt;
			const bool filterMoved = filter != listFilterKept;
			mode = dirty || structure || witnessMoved || filterMoved ? ListMode::Rebuild : ListMode::Keep;
			listStats.rebuildDirty += dirty ? 1 : 0;
			listStats.rebuildStructure += !dirty && structure ? 1 : 0;
			listStats.rebuildWitness += !dirty && !structure && witnessMoved ? 1 : 0;
			listStats.rebuildFilter += !dirty && !structure && !witnessMoved && filterMoved ? 1 : 0;
			listWitness = witness;
		} else {
			++listStats.notKeepable;
			++listStats.engineFrames;
			listsDirty.store(true, std::memory_order_relaxed);
		}
		listStats.published += filter ? 1 : 0;
		listFilter.store(std::move(filter), std::memory_order_release);
		listMode.store(mode, std::memory_order_release);
	}

	void PrimaryCull::FilterSceneLists()
	{
		const auto filter = listFilter.load(std::memory_order_acquire);
		if (!filter || filter->roots.empty())
			return;
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		auto* lists = Global<SceneList*>(Engine::Address::kSceneLists);
		if (!lists || !count)
			return;
		std::uint64_t seen = 0, removed = 0;
		for (std::uint32_t l = 0; l < count; ++l) {
			auto& list = lists[l];
			const std::uint32_t size = list.size();
			seen += size;
			filterScratch.assign(size, 0);
			std::uint32_t removable = 0;
			for (std::uint32_t r = 0; r < size; ++r)
				if (const auto* object = list[r].get(); object && filter->roots.contains(object)) {
					filterScratch[r] = 1;
					++removable;
				}
			// Each list keeps an entry: its job sets its process's frustum up from the first (Process2).
			if (removable && removable == size) {
				filterScratch[0] = 0;
				--removable;
			}
			if (!removable)
				continue;
			std::uint32_t kept = 0;
			for (std::uint32_t r = 0; r < size; ++r) {
				if (filterScratch[r]) {
					list[r].reset();
					continue;
				}
				if (kept != r)
					list[kept] = std::move(list[r]);
				++kept;
			}
			list.resize(kept);
			removed += removable;
		}
		listStats.filtered.fetch_add(1, std::memory_order_relaxed);
		listStats.entries.fetch_add(seen, std::memory_order_relaxed);
		listStats.removed.fetch_add(removed, std::memory_order_relaxed);
		listsFiltered.store(true, std::memory_order_release);
	}

	void PrimaryCull::RebuildSceneLists(const ListFilter* a_filter)
	{
		const std::int64_t start = Now();
		const auto* sceneNode = SceneNode();
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		auto* lists = Global<SceneList*>(Engine::Address::kSceneLists);
		const auto* tes = RE::TES::GetSingleton();
		if (!sceneNode || !lists || !count || count > listSentinels.size() || !tes)
			return;
		// Read first: an event during the build rebuilds again next frame.
		listStructureBuilt = listStructure.load(std::memory_order_acquire);
		listWitnessBuilt = listWitness;
		listFilterKept = listFilter.load(std::memory_order_acquire);
		BuryLists();
		for (std::uint32_t l = 0; l < count; ++l) {
			auto& list = lists[l];
			if (!listSentinels[l])
				listSentinels[l] = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(0));
			if (listSentinels[l])
				list.push_back(RE::NiPointer<RE::NiAVObject>(listSentinels[l].get()));
		}
		const auto* objectRoot = ObjectRoot(*sceneNode);
		const auto* worldSpace = At<const std::byte*>(tes, kTesWorldSpace);
		const bool skipCategory2 = worldSpace && (At<std::uint8_t>(worldSpace, kWorldSpaceFlags) & 0x40);
		DropListEvents();
		listStructural.clear();
		listCategories.clear();
		listPositions.clear();
		std::uint32_t next = 0;
		std::uint64_t entries = 0, removed = 0;
		EnumerateSceneLists(
			objectRoot, skipCategory2,
			[&](const RE::NiAVObject* a_root, bool a_first) {
				if (a_filter && a_filter->roots.contains(a_root)) {
					++removed;
					return;
				}
				++entries;
				auto& list = lists[a_first ? 0 : next++ % count];
				listPositions.insert_or_assign(a_root, std::pair<std::uint32_t, std::uint32_t>(a_first ? 0 : (next - 1) % count, list.size()));
				list.push_back(RE::NiPointer<RE::NiAVObject>(const_cast<RE::NiAVObject*>(a_root)));
			},
			[&](const RE::NiAVObject* a_node) { listStructural.insert(a_node); });
		listNext = next;
		// The category nodes whose roots were listed: a root attached under one is listed by an event.
		ForEachCell(objectRoot, 2, [&](RE::NiNode& a_cell) {
			ForEachCategory(a_cell, [&](std::uint16_t a_index, RE::NiNode& a_category) {
				if (CategoryListed(a_index, skipCategory2))
					listCategories.emplace(&a_category, a_index);
			});
		});
		for (std::uint32_t l = 0; l < count; ++l)
			listKeptSize[l] = lists[l].size();
		// What the detours compare a parent with.
		const auto* objectNode = objectRoot ? const_cast<RE::NiAVObject*>(objectRoot)->AsNode() : nullptr;
		for (std::uint16_t i = 0; i < 2; ++i)
			listWholeEntries[i].store(objectNode && i < objectNode->GetChildren().free_idx() ? objectNode->GetChildren()[i].get() : nullptr, std::memory_order_relaxed);
		listObjectRoot.store(objectRoot, std::memory_order_release);
		listsDirty.store(false, std::memory_order_relaxed);
		listStats.rebuilt.fetch_add(1, std::memory_order_relaxed);
		listStats.rebuildEntries.fetch_add(entries, std::memory_order_relaxed);
		listStats.removed.fetch_add(removed, std::memory_order_relaxed);
		listStats.rebuildTicks.fetch_add(static_cast<std::uint64_t>(Now() - start), std::memory_order_relaxed);
	}

	void PrimaryCull::DropListEvents()
	{
		listEvents.Drain([&](ListEvent&& a_event) {
			if (a_event.held)
				listGraveyard.push_back(std::move(a_event.held));
		});
	}

	bool PrimaryCull::ApplyListEvents(const ListFilter* a_filter)
	{
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		auto* lists = Global<SceneList*>(Engine::Address::kSceneLists);
		bool rebuild = false;
		listEvents.Drain([&](ListEvent&& a_event) {
			listStats.eventsApplied.fetch_add(1, std::memory_order_relaxed);
			RE::NiPointer<RE::NiAVObject> held = std::move(a_event.held);
			if (rebuild || !lists || !count)
				return;
			const auto category = listCategories.find(a_event.parent);
			if (category == listCategories.end()) {
				if (held)
					listGraveyard.push_back(std::move(held));
				return;  // not a listed category node: nothing under it is listed
			}
			if (a_event.attached) {
				// Category node 6's first child is not listed (the build starts at 1); the cell's order is read now.
				const auto* node = const_cast<RE::NiAVObject*>(a_event.parent)->AsNode();
				const bool first = category->second == 6 && node && node->GetChildren().free_idx() && node->GetChildren()[0].get() == a_event.child;
				if (!first && held && !listPositions.contains(a_event.child) && !(a_filter && a_filter->roots.contains(a_event.child))) {
					const std::uint32_t l = listNext++ % count;
					listPositions.emplace(a_event.child, std::pair<std::uint32_t, std::uint32_t>(l, listKeptSize[l]));
					lists[l].push_back(held);
					++listKeptSize[l];
					listStats.eventsAdded.fetch_add(1, std::memory_order_relaxed);
				}
				if (held)
					listGraveyard.push_back(std::move(held));
				return;
			}
			const auto it = listPositions.find(a_event.child);
			if (it == listPositions.end())
				return;
			const auto [l, position] = it->second;
			const std::uint32_t last = listKeptSize[l] - 1;
			if (l >= count || position > last || lists[l][position].get() != a_event.child) {
				rebuild = true;  // the kept lists are not what the positions say: built again
				return;
			}
			listGraveyard.push_back(std::move(lists[l][position]));
			if (position != last) {
				lists[l][position] = std::move(lists[l][last]);
				listPositions[lists[l][position].get()].second = position;
			}
			lists[l].resize(last);
			listKeptSize[l] = last;
			listPositions.erase(it);
			listStats.eventsRemoved.fetch_add(1, std::memory_order_relaxed);
		});
		if (rebuild)
			listStats.eventsRebuilt.fetch_add(1, std::memory_order_relaxed);
		return !rebuild;
	}

	void PrimaryCull::BuryLists()
	{
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		auto* lists = Global<SceneList*>(Engine::Address::kSceneLists);
		for (std::uint32_t l = 0; lists && l < count; ++l) {
			for (auto& entry : lists[l])
				if (entry)
					listGraveyard.push_back(std::move(entry));
			lists[l].clear();
		}
	}

	void PrimaryCull::ListUpkeep()
	{
		// What DrawWorld_BuildSceneLists' object root walk does besides the lists: its visible children marked kAccumulated.
		const auto* sceneNode = SceneNode();
		if (!sceneNode)
			return;
		ForEachCell(ObjectRoot(*sceneNode), 0, [](RE::NiNode& a_node) {
			std::atomic_ref<std::uint32_t>(At<std::uint32_t>(&a_node, kObjectFlags)).fetch_or(kFlagAccumulated, std::memory_order_relaxed);
		});
	}

	bool PrimaryCull::ListRoots(const RE::NiAVObject*& a_objectRoot, std::vector<const RE::NiAVObject*>& a_roots)
	{
		const auto* sceneNode = SceneNode();
		const auto* tes = RE::TES::GetSingleton();
		if (!sceneNode || !tes)
			return false;
		a_objectRoot = ObjectRoot(*sceneNode);
		const auto* worldSpace = At<const std::byte*>(tes, kTesWorldSpace);
		const bool skipCategory2 = worldSpace && (At<std::uint8_t>(worldSpace, kWorldSpaceFlags) & 0x40);
		EnumerateSceneLists(a_objectRoot, skipCategory2, [&](const RE::NiAVObject* a_root, bool) { a_roots.push_back(a_root); }, [](const RE::NiAVObject*) {});
		return true;
	}

	void PrimaryCull::RestoreSceneLists()
	{
		if (!listsFiltered.exchange(false, std::memory_order_acq_rel))
			return;
		const auto filter = listFilter.load(std::memory_order_acquire);
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		auto* lists = Global<SceneList*>(Engine::Address::kSceneLists);
		if (!filter || !lists || !count)
			return;
		// The filter's roots are alive: its candidates hold them (SunCandidates::held). One the engine detached since the snapshot
		// was made stays out (it has left the scene); a hidden one is skipped by the cull. Taking a reference to a root the
		// snapshot did not hold is what put freed roots into the lists, released at Present (crash-catalog.md).
		const auto* scene = SceneNode();
		std::uint32_t l = 0;
		for (const auto* root : filter->roots) {
			const RE::NiAVObject* above = root;
			for (std::uint32_t depth = 0; above && above != scene && depth < 64; ++depth)
				above = above->parent;
			if (!scene || above != scene) {
				++listStats.restoreDetached;
				continue;
			}
			lists[l++ % count].push_back(RE::NiPointer<RE::NiAVObject>(const_cast<RE::NiAVObject*>(root)));
		}
		listsDirty.store(true, std::memory_order_relaxed);
		++listStats.restored;
	}

	void PrimaryCull::NoteListStructure(const RE::NiNode* a_parent, RE::NiAVObject* a_child, bool a_attached)
	{
		LocalLightCull::NoteStructure(a_parent, a_child, a_attached);
		PortalViews::Get().NoteStructure(a_parent);
		const auto* objectRoot = listObjectRoot.load(std::memory_order_acquire);
		if (!a_parent || !objectRoot)
			return;
		const auto* whole0 = listWholeEntries[0].load(std::memory_order_relaxed);
		const auto* whole1 = listWholeEntries[1].load(std::memory_order_relaxed);
		int kind = a_parent == objectRoot ? 0 : -1;
		// A cell's category node, or a category node's root; not anything under the two whole entries (the actors).
		if (const auto* grandparent = a_parent->parent; kind < 0 && grandparent) {
			if (grandparent == objectRoot && a_parent != whole0 && a_parent != whole1)
				kind = 1;
			else if (grandparent->parent == objectRoot && grandparent != whole0 && grandparent != whole1)
				kind = 2;
		}
		if (kind < 0)
			return;
		listStats.structureBy[kind].fetch_add(1, std::memory_order_relaxed);
		// A root: applied by the next build. A detach is identity only: the root may be gone by then.
		if (kind == 2 && a_child) {
			listEvents.Push({ a_attached ? RE::NiPointer<RE::NiAVObject>(a_child) : RE::NiPointer<RE::NiAVObject>(), a_child, a_parent, a_attached });
			return;
		}
		listStructure.fetch_add(1, std::memory_order_relaxed);
	}

	struct PrimaryCull::ListHooks
	{
		/** @brief DrawWorld_BuildSceneLists (a job of Main::Draw's): the engine's build, DCLF's, or the kept lists. */
		struct BuildSceneLists
		{
			static void thunk()
			{
				auto& self = PrimaryCull::Get();
				auto mode = self.listMode.load(std::memory_order_acquire);
				// A structure change since the publication: built again now rather than kept.
				if (mode == ListMode::Keep && self.listStructure.load(std::memory_order_acquire) != self.listStructureBuilt) {
					mode = ListMode::Rebuild;
					self.listStats.lateRebuilds.fetch_add(1, std::memory_order_relaxed);
				}
				const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
				auto* lists = Global<SceneList*>(Engine::Address::kSceneLists);
				// Still the exterior branch (an interior since the publication: the engine's build, on cleared lists).
				std::uint64_t witness = 0;
				if (mode != ListMode::Engine && !self.ListsKeepable(witness))
					mode = ListMode::Engine;
				if (mode == ListMode::Engine) {
					// Main::Draw clears the lists at the frame's end (ClearLists, after the occlusion maps): after a frame that
					// kept them they still hold DCLF's build, which the engine's would be appended to.
					self.DropListEvents();
					self.BuryLists();
					func();
					self.listsDirty.store(true, std::memory_order_relaxed);
					Diagnostics::Notify<Diagnostics::EnginePoint::EngineSceneListsBuilt>();
					self.FilterSceneLists();
					return;
				}
				// A list shorter than its kept part was cleared after all: built again.
				for (std::uint32_t l = 0; mode == ListMode::Keep && l < count && l < self.listKeptSize.size(); ++l)
					if (lists[l].size() < self.listKeptSize[l]) {
						mode = ListMode::Rebuild;
						self.listStats.lateRebuilds.fetch_add(1, std::memory_order_relaxed);
					}
				if (mode == ListMode::Rebuild) {
					self.RebuildSceneLists(self.listFilter.load(std::memory_order_acquire).get());
				} else {
					// Last frame's engine entries (rooms, occlusion planes) go; the kept part stays, with the roots attached and
					// detached since.
					for (std::uint32_t l = 0; l < count && l < self.listKeptSize.size(); ++l)
						lists[l].resize(self.listKeptSize[l]);
					if (self.ApplyListEvents(self.listFilterKept.get())) {
						self.listStats.kept.fetch_add(1, std::memory_order_relaxed);
					} else {
						self.listStats.lateRebuilds.fetch_add(1, std::memory_order_relaxed);
						self.RebuildSceneLists(self.listFilter.load(std::memory_order_acquire).get());
					}
				}
				// The engine's build for the rest: the extra list, and what follows the camera; not the object root.
				self.skipObjectRoot = true;
				func();
				self.skipObjectRoot = false;
				self.ListUpkeep();
				if (const auto filter = self.listFilterKept; filter && !filter->roots.empty())
					self.listsFiltered.store(true, std::memory_order_release);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The build's AsNode of the object root: null while its part of the lists is DCLF's (kept or built). */
		struct ObjectRootAsNode
		{
			static RE::NiNode* thunk(RE::NiAVObject* a_object)
			{
				return PrimaryCull::Get().skipObjectRoot ? nullptr : a_object->AsNode();
			}
		};

		/** @brief Main::Draw's ClearLists job, one list: kept lists stay (the build trims the engine's own entries). */
		struct ClearList
		{
			static void thunk(void* a_list)
			{
				if (PrimaryCull::Get().listMode.load(std::memory_order_acquire) != ListMode::Engine && IsSceneList(a_list, false))
					return;
				func(a_list);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief FUN_140e28f70, every list cull (the list jobs, the sun's full-frustum cull, Precipitation::SetupMask): a scene
		 * list's hidden entries are skipped, as the engine's own build leaves them out (kept lists hold them).
		 */
		struct CullList
		{
			static void thunk(void* a_process, void* a_list, void* a_camera, bool a_skipHidden, bool a_jobs)
			{
				func(a_process, a_list, a_camera, a_skipHidden || IsSceneList(a_list, true), a_jobs);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void PrimaryCull::InstallSceneLists()
	{
		const auto base = REL::Module::get().base();
		if (std::memcmp(reinterpret_cast<const void*>(base + kObjectRootAsNode), kObjectRootAsNodeBytes.data(), kObjectRootAsNodeBytes.size()) != 0) {
			logger::warn("[DCLF] scene lists: the build's object root call is not where expected; the lists are not kept");
			listsKeepInstalled = false;
		} else {
			SKSE::GetTrampoline().write_call<6>(base + kObjectRootAsNode, ListHooks::ObjectRootAsNode::thunk);
			listsKeepInstalled = true;
		}
		stl::detour_thunk<ListHooks::BuildSceneLists>(base + kBuildSceneLists);
		stl::detour_thunk<ListHooks::ClearList>(base + kClearList);
		stl::detour_thunk<ListHooks::CullList>(base + kCullList);
	}

	PrimaryCull::ListCounters PrimaryCull::TakeListCounters()
	{
		auto& l = listStats;
		ListCounters c;
		c.filtered = l.filtered.exchange(0), c.entries = l.entries.exchange(0), c.removed = l.removed.exchange(0);
		c.kept = l.kept.exchange(0), c.rebuilt = l.rebuilt.exchange(0), c.lateRebuilds = l.lateRebuilds.exchange(0), c.rebuildTicks = l.rebuildTicks.exchange(0);
		c.rebuildEntries = l.rebuildEntries.exchange(0);
		for (std::size_t i = 0; i < c.structureBy.size(); ++i)
			c.structureBy[i] = l.structureBy[i].exchange(0);
		c.eventsApplied = l.eventsApplied.exchange(0), c.eventsAdded = l.eventsAdded.exchange(0), c.eventsRemoved = l.eventsRemoved.exchange(0);
		c.eventsRebuilt = l.eventsRebuilt.exchange(0);
		c.frames = l.frames, c.published = l.published, c.built = l.built, c.dryRuns = l.dryRuns, c.notToggled = l.notToggled, c.notCurrent = l.notCurrent;
		c.noExclusion = l.noExclusion, c.noOcclusion = l.noOcclusion;
		c.restored = l.restored, c.restoreDetached = l.restoreDetached, c.unexcluded = l.unexcluded, c.lostWhileOut = l.lostWhileOut;
		c.engineFrames = l.engineFrames, c.notKeepable = l.notKeepable, c.notKeepableBy = l.notKeepableBy;
		c.rebuildStructure = l.rebuildStructure, c.rebuildFilter = l.rebuildFilter, c.rebuildWitness = l.rebuildWitness, c.rebuildDirty = l.rebuildDirty;
		l.frames = l.published = l.built = l.dryRuns = l.notToggled = l.notCurrent = l.noExclusion = l.noOcclusion = 0;
		l.restored = l.restoreDetached = l.unexcluded = l.lostWhileOut = l.engineFrames = l.notKeepable = 0;
		l.notKeepableBy = {};
		l.rebuildStructure = l.rebuildFilter = l.rebuildWitness = l.rebuildDirty = 0;
		return c;
	}
}
