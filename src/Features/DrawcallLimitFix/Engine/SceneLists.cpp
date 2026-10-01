#include "PrimaryCull.h"

#include "EngineAccess.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Common/Toggles.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "SunAccumulation.h"

#include <algorithm>
#include <atomic>

// The main camera's scene lists without DCLF's roots, and kept across frames (drawcall-limit-fix.md, "The scene lists
// without DCLF's roots"). PrimaryCull.h, PublishListFilter, describes the modes.

namespace DCLF
{
	namespace
	{
		using namespace Engine;

		// AE 1.6.1170 (module offsets).
		constexpr std::uintptr_t kSceneLists = 0x338c870;      // BSTArray<NiPointer<NiAVObject>>*
		constexpr std::uintptr_t kSceneListCount = 0x338c868;  // std::uint32_t
		constexpr std::uintptr_t kExtraList = 0x338c888;       // BSTArray<NiPointer<NiAVObject>>, culled by the first job only
		constexpr std::uintptr_t kListProcesses = 0x338c8a0;   // BSGeometryListCullingProcess**, one per scene list
		constexpr std::uintptr_t kBuildSceneLists = 0x64bc20;  // DrawWorld_BuildSceneLists, a job of Main::Draw's
		constexpr std::uintptr_t kClearList = 0x64cf20;        // FUN_14064cf20(list): Main::Draw's ClearLists job, one list
		constexpr std::uintptr_t kCullList = 0xe28f70;         // FUN_140e28f70(process, list, camera, skipHidden, jobs): every list cull
		// In DrawWorld_BuildSceneLists: `mov rax, [rcx]; call [rax + 0x18]`, the object root's AsNode (ShadowSceneNode child 3).
		constexpr std::uintptr_t kObjectRootAsNode = 0x64be5a;
		constexpr std::array<std::uint8_t, 6> kObjectRootAsNodeBytes{ 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18 };
		constexpr std::size_t kProcessPortalEntry = 0x30190;  // BSCullingProcess::portalGraphEntry
		constexpr std::size_t kTesWorldSpace = 0x148;         // TES: the current worldspace (FUN_14019d200)
		constexpr std::size_t kWorldSpaceFlags = 0xa0;        // byte; 0x40: the build skips category node 2
		constexpr std::size_t kObjectFlags = 0xF4;
		constexpr std::uint32_t kFlagHidden = 1u;
		constexpr std::uint32_t kFlagAccumulated = 1u << 26;

		using SceneList = RE::BSTArray<RE::NiPointer<RE::NiAVObject>>;

		bool Hidden(const RE::NiAVObject* a_object) { return (At<std::uint32_t>(a_object, kObjectFlags) & kFlagHidden) != 0; }

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
		if (a_extra && a_list == &Global<SceneList>(kExtraList))
			return true;
		const auto* lists = Global<SceneList*>(kSceneLists);
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		return lists && a_list >= lists && a_list < lists + count;
	}

	bool PrimaryCull::ListsKeepable(std::uint64_t& a_witness)
	{
		const auto fail = [&](std::size_t a_reason) {
			++listStats.notKeepableBy[a_reason];
			return false;
		};
		if (!SceneStore::HiddenEventsLive() || !listsKeepInstalled)
			return fail(0);
		const auto* sceneNode = SceneNode();
		auto** processes = Global<RE::NiCullingProcess**>(kListProcesses);
		const auto* tes = RE::TES::GetSingleton();
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		if (!sceneNode || !processes || !processes[0] || !tes || !count || count > listSentinels.size())
			return fail(1);
		// The interior branch (and with no unbound space, the water's) does not walk the object root.
		const auto* entry = At<const RE::BSPortalGraphEntry*>(processes[0], kProcessPortalEntry);
		if (!entry || !entry->visibleUnboundSpace)
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
		if (!toggles.ownership || !toggles.excludePrimaryEntries) {
			++listStats.notToggled;
			return nullptr;
		}
		// Precipitation::SetupMask culls every list when the engine draws an occlusion map: both must be DCLF's.
		if (!SceneStore::OcclusionEnabled(kOcclusionSky) || !SceneStore::OcclusionEnabled(kOcclusionPrecipitation)) {
			++listStats.noOcclusion;
			return nullptr;
		}
		auto& store = SceneStore::Get();
		if (!cut.candidates || cut.candidates->generation != store.GetSunCandidatesGeneration()) {
			++listStats.notCurrent;
			return nullptr;
		}
		// The exclusion this frame's full-frustum cull applies, for the same snapshot: a root out of the lists is out of
		// the sun's cascades only when that exclusion takes it out too.
		const auto exclusion = SunAccumulation::Get().PendingExclusion();
		if (!exclusion || exclusion->candidates != cut.candidates || exclusion->excluded.size() != cut.plans.size()) {
			++listStats.noExclusion;
			return nullptr;
		}
		// Built again only when an entry's verdict may have moved: the cut's admissions and walks, or the exclusion's content.
		const auto entries = static_cast<std::uint32_t>(cut.plans.size());
		if (listFilterBuilt && listFilterBuilt->candidates == cut.candidates && filterCutVersion == cutVersion && filterExclusionVersion == exclusion->version &&
			listRemovable.size() == entries)
			return listFilterBuilt;
		filterCutVersion = cutVersion;
		filterExclusionVersion = exclusion->version;
		bool changed = !listFilterBuilt || listFilterBuilt->candidates != cut.candidates || listRemovable.size() != entries;
		listRemovable.resize(entries, 0);
		for (std::uint32_t e = 0; e < entries; ++e) {
			const std::uint8_t removable = cut.plans[e] != EntryPlan::Rejected && cut.admitted[e] && !cut.walk[e] && !cut.mixed[e] && exclusion->excluded[e] ? 1 : 0;
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
		parityFilter.reset();
		listParity = false;
		if (!installed || SwitchValue(Switch::ListFilter) == "0" || Probe()) {
			++listStats.notToggled;
			listsDirty.store(true, std::memory_order_relaxed);
			return;
		}
		// The engine's decal order is the lists' (CS_DCLF_DECAL_ORDER=engine, and its probe): its own build, whole.
		if (SwitchValue(Switch::DecalOrder) == "engine" || SwitchEnabled(Switch::DecalOrderProbe)) {
			++listStats.decalOrder;
			listsDirty.store(true, std::memory_order_relaxed);
			return;
		}
		auto filter = CurrentListFilter();
		std::uint64_t witness = 0;
		const bool keepable = ListsKeepable(witness);
		// A parity frame: the engine's build, whole. The stand-in checks the roots the filter would have left out, and
		// CheckSceneLists the engine's lists against DCLF's enumeration.
		if (SwitchEnabled(Switch::PersistentParity) && ParityDue(SceneStore::Get().GetFrame())) {
			parityFilter = std::move(filter);
			listParity = keepable;
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
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto* lists = Global<SceneList*>(kSceneLists);
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
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto* lists = Global<SceneList*>(kSceneLists);
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
		const auto& scene = sceneNode->GetChildren();
		const auto* objectRoot = scene.free_idx() > 3 ? scene[3].get() : nullptr;
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
		if (const auto* objectNode = objectRoot ? const_cast<RE::NiAVObject*>(objectRoot)->AsNode() : nullptr; objectNode && !Hidden(objectNode)) {
			const auto& cells = objectNode->GetChildren();
			for (std::uint16_t i = 2; i < cells.free_idx(); ++i) {
				auto* cell = cells[i] ? cells[i]->AsNode() : nullptr;
				if (!cell || Hidden(cell))
					continue;
				const auto& categories = cell->GetChildren();
				for (std::uint16_t j = 0; j < categories.free_idx(); ++j)
					if (auto* category = categories[j] ? categories[j]->AsNode() : nullptr; category && CategoryListed(j, skipCategory2) && !Hidden(category))
						listCategories.emplace(category, j);
			}
		}
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
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto* lists = Global<SceneList*>(kSceneLists);
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
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto* lists = Global<SceneList*>(kSceneLists);
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
		const auto& scene = sceneNode->GetChildren();
		if (auto* objectRoot = scene.free_idx() > 3 && scene[3] ? scene[3]->AsNode() : nullptr; objectRoot && !Hidden(objectRoot))
			for (const auto& child : objectRoot->GetChildren())
				if (auto* node = child ? child->AsNode() : nullptr; node && !Hidden(node))
					std::atomic_ref<std::uint32_t>(At<std::uint32_t>(node, kObjectFlags)).fetch_or(kFlagAccumulated, std::memory_order_relaxed);
	}

	void PrimaryCull::CheckSceneLists()
	{
		// The engine's lists (whole: a parity frame) against DCLF's enumeration: every root of theirs is DCLF's, and every root
		// of DCLF's not theirs is hidden (the list cull skips it).
		const auto* sceneNode = SceneNode();
		const auto* tes = RE::TES::GetSingleton();
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto* lists = Global<SceneList*>(kSceneLists);
		if (!sceneNode || !tes || !lists || !count)
			return;
		const auto& scene = sceneNode->GetChildren();
		const auto* objectRoot = scene.free_idx() > 3 ? scene[3].get() : nullptr;
		// The object root's entries: a portal graph's rooms and occlusion planes, which the engine adds every frame, are not.
		const auto underRoot = [&](const RE::NiAVObject* a_object) {
			for (; a_object; a_object = a_object->parent)
				if (a_object->parent == objectRoot)
					return true;
			return false;
		};
		ankerl::unordered_dense::set<const RE::NiAVObject*> engine;
		for (std::uint32_t l = 0; l < count; ++l)
			for (const auto& entry : lists[l])
				if (entry && underRoot(entry.get()))
					engine.insert(entry.get());
		const auto* worldSpace = At<const std::byte*>(tes, kTesWorldSpace);
		const bool skipCategory2 = worldSpace && (At<std::uint8_t>(worldSpace, kWorldSpaceFlags) & 0x40);
		std::uint64_t missing = 0, surplus = 0;
		std::string first;
		const auto note = [&](const RE::NiAVObject* a_root, const char* a_what) {
			if (first.empty())
				first = fmt::format("{} '{}'", a_what, a_root && a_root->name.c_str() ? a_root->name.c_str() : "?");
		};
		const auto take = [&](ankerl::unordered_dense::set<const RE::NiAVObject*>& a_set, const RE::NiAVObject* a_root) {
			if (!a_set.erase(a_root) && !Hidden(a_root)) {
				++surplus;
				note(a_root, "DCLF's, not hidden, not the engine's");
			}
		};
		EnumerateSceneLists(objectRoot, skipCategory2, [&](const RE::NiAVObject* a_root, bool) { take(engine, a_root); }, [](const RE::NiAVObject*) {});
		for (const auto* root : engine) {
			++missing;
			if (first.empty()) {
				// Where it hangs: each node up to the scene with the child index it is under, and whether it is hidden.
				std::string chain;
				for (const RE::NiAVObject* object = root; object; object = object->parent) {
					int index = -1;
					if (const auto* parent = object->parent)
						for (std::uint16_t c = 0; c < parent->GetChildren().free_idx(); ++c)
							if (parent->GetChildren()[c].get() == object)
								index = c;
					chain += fmt::format(" <- {}:{}[{}]{}", object->GetRTTI() ? object->GetRTTI()->name : "?", object->name.c_str() ? object->name.c_str() : "", index,
						Hidden(object) ? " hidden" : "");
				}
				first = "the engine's, not DCLF's:" + chain;
			}
		}
		listStats.parityChecks.fetch_add(1, std::memory_order_relaxed);
		listStats.parityMissing.fetch_add(missing, std::memory_order_relaxed);
		listStats.parityExtra.fetch_add(surplus, std::memory_order_relaxed);
		if (!first.empty() && listStats.parityFirst.empty())
			listStats.parityFirst = first;
	}

	void PrimaryCull::RestoreSceneLists()
	{
		if (!listsFiltered.exchange(false, std::memory_order_acq_rel))
			return;
		const auto filter = listFilter.load(std::memory_order_acquire);
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto* lists = Global<SceneList*>(kSceneLists);
		if (!filter || !lists || !count)
			return;
		// The snapshot is current this frame (PublishListFilter), so its roots are alive; a hidden one is skipped by the cull.
		std::uint32_t l = 0;
		for (const auto* root : filter->roots)
			lists[l++ % count].push_back(RE::NiPointer<RE::NiAVObject>(const_cast<RE::NiAVObject*>(root)));
		listsDirty.store(true, std::memory_order_relaxed);
		++listStats.restored;
	}

	void PrimaryCull::NoteListStructure(const RE::NiNode* a_parent, RE::NiAVObject* a_child, bool a_attached)
	{
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
				const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
				auto* lists = Global<SceneList*>(kSceneLists);
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
					if (self.listParity)
						self.CheckSceneLists();
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

	void PrimaryCull::ReportSceneLists(std::uint64_t a_checked, std::uint64_t a_missed)
	{
		auto& l = listStats;
		const auto filtered = l.filtered.exchange(0), entries = l.entries.exchange(0), removed = l.removed.exchange(0);
		const auto kept = l.kept.exchange(0), rebuilt = l.rebuilt.exchange(0), late = l.lateRebuilds.exchange(0), ticks = l.rebuildTicks.exchange(0);
		const auto rebuildEntries = l.rebuildEntries.exchange(0);
		const auto parityChecks = l.parityChecks.exchange(0), parityMissing = l.parityMissing.exchange(0), parityExtra = l.parityExtra.exchange(0);
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart);
		const bool bad = l.unexcluded || a_missed || parityMissing || parityExtra;
		logger::info("[DCLF] scene lists: {} frames: {} kept, {} built by DCLF ({} late; by cause: {} structure or hidden ({}), {} filter, {} globals, {} not DCLF's), {} the engine's ({} not keepable: {}; {} filtered); "
					 "per DCLF build {:.0f} entries ({:.3f} ms), {:.0f} DCLF roots left out a build or filtered frame; filter sets built {} (not published: {} toggled off, {} decal order, {} occlusion maps native, {} snapshot not current, {} no exclusion); "
					 "dry runs {}: {} roots, {} stand-in disagreements; DCLF's lists against the engine's: {} checks, {} of the engine's missing, {} of DCLF's not hidden extra; "
					 "put back for an occlusion map the engine drew on {} frames; filtered frames without the sun's exclusion {}, members lost while out {}{}{}",
			l.frames, kept, rebuilt, late, l.rebuildStructure, fmt::format("events: {} object root children, {} category nodes, {} roots ({} applied: {} added, {} removed, {} rebuilt), {} hidden bits", l.structureBy[0].exchange(0),
				l.structureBy[1].exchange(0), l.structureBy[2].exchange(0), l.eventsApplied.exchange(0), l.eventsAdded.exchange(0), l.eventsRemoved.exchange(0),
				l.eventsRebuilt.exchange(0), l.structureBy[3].exchange(0)), l.rebuildFilter, l.rebuildWitness, l.rebuildDirty, l.engineFrames, l.notKeepable,
			fmt::format("{} no hidden events or call site, {} scene, {} not unbound space, {} interior, {} scene children", l.notKeepableBy[0], l.notKeepableBy[1],
				l.notKeepableBy[2], l.notKeepableBy[3], l.notKeepableBy[4]),
			filtered,
			rebuilt ? double(rebuildEntries) / rebuilt : 0.0, rebuilt ? ticks * toMs / rebuilt : 0.0, (rebuilt + filtered) ? double(removed) / double(rebuilt + filtered) : 0.0,
			l.built, l.notToggled, l.decalOrder, l.noOcclusion, l.notCurrent, l.noExclusion, l.dryRuns, a_checked, a_missed, parityChecks, parityMissing, parityExtra,
			l.restored, l.unexcluded, l.lostWhileOut, bad ? " <- LIST FILTER" : " <- OK", l.parityFirst.empty() ? "" : "; first: " + l.parityFirst);
		(void)entries;
		l.frames = l.published = l.built = l.dryRuns = l.notToggled = l.notCurrent = l.noExclusion = l.noOcclusion = l.decalOrder = 0;
		l.restored = l.unexcluded = l.lostWhileOut = l.engineFrames = l.notKeepable = 0;
		l.notKeepableBy = {};
		l.rebuildStructure = l.rebuildFilter = l.rebuildWitness = l.rebuildDirty = 0;
		l.parityFirst.clear();
	}
}
