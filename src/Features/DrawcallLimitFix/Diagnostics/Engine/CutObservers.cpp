#include "CutObservers.h"

#include "Events.h"
#include "PrimaryCullView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/Views/PortalViews.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

#include <array>

namespace DCLF::Diagnostics::CutObservers
{
	namespace
	{
		using Engine::At;
		using Engine::Global;

		// The fade node's fields the visibility check reads (BSFadeNode).
		constexpr std::uint32_t kFlagFadeSettled = 1u << 15;
		constexpr std::size_t kFadeAmount = 0x100;
		constexpr std::size_t kCurrentFade = 0x130;
		constexpr std::size_t kLastVisibleFrame = 0x13C;
		constexpr std::size_t kFadeMetric = 0x144;
		constexpr std::size_t kLightData = Engine::kPropertyLightData;

		/** @brief One list job's counts: written by its job thread only, read after Finish. */
		struct Slot
		{
			std::uint64_t walkMissed = 0, filterChecked = 0, filterMissed = 0, visibilityChecked = 0, visibilityDiffer = 0;
			std::string walkMissedFirst, visibilityFirst;
		};
		std::array<Slot, 16> slots;
		Counts counts;  // render thread (the list parity's: the build's job, read after Finish)

		// The scene lists' whole frames: the roots the filter would have left out (read by the list jobs), and whether the engine's
		// build is checked.
		std::shared_ptr<const void> dryHold;
		const ankerl::unordered_dense::set<const RE::NiAVObject*>* dryRoots = nullptr;
		bool checkLists = false;

		std::uint32_t sunBits = 0;  // the sun's cascade bits as the list jobs ended (the light masks')
		std::uint64_t frames = 0;   // PrepareFrame's frames (the fade port's every 30th)
		std::uint32_t fadePortCursor = 0;
		std::vector<FadeNodeState> nodeSnapshot;
		std::uint32_t nodeSnapshotFrame = ~0u;

		// ---- The cut.

		void CheckCut(const CutSynced& a_event)
		{
			if (!ParityDue(a_event.frame, 11))
				return;
			const auto& cut = PrimaryCullView::Cut();
			if (!cut.candidates)
				return;
			const auto whole = PrimaryCullView::PlanWholeCut();
			++counts.cutChecks;
			std::string first;
			std::uint32_t differ = 0;
			auto note = [&](std::uint32_t a_e, const char* a_what) {
				if (!differ++)
					first = fmt::format("entry {} ({}): {}", a_e, static_cast<const void*>(a_e < cut.roots.size() ? cut.roots[a_e] : nullptr), a_what);
			};
			if (whole.roots.size() != cut.roots.size())
				note(0, "the index space");
			for (std::uint32_t e = 0; e < std::min(whole.roots.size(), cut.roots.size()); ++e) {
				if (whole.roots[e] != cut.roots[e] || whole.plans[e] != cut.plans[e]) {
					if (!differ) {
						const auto* root = cut.roots[e];
						note(e, "");
						first += fmt::format("root {} / {}, plan {} kept, {} whole; members {} kept, {} whole; '{}' {}", static_cast<const void*>(cut.roots[e]),
							static_cast<const void*>(whole.roots[e]), static_cast<int>(cut.plans[e]), static_cast<int>(whole.plans[e]), cut.memberEnd[e] - cut.memberBegin[e],
							whole.memberEnd[e] - whole.memberBegin[e], root && root->name.c_str() ? root->name.c_str() : "?", root ? root->GetRTTI() ? root->GetRTTI()->name : "?" : "");
					} else {
						note(e, "root or plan");
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
			if (whole.eligible.size() != cut.eligible.size())
				note(0, "the eligible roots");
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

		/** @brief The engine's verdict (its OnVisible ran: the node's metric or last visible frame changed) against FadeStateCS's test. */
		void MixedCulled(const MixedEntryCulled& a_event)
		{
			auto* object = a_event.root;
			// A settled node, its fades at 1, returns before updating anything: nothing tells.
			const bool settled = a_event.fadeRoot && (object->GetFlags().underlying() & kFlagFadeSettled) && At<float>(object, kFadeAmount) == 1.0f &&
			                     At<float>(object, kCurrentFade) == 1.0f;
			if (!a_event.fadeRoot || settled || a_event.slot < 0)
				return;
			// A root DCLF writes keeps its node through OnVisible (FadeOnVisible), so the engine's verdict is the kAccumulated Process1 wrote
			// after it.
			const bool engine = PrimaryCull::Get().FadeWrittenByDclf(object) ? (object->GetFlags().underlying() & (1u << 26)) != 0 :
			                                                                   At<float>(object, kFadeMetric) != a_event.metricBefore || At<std::int32_t>(object, kLastVisibleFrame) != a_event.visibleBefore;
			const float centre[3]{ object->worldBound.center.x, object->worldBound.center.y, object->worldBound.center.z };
			std::string why;
			auto& portals = PortalViews::Get();
			const int port = PortalViews::Visible(portals.Encoded(), portals.ProgramOf(object), centre, object->worldBound.radius, object->GetFlags().underlying(), why);
			if (port < 0)
				return;
			auto& slot = slots[a_event.slot];
			++slot.visibilityChecked;
			if ((port != 0) != engine && slot.visibilityDiffer++ == 0)
				slot.visibilityFirst = fmt::format("'{}' slot {}: the engine {}, the port {} ({}); centre ({:.0f} {:.0f} {:.0f}) r {:.0f}, flags {:#x}",
					object->name.c_str() ? object->name.c_str() : "", a_event.slot, engine ? "visible" : "culled", port ? "visible" : "culled", why, centre[0], centre[1],
					centre[2], object->worldBound.radius, object->GetFlags().underlying());
		}

		void Merge(const ListJobsMerged& a_event)
		{
			sunBits = SunAccumulation::Get().SunBits();
			const auto count = PrimaryCullView::Cut().processCount;
			for (std::uint32_t i = 0; i < count && i < slots.size(); ++i) {
				auto& slot = slots[i];
				counts.walkMissed += slot.walkMissed;
				counts.filterChecked += slot.filterChecked;
				counts.filterMissed += slot.filterMissed;
				counts.visibilityChecked += slot.visibilityChecked;
				counts.visibilityDiffer += slot.visibilityDiffer;
				if (counts.visibilityFirst.empty() && !slot.visibilityFirst.empty())
					counts.visibilityFirst = slot.visibilityFirst;
				if (counts.walkMissedFirst.size() < 2000 && !slot.walkMissedFirst.empty())
					counts.walkMissedFirst += fmt::format(" [frame {}] {}", a_event.frame, slot.walkMissedFirst);
			}
		}

		/** @brief After the registration jobs: a property an engine-drawn geometry shares holds that one's bits until its main registration. */
		void CheckLightMasks(const MainRegistrationsDone&)
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

		// ---- The fade.

		void CountFrame(const CutFrameBegin&)
		{
			++frames;
		}

		/** @brief The port against the engine's functions on copies of the snapshot's fade roots (nothing writes them until the list jobs). */
		void CheckFadePort(const FullFrustumDone&)
		{
			if (!PrimaryCullView::StandInLive() || (frames % 30) != 0)
				return;
			const auto& cut = PrimaryCullView::Cut();
			auto** processes = Global<RE::NiCullingProcess**>(Engine::Address::kListProcesses);
			const auto* camera = processes && cut.processCount ? processes[0]->camera : nullptr;
			if (!camera)
				return;
			std::uint32_t checked = 0;
			const auto entries = static_cast<std::uint32_t>(cut.roots.size());
			for (std::uint32_t n = 0; n < entries && checked < 64; ++n) {
				const std::uint32_t e = (fadePortCursor + n) % entries;
				if (!PrimaryCullView::FadePlan(e))
					continue;
				FadeState::CheckPort(*cut.roots[e], *camera, counts.fadePort);
				++checked;
			}
			fadePortCursor = entries ? (fadePortCursor + 64) % entries : 0;
		}

		/** @brief The fade roots' nodes now, after this cull's OnVisible and before the next animation job's updates. */
		void SnapshotNodes(const ListJobsDone& a_event)
		{
			if ((a_event.frame % 30) != 0)
				return;
			const auto& roots = Scene::FrameView::Get().GetTables().fadeRootNode;
			nodeSnapshot.resize(roots.size());
			for (std::size_t r = 0; r < roots.size(); ++r)
				nodeSnapshot[r] = roots[r] ? FadeState::ReadNode(*static_cast<const RE::NiAVObject*>(roots[r])) : FadeNodeState{};
			nodeSnapshotFrame = a_event.frame;
		}

		// ---- The scene lists.

		void ListsPublishing(const SceneListsPublishing&)
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
		void CheckLists(const EngineSceneListsBuilt&)
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
	}

	void Install(bool a_scene, bool a_fade)
	{
		if (a_scene) {
			Subscribe<CutSynced>(CheckCut);
			Subscribe<StandInEntry>(EntryReached);
			Subscribe<StandInHandedOver>(HandedOver);
			Subscribe<MainRegistrationsDone>(CheckLightMasks);
			Subscribe<SceneListsPublishing>(ListsPublishing);
			Subscribe<SceneListsWhole>(ListsWhole);
			Subscribe<EngineSceneListsBuilt>(CheckLists);
		}
		if (a_fade) {
			Subscribe<MixedEntryCulled>(MixedCulled);
			Subscribe<CutFrameBegin>(CountFrame);
			Subscribe<FullFrustumDone>(CheckFadePort);
			Subscribe<ListJobsDone>(SnapshotNodes);
		}
		if (a_scene || a_fade) {
			Subscribe<CutSlotsReset>(ResetSlots);
			Subscribe<ListJobsMerged>(Merge);
		}
	}

	const std::vector<FadeNodeState>* NodeSnapshot(std::uint32_t a_frame)
	{
		return nodeSnapshotFrame == a_frame ? &nodeSnapshot : nullptr;
	}

	Counts Take()
	{
		return std::exchange(counts, {});
	}
}
