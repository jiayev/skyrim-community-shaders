// PrimaryCull's scene lists (dclf-architecture.md, "The scene lists without DCLF's roots"): the filter published on the render
// thread (PublishListFilter, RestoreSceneLists), the build's job (DrawWorld_BuildSceneLists' detour: kept, rebuilt or the engine's
// build filtered), and the attach/detach detours' structure events (any thread). PrimaryCull.h, PublishListFilter, describes the modes.
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"

#include <algorithm>
#include <atomic>
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/PrimaryCullView.h"

namespace DCLF
{
	namespace
	{
		using namespace Engine;

		// Engine::Offsets (Address Library ids).
		constexpr Engine::Offset kBuildSceneLists{ { 36643 }, { 35630 } };  // DrawWorld_BuildSceneLists, a job of Main::Draw's
		constexpr Engine::Offset kClearList{ { 36655 }, { 35641 } };  // FUN_14064cf20(list): Main::Draw's ClearLists job, one list
		constexpr Engine::Offset kCullList{ { 76561 }, { 74811 } };  // FUN_140e28f70(process, list, camera, skipHidden, jobs): every list cull
		// In DrawWorld_BuildSceneLists: `mov rax, [rcx]; call [rax + 0x18]`, the object root's AsNode (ShadowSceneNode child 3).
		constexpr Engine::Offset kObjectRootAsNode{ { 36643, 0x23a }, { 35630, 0x239 } };
		constexpr std::array<std::uint8_t, 6> kObjectRootAsNodeBytes{ 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18 };
		// TES: the current worldspace (AE FUN_14019d200 reads +0x148, SE FUN_140153cf0 +0x140: TES is 8 bytes shorter before it on SE).
		inline std::size_t TesWorldSpace() { return REL::Module::IsSE() ? 0x140 : 0x148; }
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
		// unbound space is DCLF's walk's (the main cull input's, after Main::Update's: Features/Portals). The primary cull is installed
		// only with its walk, so an input is registered here.
		if (const auto* input = Extensions::MainCullInput(); input && !input->contribution.unboundVisible())
			return fail(2);
		if (tes->interiorCell)
			return fail(3);
		const auto& scene = sceneNode->GetChildren();
		if (scene.free_idx() < 4 || !scene[3])
			return fail(4);
		const auto* worldSpace = At<const std::byte*>(tes, TesWorldSpace());
		const bool skipCategory2 = worldSpace && (At<std::uint8_t>(worldSpace, kWorldSpaceFlags) & 0x40);
		a_witness = (std::uint64_t(count) << 1) ^ (skipCategory2 ? 1u : 0u) ^ (reinterpret_cast<std::uintptr_t>(scene[3].get()) << 8) ^
		            (std::uint64_t(scene.free_idx()) << 56);
		return true;
	}

	bool PrimaryCull::ListFilterReady(bool& a_sunOff, Api::ShadowListExclusion& a_exclusion)
	{
		// Entries to judge again pile up while no filter is made: past the index space, all are.
		if (filterDirty.size() > std::max<std::size_t>(cut.plans.size(), 1024)) {
			filterDirty.clear();
			filterDirtyAll = true;
		}
		const auto toggles = ActiveToggles();
		if (!Policy::WithholdFromEngine() || !toggles.excludePrimaryEntries) {
			++listStats.notToggled;
			return false;
		}
		// Precipitation::SetupMask culls every list when the engine draws an occlusion map: every map must be DCLF's.
		bool everyMap = true;
		for (std::uint32_t v = 0; v < kOcclusionViews && everyMap; ++v)
			everyMap = Extensions::OcclusionOn(ActiveToggles(), v);
		if (!everyMap) {
			++listStats.noOcclusion;
			return false;
		}
		// The cut as the last frame left it, brought up to the frame's candidates (it plans again only the entries that moved).
		TakePlans();
		if (const auto candidates = Scene::FrameView::Get().GetSunCandidates(); !candidates) {
			++listStats.notCurrent;
			return false;
		} else if (candidates != cut.candidates) {
			std::vector<std::uint32_t> changed;
			ChangedEntriesByLog(cut.candidates.get(), *candidates, changed);
			SyncCut(candidates, changed);
			cutStats.entriesPlanned += changed.size();
		}
		// The exclusion this frame's full-frustum cull applies, for the same snapshot: a root out of the lists is out of
		// the sun's cascades only when that exclusion takes it out too. With the sun off (an interior) no full-frustum cull
		// reads the lists, and no exclusion is needed. The exclusion is the contribution's (Features/Shadows/Local's SunExclusionSlot), read
		// through its plain view.
		const bool sunOff = Global<std::uint8_t>(Engine::Address::kSunOff) != 0;
		Api::ShadowListExclusion exclusion;
		if (!sunOff)
			Extensions::ListExclusion(exclusion);
		if (!sunOff && (!exclusion.held || exclusion.candidates != cut.candidates.get() || exclusion.excluded.size() != cut.plans.size())) {
			++listStats.noExclusion;
			return false;
		}
		a_sunOff = sunOff;
		a_exclusion = std::move(exclusion);
		return true;
	}

	bool PrimaryCull::MarkFilterDirty(std::uint32_t a_entries, bool a_sunOff, Api::ShadowListExclusion& a_exclusion)
	{
		// Judged again: the entries the cut changed (NoteFilterEntry) and those whose exclusion bit moved; all for a first filter.
		bool all = filterDirtyAll || !listFilterBuilt || a_sunOff != filterSunOff || (!a_sunOff && !filterExclusion.held);
		if (!all && !a_sunOff && a_exclusion.held != filterExclusion.held) {
			const auto was = filterExclusion.excluded;
			const auto now = a_exclusion.excluded;
			for (std::uint32_t e = 0; e < a_entries; ++e)
				if (e >= was.size() || was[e] != now[e])
					filterDirty.push_back(e);
		}
		// Entries past a shrunk index space leave the filter.
		if (!all)
			for (std::uint32_t e = a_entries; e < listReason.size(); ++e)
				filterDirty.push_back(e);
		filterDirtyAll = false;
		filterSunOff = a_sunOff;
		filterExclusion = std::move(a_exclusion);
		return all;
	}

	std::shared_ptr<PrimaryCull::ListFilter> PrimaryCull::JudgeFilterEntries(std::uint32_t a_entries, bool a_sunOff, bool a_all)
	{
		const auto reasonOf = [&](std::uint32_t a_e) -> std::uint8_t {
			if (a_e >= a_entries || !cut.roots[a_e] || cut.plans[a_e] == EntryPlan::Rejected)
				return kNoEntry;
			if (!cut.admitted[a_e])
				return kNotAdmitted;
			if (cut.mixed[a_e])
				return kMixed;
			if (cut.walk[a_e])
				return kWalked;
			if (!a_sunOff && !filterExclusion.excluded[a_e])
				return kNotExcluded;
			return kRemovable;
		};
		std::shared_ptr<ListFilter> next;
		const auto edit = [&]() -> ListFilter& {
			if (!next) {
				next = std::make_shared<ListFilter>();
				if (listFilterBuilt && !a_all) {
					next->roots = listFilterBuilt->roots;
					next->base = listFilterBuilt->version;
				}
			}
			return *next;
		};
		if (a_all) {
			edit();
			listReason.assign(a_entries, kNoEntry);
			listReasonRoot.assign(a_entries, nullptr);
			listReasonCount = {};
			listReasonCount[kNoEntry] = a_entries;
			for (std::uint32_t e = 0; e < a_entries; ++e)
				filterDirty.push_back(e);
		}
		if (listReason.size() < a_entries) {
			listReasonCount[kNoEntry] += a_entries - listReason.size();
			listReason.resize(a_entries, kNoEntry);
			listReasonRoot.resize(a_entries, nullptr);
		}
		// Erased first, then inserted: a root that moved to another index within the delta stays in.
		std::vector<std::pair<std::uint32_t, std::uint8_t>> judged;
		judged.reserve(filterDirty.size());
		for (const std::uint32_t e : filterDirty) {
			if (e >= listReason.size())
				continue;
			const std::uint8_t reason = reasonOf(e);
			const auto* root = e < a_entries ? cut.roots[e] : nullptr;
			++listStats.filterJudged;
			if (reason == listReason[e] && root == listReasonRoot[e])
				continue;
			if (listReason[e] == kRemovable && listReasonRoot[e]) {
				auto& filter = edit();
				if (filter.roots.erase(listReasonRoot[e])) {
					filter.erased.push_back(listReasonRoot[e]);
					++listStats.filterErased;
				}
			}
			--listReasonCount[listReason[e]];
			++listReasonCount[reason];
			listReason[e] = reason;
			listReasonRoot[e] = root;
			if (reason == kRemovable)
				judged.emplace_back(e, reason);
		}
		filterDirty.clear();
		for (const auto& [e, reason] : judged) {
			auto& filter = edit();
			if (filter.roots.insert(listReasonRoot[e]).second) {
				filter.inserted.push_back(listReasonRoot[e]);
				++listStats.filterInserted;
			}
		}
		return next;
	}

	std::shared_ptr<const PrimaryCull::ListFilter> PrimaryCull::CurrentListFilter()
	{
		bool sunOff = false;
		Api::ShadowListExclusion exclusion;
		if (!ListFilterReady(sunOff, exclusion))
			return nullptr;
		const auto entries = static_cast<std::uint32_t>(cut.plans.size());
		const bool all = MarkFilterDirty(entries, sunOff, exclusion);
		auto next = JudgeFilterEntries(entries, sunOff, all);
		// Entries past the index space are gone from it.
		if (listReason.size() > entries) {
			for (std::uint32_t e = entries; e < listReason.size(); ++e)
				--listReasonCount[listReason[e]];
			listReason.resize(entries);
			listReasonRoot.resize(entries);
		}
		// Every root a version lists is its entry's root in that version's candidates, which hold it; one erased is held by the base's.
		// No root moved: the last version stands (its candidates hold the same roots), whatever snapshot is current.
		if (next) {
			next->candidates = cut.candidates;
			next->version = ++listFilterVersion;
			listFilterBuilt = std::move(next);
			++listStats.built;
		}
		++listStats.reasonFrames;
		for (std::size_t r = 0; r < listReasonCount.size(); ++r)
			listStats.keptBy[r] += listReasonCount[r];
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
			// A new filter whose delta is from the kept lists' is applied to them (ApplyFilterDelta); any other rebuilds them.
			const bool filterMoved = filter != listFilterKept && !(filter && listFilterKept && filter->base && filter->base == listFilterKept->version);
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
		const auto* worldSpace = At<const std::byte*>(tes, TesWorldSpace());
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

	bool PrimaryCull::ApplyFilterDelta(const ListFilter& a_filter)
	{
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		auto* lists = Global<SceneList*>(Engine::Address::kSceneLists);
		if (!lists || !count)
			return false;
		// Put back: the roots the filter no longer leaves out, where the build lists them (under a kept category node, in the round
		// robin). Each is alive: the kept lists' filter (the delta's base) holds it. One detached since has left the scene; one under
		// anything else than a kept category node is the build's to place.
		std::uint64_t in = 0, out = 0;
		for (const auto* root : a_filter.erased) {
			if (listPositions.contains(root))
				continue;
			const auto* parent = root->parent;
			if (!parent)
				continue;
			const auto category = listCategories.find(parent);
			if (category == listCategories.end())
				return false;
			// Category node 6's first child is not listed (the build starts at 1).
			if (category->second == 6 && parent->GetChildren().free_idx() && parent->GetChildren()[0].get() == root)
				continue;
			const std::uint32_t l = listNext++ % count;
			listPositions.emplace(root, std::pair<std::uint32_t, std::uint32_t>(l, listKeptSize[l]));
			lists[l].push_back(RE::NiPointer<RE::NiAVObject>(const_cast<RE::NiAVObject*>(root)));
			++listKeptSize[l];
			++in;
		}
		// Take out: the roots it now leaves out (as a detach event).
		for (const auto* root : a_filter.inserted) {
			const auto it = listPositions.find(root);
			if (it == listPositions.end())
				continue;
			const auto [l, position] = it->second;
			if (l >= count || !listKeptSize[l] || position > listKeptSize[l] - 1 || lists[l][position].get() != root)
				return false;
			const std::uint32_t last = listKeptSize[l] - 1;
			listGraveyard.push_back(std::move(lists[l][position]));
			if (position != last) {
				lists[l][position] = std::move(lists[l][last]);
				listPositions[lists[l][position].get()].second = position;
			}
			lists[l].resize(last);
			listKeptSize[l] = last;
			listPositions.erase(it);
			++out;
		}
		listStats.deltaIn.fetch_add(in, std::memory_order_relaxed);
		listStats.deltaOut.fetch_add(out, std::memory_order_relaxed);
		return true;
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
		const auto* worldSpace = At<const std::byte*>(tes, TesWorldSpace());
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
		// The filter's roots are alive: its candidates hold them (EntrySpace::held). One the engine detached since the snapshot
		// was made stays out (it has left the scene); a hidden one is skipped by the cull. Taking a reference to a root the
		// snapshot did not hold is what put freed roots into the lists, released at Present (dclf-architecture.md, "Engine references").
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
		// The stand-in observers' structure first (the point lights' category filter, Features/Shadows/Local), then the cull input's.
		Extensions::StandInStructure(a_parent);
		if (const auto* input = Extensions::MainCullInput())
			input->contribution.structure(a_parent);
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
					// The events against the filter the kept lists were made with, then the new filter's delta from it.
					const auto filter = self.listFilter.load(std::memory_order_acquire);
					bool kept = self.ApplyListEvents(self.listFilterKept.get());
					if (kept && filter != self.listFilterKept) {
						kept = filter && self.listFilterKept && filter->base == self.listFilterKept->version && self.ApplyFilterDelta(*filter);
						self.listStats.deltaKept.fetch_add(kept ? 1 : 0, std::memory_order_relaxed);
						self.listStats.deltaRebuilt.fetch_add(kept ? 0 : 1, std::memory_order_relaxed);
						if (kept)
							self.listFilterKept = filter;
					}
					if (kept) {
						self.listStats.kept.fetch_add(1, std::memory_order_relaxed);
						if (Diagnostics::Armed<Diagnostics::KeptSceneListsBuilt>())
							Diagnostics::Notify(Diagnostics::KeptSceneListsBuilt{ self.listFilterKept ? &self.listFilterKept->roots : nullptr, self.listKeptSize.data(), count });
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
		if (!kBuildSceneLists.Known() || !kClearList.Known() || !kCullList.Known()) {
			logger::warn("[DCLF] scene lists: the build, clear and cull functions are not known for this runtime; the lists stay the engine's");
			return;
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
		c.deltaKept = l.deltaKept.exchange(0), c.deltaOut = l.deltaOut.exchange(0), c.deltaIn = l.deltaIn.exchange(0), c.deltaRebuilt = l.deltaRebuilt.exchange(0);
		c.filterJudged = l.filterJudged, c.filterInserted = l.filterInserted, c.filterErased = l.filterErased;
		c.reasonFrames = l.reasonFrames, c.keptBy = l.keptBy;
		l.filterJudged = l.filterInserted = l.filterErased = l.reasonFrames = 0;
		l.keptBy = {};
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
