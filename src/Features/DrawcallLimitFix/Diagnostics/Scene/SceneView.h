#pragma once

#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

namespace DCLF::Diagnostics
{
	/**
	 * @brief The scene area's observers and reports read Coordinator's private state through this view (Coordinator befriends it): each
	 * accessor names one member. Const for the observers; the reports take (and reset) the counters they read, which is the only
	 * write through it.
	 */
	struct SceneView
	{
		using Store = Scene::Coordinator;
		using Tracked = Scene::Coordinator::Tracked;
		using Tables = SceneTables;

		// The scene: the tracked set, the tables, the mirror, the pass's clocks.
		static const auto& tracked(const Store& s) { return s.tracked; }
		static const Tables& tables(const Store& s) { return s.tables; }
		static const auto& mirror(const Store& s) { return s.mirror; }
		static auto& mirror(Store& s) { return s.mirror; }
		static std::uint32_t sceneFrame(const Store& s) { return s.sceneFrame; }
		static std::uint64_t passSerial(const Store& s) { return s.passSerial; }
		static std::uint32_t tablesGeneration(const Store& s) { return s.tablesGeneration; }
		static std::uint32_t walkSerial(const Store& s) { return s.walkSerial; }
		static const auto& perFrameSet(const Store& s) { return s.perFrameSet; }
		static const RE::NiNode* firstViewRoot(const Store& s) { return s.FirstViewRoot(); }

		// The render thread's (Scene::FrameView): its frame, its captures' and constants' counters.
		using Frame = Scene::FrameView;
		static std::uint32_t frame() { return Frame::Get().frame; }
		static const auto& projectedTextures() { return Frame::Get().projectedTextures; }
		static auto& geometryStats() { return Frame::Get().geometryStats; }
		static auto& pipelinesPosted() { return Frame::Get().pipelinesPosted; }
		static auto& shadingFrames() { return Frame::Get().shadingFrames; }
		static auto& captureNs() { return Frame::Get().captureNs; }
		static auto& categoryCaptureNs() { return Frame::Get().categoryCaptureNs; }
		static auto& categoryCapturesMade() { return Frame::Get().categoryCapturesMade; }
		static auto& captureFrames() { return Frame::Get().captureFrames; }
		static auto& categoryMirrorCaptures() { return Frame::Get().categoryMirrorCaptures; }
		static auto& categoryMirrorRecords() { return Frame::Get().categoryMirrorRecords; }
		static RE::BSGraphics::BSShaderAccumulator* latchedAccumulator() { return Frame::Get().latchedAccumulator; }

		// The report counters (taken by the reports).
		static auto& pumpStats(Store& s) { return s.pumpStats; }
		static auto& stats(Store& s) { return s.stats; }
		static auto& residentStats(Store& s) { return s.residentStats; }
		static auto& delta(Store& s) { return s.delta; }
		static auto& materialRecordStats(Store& s) { return s.materialRecordStats; }
		static auto& materialSnapshotStats(Store& s) { return s.materialSnapshotStats; }
		static const auto& materialSnapshots(const Store& s) { return s.materialSnapshots; }
		static auto& materialBindings(Store& s) { return s.materialBindings; }
		static auto& sharedBindings(Store& s) { return s.sharedBindings; }
		static const auto& lookups(const Store& s) { return s.lookups; }
		static auto& reportedChangeCounts(Store& s) { return s.reportedChangeCounts; }
		// The features' and the components' text lines at a coordinator report point (Coordinator::ComponentsReportText).
		static void componentsReportText(Store& s, Api::ReportPoint a_point, std::string& a_text) { s.ComponentsReportText(a_point, a_text); }
		// ... and their logged lines at a logged one (Coordinator::ComponentsReportLines).
		static void componentsReportLines(Store& s, Api::ReportPoint a_point, std::uint32_t a_frame) { s.ComponentsReportLines(a_point, a_frame); }
		static auto& referenceStats(Store& s) { return s.referenceStats; }
		static auto& leafStats(Store& s) { return s.leafStats; }
		static auto& subtreesPended(Store& s) { return s.subtreesPended; }
		static auto& subtreesDropped(Store& s) { return s.subtreesDropped; }
		static auto& categoryChildrenMissing(Store& s) { return s.categoryChildrenMissing; }
		static auto& sunTable(Store& s) { return s.sunTable; }
		static auto& lightTable(Store& s) { return s.lightTable; }
		static auto& rootMotion(Store& s) { return s.rootMotion; }
		using CandidateTable = Scene::CandidateTable;
		// The set's commit.
		using SlotDecision = Scene::Coordinator::SlotDecision;
		static SlotDecision DecideSlot(const Store& s, std::uint32_t a_slot, bool a_live, std::uint32_t a_drawn) { return s.DecideSlot(a_slot, a_live, a_drawn); }
		static const auto& setPhasesNext(const Store& s) { return s.setPhasesNext; }
		static const auto& setWaitingMark(const Store& s) { return s.setWaitingMark; }
		static bool EntryVetoed(const Store& s, std::uint32_t a_slot) { return s.EntryVetoed(a_slot); }
		// The mirror reads' checks (Coordinator's counting: NoteMirrorRead, LiveCheckLease, the deferred rechecks).
		using MirrorRead = Scene::Coordinator::MirrorRead;
		static EngineReadWindow::Lease LiveCheckLease(const Store& s, MirrorRead a_read) { return s.LiveCheckLease(a_read); }
		template <class Describe, class Recheck>
		static void NoteMirrorRead(const Store& s, MirrorRead a_read, bool a_differ, Describe&& a_describe, Recheck&& a_recheck)
		{
			s.NoteMirrorRead(a_read, a_differ, std::forward<Describe>(a_describe), std::forward<Recheck>(a_recheck));
		}
		static RE::NiNode* FindCategoryNodeLive(const Store& s, RE::NiAVObject* a_object, Ineligible* a_reason) { return s.FindCategoryNodeLive(a_object, a_reason); }
		/** @brief The mirror's chain from a_from up to a_stop (left out) or its top, at most 65 keys. */
		static std::vector<const void*> MirrorChain(const Store& s, const void* a_from, const void* a_stop)
		{
			std::vector<const void*> chain;
			for (const void* key = a_from; key && key != a_stop && chain.size() <= 64; key = s.mirror.Parent(key))
				chain.push_back(key);
			return chain;
		}
		static RE::NiNode* MirrorCategoryNode(const Store& s, const void* a_object, Ineligible& a_reason, bool& a_unmirrored) { return s.MirrorCategoryNode(a_object, a_reason, a_unmirrored); }
		static const void* MirrorSunEntry(const Store& s, const void* a_geometry) { return s.MirrorSunEntry(a_geometry); }
		static const RE::NiAVObject* LiveSunEntry(const RE::BSGeometry& a_geometry) { return Store::LiveSunEntry(a_geometry); }
		static const RE::NiAVObject* LiveLightRoot(const RE::BSGeometry& a_geometry, const RE::NiNode* a_category, bool& a_actor) { return Store::LiveLightRoot(a_geometry, a_category, a_actor); }
		static void MirrorSubtree(const Store& s, const void* a_root, Ineligible a_reason, std::vector<std::pair<RE::BSGeometry*, Ineligible>>& a_out, bool& a_unmirrored)
		{
			s.MirrorSubtree(a_root, a_reason, a_out, a_unmirrored);
		}
		static const auto& shadowIndexedInputs(const Store& s) { return s.shadowIndexedInputs; }
		static const auto& shadowIndexedLive(const Store& s) { return s.shadowIndexedLive; }
		static Scene::Coordinator::ShadowInputs ShadowInputsOf(const Store& s, std::uint32_t a_slot) { return s.ShadowInputsOf(a_slot); }
		static const auto& stats(const Store& s) { return s.stats; }
		static const auto& sunTable(const Store& s) { return s.sunTable; }
		static const auto& lightTable(const Store& s) { return s.lightTable; }
		static const auto& sunCandidateSet(const Store& s) { return s.sunCandidateSet; }
		static const auto& lightCandidateSet(const Store& s) { return s.lightCandidateSet; }
		static const auto& rootDependents(const Store& s) { return s.rootDependents; }
		static const std::vector<RE::BSGeometry*>* LightDependentsOf(const Store& s, const RE::NiAVObject* a_root) { return s.LightDependentsOf(a_root); }
		static std::uint8_t PrimaryVerdict(const Store& s, const Tracked& a_tracked, const RE::BSGeometry& a_geometry)
		{
			return Eligibility::PrimaryVerdict(Store::VerdictOf(a_tracked), [&] { return s.mirror.Leaf(&a_geometry); });
		}
		static auto& fanoutStats(Store& s) { return s.fanoutStats; }
		static std::string attachLine(Store& s) { return s.TakeAttachLine(); }
		static auto& placementStats(Store& s) { return s.placementStats; }
		// The entries held out (the Hold vetoes) and observed (Observe), as the coordinator counts them.
		static std::uint32_t vetoHeldCount(const Store& s) { return s.vetoHeld; }
		static std::uint32_t vetoObservedCount(const Store& s) { return s.vetoObserved; }
		static const auto& setWaitCause(const Store& s) { return s.setWaitCause; }
		static const auto& setWaitWhy(const Store& s) { return s.setWaitWhy; }
		static Api::TrackedState TrackedStateOf(const Store& s, const void* a_entry) { return s.TrackedStateOf(a_entry); }
		static bool IsResidentSlot(const Store& s, std::uint32_t a_slot) { return s.IsResidentSlot(a_slot); }
		static auto& shadingStats(Store& s) { return s.shadingStats; }
		static auto& mirrorReads(Store& s) { return s.mirrorReads; }
		static constexpr const auto& kMirrorReadNames = Scene::Coordinator::kMirrorReadNames;
		// A component's state (null before it is made, or past the components) and the deltas it published since the last publication
		// (PublishDelta), for its observers.
		static void* componentState(const Store& s, Api::CoordinatorComponentId a_component)
		{
			return a_component.index < s.components.size() ? s.components[a_component.index].get() : nullptr;
		}
		static const std::vector<std::shared_ptr<const void>>& componentDeltas(const Store& s, Api::CoordinatorComponentId a_component)
		{
			static const std::vector<std::shared_ptr<const void>> none;
			return a_component.index < s.componentDeltaRequests.size() ? s.componentDeltaRequests[a_component.index] : none;
		}
		static const SetSnapshot* publicationClaims(const Store& s) { return s.publicationClaims.get(); }
		// The components' observers' reads (the LOD gates' parity): the coordinator's root marks, the observed slots' readiness as the
		// commit recorded it, the slots requeued for the next commit, the next publication's claims, and a slot's participation (DecideSlot's).
		static const auto& rootMarks(const Store& s) { return s.rootMarks; }
		static Api::ObservedState ObservedSlot(const Store& s, std::uint32_t a_slot) { return s.ObservedSlotOf(a_slot); }
		static const auto& requeued(const Store& s) { return s.requeued; }
		static const SetSnapshot* setBuilding(const Store& s) { return s.setBuilding.get(); }
		static std::uint8_t SetParticipation(const Store& s, std::uint32_t a_slot, std::uint32_t a_drawn) { return s.SetParticipation(a_slot, a_drawn); }

		// The mirror's records and its report's counters.
		static const auto& mirrorNodes(const SceneMirror& m) { return m.nodes; }
		static const auto& mirrorGeometries(const SceneMirror& m) { return m.geometries; }
		static const auto& mirrorProperties(const SceneMirror& m) { return m.properties; }
		static const auto& mirrorAlphas(const SceneMirror& m) { return m.alphas; }
		/** @brief Whether the mirror holds a node, property or alpha record of a_key. */
		static bool mirrorHolds(const SceneMirror& m, const void* a_key) { return m.nodes.contains(a_key) || m.properties.contains(a_key) || m.alphas.contains(a_key); }
		static auto& mirrorApplied(SceneMirror& m) { return m.applied; }
		static auto& mirrorDetached(SceneMirror& m) { return m.detached; }
		static auto& mirrorUpdates(SceneMirror& m) { return m.updates; }
		static auto& mirrorUpdatesUnheld(SceneMirror& m) { return m.updatesUnheld; }
		static auto& mirrorLightEvictions(SceneMirror& m) { return m.lightEvictions; }
		static auto& mirrorEvictions(SceneMirror& m) { return m.evictions; }
		static auto& mirrorEvictedRecords(SceneMirror& m) { return m.evictedRecords; }
		static auto& mirrorFirstEviction(SceneMirror& m) { return m.firstEviction; }
		static auto& mirrorReplayed(SceneMirror& m) { return m.replayed; }
		static auto& mirrorStale(SceneMirror& m) { return m.stale; }
		static auto& mirrorSuperseded(SceneMirror& m) { return m.superseded; }
		static auto& mirrorUpdated(SceneMirror& m) { return m.updated; }

		// The private reads the observers make.
		static constexpr std::uint32_t kTraitActor = Scene::Coordinator::kTraitActor, kTraitMoves = Scene::Coordinator::kTraitMoves, kTraitRootMoves = Scene::Coordinator::kTraitRootMoves;
		static bool ResidentObject(const Store& s, std::int32_t a_object) { return s.ResidentObject(a_object); }
		static bool HiddenForWalk(const Store& s, const RE::NiAVObject* a_object) { return s.HiddenForWalk(a_object); }
		static std::uint32_t PerFrameTraits(const Store& s, const Tracked& a_tracked, const RE::BSGeometry& a_geometry) { return s.PerFrameTraits(a_tracked, a_geometry); }
		/** @brief PerFrameOf with the reference roots' motion taken from scratch (a_freshMotion): it writes nothing of the coordinator then. */
		static std::pair<bool, std::uint32_t> PerFrameOfFresh(const Store& s, const Tracked& a_tracked, const RE::BSGeometry& a_geometry, Ineligible a_reason,
			std::uint32_t& a_traits, ankerl::unordered_dense::map<const RE::NiAVObject*, bool>& a_freshMotion)
		{
			return const_cast<Store&>(s).PerFrameOf(a_tracked, a_geometry, a_reason, a_traits, &a_freshMotion);
		}

		/** @brief The tables, writable for the one observer that undoes a write it makes (the extras check). */
		static Tables& RestorableTables(Store& s) { return s.tables; }
		static bool WriteObjectExtras(Store& s, std::uint32_t a_object) { return s.WriteObjectExtras(a_object); }
		/** @brief The shading check's late events named for the next frame (what the walk's next drain would name). */
		static void NameShadingKeys(Store& s, const std::vector<const void*>& a_keys, std::vector<std::uint32_t>* a_slots) { s.NameShadingKeys(a_keys, a_slots); }

		static const auto& residents(const Store& s) { return s.residents; }
		static const auto& shadingNamed(const Store& s) { return s.shadingNamed; }
		static const auto& residentPatches(const Store& s) { return s.residentPatches; }

		// The walk's reference (Coordinator::DenseReference): the coordinator is left as it was.
		using ReferenceReasons = ankerl::unordered_dense::map<const RE::BSGeometry*, Ineligible>;
		static void DenseReference(Store& s, Tables& a_dense, ReferenceReasons& a_reasons) { s.DenseReference(a_dense, a_reasons); }
	};
}
