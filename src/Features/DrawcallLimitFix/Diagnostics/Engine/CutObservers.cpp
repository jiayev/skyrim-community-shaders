// The primary cull's observers (CutObservers.h): the render thread's at PrimaryCull's hook points, and the list jobs' per-slot counts
// on their job threads (read after Finish).

#include "CutObservers.h"

#include "Events.h"
#include "PrimaryCullView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

#include <array>

namespace DCLF::Diagnostics::CutObservers
{
	namespace
	{
		using Engine::At;
		using Engine::Global;

		constexpr std::size_t kLightData = Engine::kPropertyLightData;

		/** @brief One list job's counts: written by its job thread only, read after Finish. */
		struct Slot
		{
			std::uint64_t walkMissed = 0, filterChecked = 0, filterMissed = 0;
			std::string walkMissedFirst;
		};
		std::array<Slot, 16> slots;
		Counts counts;  // render thread (the list parity's: the build's job, read after Finish)

		// The scene lists' whole frames: the roots the filter would have left out (read by the list jobs), and whether the engine's
		// build is checked.
		std::shared_ptr<const void> dryHold;
		const ankerl::unordered_dense::set<const RE::NiAVObject*>* dryRoots = nullptr;
		bool checkLists = false;

		// The cut parity: the roots of the entries that differed from the whole plan at the last check (CheckCut).
		ankerl::unordered_dense::set<const RE::NiAVObject*> cutDiffering;
		std::uint32_t sunBits = 0;  // the sun's cascade bits as the list jobs ended (the light masks')
		std::uint32_t sunBitsNow = 0;  // ... as last set (SunBitsSet)

		// ---- The cut.

		void CheckCut(const On<EnginePoint::CutSynced>&)
		{
			if (!ParityDue(Scene::FrameView::Get().GetFrame(), 11))
				return;
			const auto& cut = PrimaryCullView::Cut();
			if (!cut.candidates)
				return;
			const auto whole = PrimaryCullView::PlanWholeCut();
			++counts.cutChecks;
			std::string first;
			std::uint32_t differ = 0;
			// The whole plan walks the subtrees as they are now; the kept one as they were when planned. An engine change under a root
			// reaches the cut with the snapshot that follows it, so an entry differing at one check only is the snapshot's lag behind the
			// engine (counted); one whose root differs at two checks in a row (a second apart) is a finding.
			ankerl::unordered_dense::set<const RE::NiAVObject*> differing;
			std::uint32_t lagging = 0;
			std::string firstLagging;
			auto note = [&](std::uint32_t a_e, const char* a_what) {
				const auto* root = a_e < cut.roots.size() ? cut.roots[a_e] : nullptr;
				const bool entry = a_e < cut.roots.size() && root;
				if (entry) {
					const bool added = differing.insert(root).second;
					if (!cutDiffering.contains(root)) {
						if (added && !lagging++)
							firstLagging = fmt::format("entry {} ({}): {}", a_e, static_cast<const void*>(root), a_what);
						return false;
					}
				}
				if (!differ++)
					first = fmt::format("entry {} ({}): {}", a_e, static_cast<const void*>(root), a_what);
				return true;
			};
			if (whole.roots.size() != cut.roots.size() && !differ++)
				first = "the index space";
			// Entries waiting for their plan from the pool are Rejected meanwhile (the engine culls them): not compared.
			std::size_t awaitingEligible = 0;
			for (std::uint32_t e = 0; e < std::min(whole.roots.size(), cut.roots.size()); ++e) {
				if (e < cut.awaiting.size() && cut.awaiting[e]) {
					awaitingEligible += !PrimaryCullView::RejectedPlan(whole.plans[e]) ? 1 : 0;
					continue;
				}
				if (whole.roots[e] != cut.roots[e] || whole.plans[e] != cut.plans[e]) {
					const bool finding = note(e, "root or plan");
					if (finding ? differ == 1 : lagging == 1) {
						const auto* root = cut.roots[e];
						(finding ? first : firstLagging) += fmt::format(": root {} / {}, plan {} kept, {} whole; members {} kept, {} whole; '{}' {}", static_cast<const void*>(cut.roots[e]),
							static_cast<const void*>(whole.roots[e]), static_cast<int>(cut.plans[e]), static_cast<int>(whole.plans[e]), cut.memberEnd[e] - cut.memberBegin[e],
							whole.memberEnd[e] - whole.memberBegin[e], root && root->name.c_str() ? root->name.c_str() : "?", root ? root->GetRTTI() ? root->GetRTTI()->name : "?" : "");
					}
					continue;
				}
				if (whole.mixed[e] != cut.mixed[e])
					note(e, "mixed");
				const auto members = cut.memberEnd[e] - cut.memberBegin[e];
				if (whole.memberEnd[e] - whole.memberBegin[e] != members) {
					note(e, "member count");
					continue;
				}
				for (std::uint32_t i = 0; i < members; ++i) {
					const auto& a = cut.members[cut.memberBegin[e] + i];
					const auto& b = whole.members[whole.memberBegin[e] + i];
					const bool samePath = std::equal(cut.switchPaths.begin() + a.switchBegin, cut.switchPaths.begin() + a.switchEnd, whole.switchPaths.begin() + b.switchBegin,
						whole.switchPaths.begin() + b.switchEnd);
					if (a.geometry != b.geometry || a.engine != b.engine || !samePath || cut.memberObject[cut.memberBegin[e] + i] != whole.memberObject[whole.memberBegin[e] + i]) {
						note(e, "a member");
						break;
					}
				}
				if (!std::equal(cut.switches.begin() + cut.switchBegin[e], cut.switches.begin() + cut.switchEnd[e], whole.switches.begin() + whole.switchBegin[e],
						whole.switches.begin() + whole.switchEnd[e]))
					note(e, "switches");
			}
			// The eligible count follows the plans: a finding only when no entry is lagging.
			if (!lagging && whole.eligible.size() != cut.eligible.size() + awaitingEligible && !differ++)
				first = "the eligible roots";
			cutDiffering = std::move(differing);
			counts.cutLagging += lagging;
			if (lagging) {
				static std::uint32_t loggedLagging = 0;
				if (loggedLagging++ < 8)
					logger::info("[DCLF] cut parity at frame {}: {} entries differ from a whole plan once (the engine moved under them since their snapshot; "
								 "a finding if they still differ at the next check); first {}",
						Scene::Coordinator::Get().GetFrame(), lagging, firstLagging);
			}
			if (differ) {
				++counts.cutMismatches;
				static std::uint32_t logged = 0;
				if (logged++ < 8)
					logger::warn("[DCLF] cut parity at frame {}: {} entries differ from a whole plan; first {} <- CUT", Scene::Coordinator::Get().GetFrame(), differ, first);
			}
		}

		// ---- The list jobs (each slot on its job's thread).

		void ResetSlots(const CutSlotsReset& a_event)
		{
			for (std::uint32_t i = 0; i < a_event.count && i < slots.size(); ++i)
				slots[i] = {};
		}

		bool Dry(std::uint32_t a_entry)
		{
			return dryRoots && dryRoots->contains(PrimaryCullView::Cut().roots[a_entry]);
		}

		/** @brief A whole frame's dry run of the list filter: a root it would have left out must be one the stand-in does nothing for. */
		void EntryReached(const StandInEntry& a_event)
		{
			if (!Dry(a_event.entry))
				return;
			const auto& cut = PrimaryCullView::Cut();
			auto& slot = slots[a_event.slot];
			++slot.filterChecked;
			slot.filterMissed += !cut.admitted[a_event.entry] || cut.walk[a_event.entry] ? 1 : 0;
		}

		void HandedOver(const StandInHandedOver& a_event)
		{
			const auto& cut = PrimaryCullView::Cut();
			auto& slot = slots[a_event.slot];
			const std::uint32_t e = a_event.entry;
			// An entry only Policy::WalkEveryEntry walks (Cut::walk clear): normally nothing would have handed this one over.
			if (!cut.walk[e]) {
				++slot.walkMissed;
				// What the walk's decision missed (the first few a job, a frame).
				if (slot.walkMissed <= 4) {
					const auto& view = Scene::FrameView::Get().GetTables();
					const auto* geometry = a_event.geometry;
					const auto& member = cut.members[a_event.member];
					const std::int32_t object = cut.memberObject[a_event.member];
					const auto* atSlot = object >= 0 && static_cast<std::size_t>(object) < view.objectGeometry.size() ? view.objectGeometry[object] : nullptr;
					const auto& frameSet = PrimaryCullView::FrameSet();
					slot.walkMissedFirst += fmt::format("{}'{}' (entry {} '{}', {}): slot {} {}, set phases {:#x}, resident {}, claimed {:#x}, admitted {}",
						slot.walkMissedFirst.empty() ? "" : "; ", geometry->name.c_str() ? geometry->name.c_str() : "", e,
						cut.roots[e] && cut.roots[e]->name.c_str() ? cut.roots[e]->name.c_str() : "", member.engine ? "the engine's" : "DCLF's", object,
						atSlot == geometry ? "holds it" : atSlot ? "holds another" : "empty", Scene::FrameView::Get().SetPhasesOf(object), Scene::FrameView::Get().IsMember(object),
						frameSet ? frameSet->PhasesOf(geometry) : 0xFF, cut.admitted[e]);
				}
			}
			slot.filterMissed += Dry(e) ? 1 : 0;
		}

		void NoteSunBits(const SunBitsSet& a_event)
		{
			sunBitsNow = a_event.bits;
		}

		void Merge(const On<EnginePoint::ListJobsMerged>&)
		{
			sunBits = sunBitsNow;
			const auto count = PrimaryCullView::Cut().processCount;
			for (std::uint32_t i = 0; i < count && i < slots.size(); ++i) {
				auto& slot = slots[i];
				counts.walkMissed += slot.walkMissed;
				counts.filterChecked += slot.filterChecked;
				counts.filterMissed += slot.filterMissed;
				if (counts.walkMissedFirst.size() < 2000 && !slot.walkMissedFirst.empty())
					counts.walkMissedFirst += fmt::format(" [frame {}] {}", Scene::FrameView::Get().GetFrame(), slot.walkMissedFirst);
			}
		}

		/** @brief After the registration jobs: a property an engine-drawn geometry shares holds that one's bits until its main registration. */
		void CheckLightMasks(const On<FramePoint::RegistrationsDone>&)
		{
			const auto& cull = PrimaryCull::Get();
			for (const auto* geometry : PrimaryCullView::FrameVisible())
				if (cull.Owned(*geometry))
					if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get())
						if (void* lightData = At<void*>(property, kLightData)) {
							const auto mask = At<std::uint32_t>(lightData, Engine::kLightDataActiveMask);
							++counts.maskChecked;
							counts.maskSun += (mask & sunBits) ? 1 : 0;
							counts.maskOther += (mask & ~sunBits) ? 1 : 0;
							if (mask && counts.maskFirst.empty())
								counts.maskFirst = fmt::format("'{}' mask {:#x} (sun {:#x}){}", geometry->name.c_str() ? geometry->name.c_str() : "?", mask, sunBits, "");
						}
		}

		// ---- The scene lists.

		void ListsPublishing(const On<EnginePoint::SceneListsPublishing>&)
		{
			dryHold.reset();
			dryRoots = nullptr;
			checkLists = false;
		}

		void ListsWhole(const SceneListsWhole& a_event)
		{
			dryHold = a_event.hold;
			dryRoots = a_event.roots;
			checkLists = a_event.keepable;
		}

		/** @brief The build's job: the engine's lists (whole) against DCLF's enumeration: every root of theirs is DCLF's, every root of DCLF's not theirs hidden. */
		void CheckLists(const On<EnginePoint::EngineSceneListsBuilt>&)
		{
			if (!checkLists)
				return;
			const RE::NiAVObject* objectRoot = nullptr;
			std::vector<const RE::NiAVObject*> roots;
			const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
			auto* lists = Global<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>*>(Engine::Address::kSceneLists);
			if (!lists || !count || !PrimaryCullView::ListRoots(objectRoot, roots))
				return;
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
			std::uint64_t missing = 0, surplus = 0;
			std::string first;
			const auto note = [&](const RE::NiAVObject* a_root, const char* a_what) {
				if (first.empty())
					first = fmt::format("{} '{}'", a_what, a_root && a_root->name.c_str() ? a_root->name.c_str() : "?");
			};
			for (const auto* root : roots)
				if (!engine.erase(root) && !Engine::IsHidden(root)) {
					++surplus;
					note(root, "DCLF's, not hidden, not the engine's");
				}
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
							Engine::IsHidden(object) ? " hidden" : "");
					}
					first = "the engine's, not DCLF's:" + chain;
				}
			}
			++counts.listChecks;
			counts.listMissing += missing;
			counts.listExtra += surplus;
			if (!first.empty() && counts.listFirst.empty())
				counts.listFirst = first;
		}

		/** @brief The build's job, a kept frame: the kept lists hold exactly what RebuildSceneLists would list (hidden roots included). */
		void CheckKeptLists(const KeptSceneListsBuilt& a_event)
		{
			const RE::NiAVObject* objectRoot = nullptr;
			std::vector<const RE::NiAVObject*> roots;
			auto* lists = Global<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>*>(Engine::Address::kSceneLists);
			if (!lists || !PrimaryCullView::ListRoots(objectRoot, roots))
				return;
			ankerl::unordered_dense::set<const RE::NiAVObject*> kept;
			for (std::uint32_t l = 0; l < a_event.count; ++l)
				for (std::uint32_t r = 0; r < a_event.kept[l] && r < lists[l].size(); ++r)
					if (const auto* entry = lists[l][r].get(); entry && entry->parent)  // the sentinels have no parent
						kept.insert(entry);
			std::uint64_t missing = 0, extra = 0;
			std::string first;
			const auto note = [&](const RE::NiAVObject* a_root, const char* a_what) {
				if (first.empty())
					first = fmt::format("{} '{}'", a_what, a_root && a_root->name.c_str() ? a_root->name.c_str() : "?");
			};
			for (const auto* root : roots) {
				if (a_event.filter && a_event.filter->contains(root))
					continue;
				if (!kept.erase(root)) {
					++missing;
					note(root, "a rebuild's, not kept:");
				}
			}
			for (const auto* root : kept) {
				++extra;
				note(root, "kept, not a rebuild's:");
			}
			++counts.keptChecks;
			counts.keptMissing += missing;
			counts.keptExtra += extra;
			if (!first.empty() && counts.keptFirst.empty())
				counts.keptFirst = first;
		}
	}

	void Install(bool a_scene)
	{
		if (a_scene) {
			Subscribe<EnginePoint::CutSynced>(CheckCut);
			Subscribe<StandInEntry>(EntryReached);
			Subscribe<StandInHandedOver>(HandedOver);
			Subscribe<FramePoint::RegistrationsDone>(CheckLightMasks);
			PrimaryCullView::CollectVisible();
			Subscribe<EnginePoint::SceneListsPublishing>(ListsPublishing);
			Subscribe<SceneListsWhole>(ListsWhole);
			Subscribe<EnginePoint::EngineSceneListsBuilt>(CheckLists);
			Subscribe<KeptSceneListsBuilt>(CheckKeptLists);
		}
		if (a_scene) {
			Subscribe<CutSlotsReset>(ResetSlots);
			Subscribe<SunBitsSet>(NoteSunBits);
			Subscribe<EnginePoint::ListJobsMerged>(Merge);
		}
	}

	Counts Take()
	{
		return std::exchange(counts, {});
	}
}
