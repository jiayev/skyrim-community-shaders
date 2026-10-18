// The DCLF set (SceneSet.h; dclf-architecture.md, "The DCLF set"): who draws an object, decided by the commit of each scene pass.
// Also the pass itself (RunSceneWork), its publication and the lookups and table snapshots it carries. Scene lane.

#include "Internal.h"


#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <algorithm>
#include <atomic>
#include <span>

namespace DCLF::Scene
{
	std::uint32_t Coordinator::SetCapability()
	{
		// Main always; shadow/occlusion once set up (objects then wait on PhaseReady); each view's phase once it is drawable, in slot order
		// (the reflection's, the first person's). Under the commit's toggles (the pass's: ToggleSnapshotScope).
		const auto toggles = ActiveToggles();
		std::uint32_t phases = kSetMain | (toggles.shadows ? IndirectDraws::Get().ShadowCapability() : 0u);
		const auto& views = Extensions::Views();
		for (std::size_t v = 0; v < views.views.size(); ++v)
			if (views.views[v].phase && IndirectDraws::Get().ViewDrawable(static_cast<Api::EngineView>(v), toggles))
				phases |= views.views[v].phase;
		return phases;
	}

	std::uint8_t Coordinator::SetParticipation(std::uint32_t a_slot, std::uint32_t a_drawn) const
	{
		const auto flags = tables.objects[a_slot].flags;
		if (flags & kObjectFree)
			return 0;
		// A CategoryRoot view's slot (a first-person one) takes part in its view alone (the engine draws the skeleton in no world view;
		// the sun's casters run with both roots hidden): opaque, alpha-tested or forward (blended Lighting, Effect meshes, particles).
		// Decals, refraction and shadow-only objects stay the engine's (an effect decal is forward there, ViewDecalForward).
		if (const auto view = ViewRootOfSlot(a_slot); view != Api::EngineView::Count) {
			const auto p = tables.objects[a_slot].pipelineIndex;
			const bool forward = (flags & kObjectForward) && p < tables.pipelines.size() && RasterForward(tables.pipelines[p].rasterFlags);
			const bool drawable = !(flags & kObjectShadowOnly) && !ObjectDecalGroup(flags) && p < tables.pipelines.size() &&
			                      !RasterRefraction(tables.pipelines[p].rasterFlags) &&
			                      (forward || !((flags & kObjectForward) || Extensions::FamilyRules(tables.pipelines[p].rasterFlags).rootViewsForwardOnly));
			return static_cast<std::uint8_t>((drawable ? Extensions::ViewEntryOf(view).phase : 0u) & a_drawn);
		}
		std::uint32_t phases = 0;
		if (!(flags & kObjectShadowOnly))
			phases |= kSetMain;
		// The LodTechniques views draw the LOD techniques (the reflection's faces: dclf-architecture.md, "Water reflections").
		if (const auto p = tables.objects[a_slot].pipelineIndex; !(flags & kObjectShadowOnly) && p < tables.pipelines.size() &&
			LodLightingTechnique(tables.pipelines[p].passDescriptor))
			phases |= Extensions::Views().lodPhases;
		if (!(flags & kObjectNoShadow))
			phases |= kSetCaster | kSetCasterPoint;
		// Each occlusion map's phase (its entry's: 0 for a map none takes) for a slot with its technique.
		const auto& maps = Extensions::ShadowViewsSet().maps;
		if (a_slot < tables.occlusionTechnique[kOcclusionSky].size() && tables.occlusionTechnique[kOcclusionSky][a_slot])
			phases |= maps[kOcclusionSky].phase;
		if (a_slot < tables.occlusionTechnique[kOcclusionPrecipitation].size() && tables.occlusionTechnique[kOcclusionPrecipitation][a_slot])
			phases |= maps[kOcclusionPrecipitation].phase;
		return static_cast<std::uint8_t>(phases & a_drawn);
	}

	Api::EngineView Coordinator::ViewRootOfSlot(std::uint32_t a_slot) const
	{
		if (!FirstViewRoot() || a_slot >= tables.objectGeometry.size() || !tables.objectGeometry[a_slot])
			return Api::EngineView::Count;
		const auto entry = tracked.find(tables.objectGeometry[a_slot]);
		return entry != tracked.end() ? ViewRootOf(entry->second.categoryNode) : Api::EngineView::Count;
	}

	bool Coordinator::MainReady(std::uint32_t a_slot, std::uint32_t& a_why) const
	{
		// What a main build's pair resolution (MainBuild::ResolvePair) needs from this pass's lookups. Its other checks are the tables'
		// own structure: a member that fails them is a defect set parity reports.
		const auto& object = tables.objects[a_slot];
		const std::uint32_t p = object.pipelineIndex, m = object.materialIndex;
		if ((object.flags & kObjectNoBindings) || !PipelineDrawableIn(tables, lookups, p))
			return a_why = 0, false;
		// A refraction object draws with the refraction pipeline: the engine keeps it until the lookups' catalog has one.
		if (RasterRefraction(tables.pipelines[p].rasterFlags) && !(lookupsCatalog && lookupsCatalog->refraction.pipeline))
			return a_why = 11, false;
		if (m >= lookups.materials.size() || m >= tables.materialSlotKey.size() || !lookups.materials[m].resolved || lookups.materials[m].key != tables.materialSlotKey[m])
			return a_why = 1, false;
		if (!tables.PipelineConstantsCurrent(p) || !tables.TechniqueConstantsValid(p))
			return a_why = 9, false;
		if (tables.TechniqueShadowMask(p) && lookups.pipelines[p].shadowMaskIndex == Lookups::kNone)
			return a_why = 2, false;
		if (!lookups.samplersResolved || lookups.nullTexture == Lookups::kNone)
			return a_why = 3, false;
		// A ProjectedUV pipeline (not Hair, technique 6) binds the projected textures.
		if (const auto pass = tables.pipelines[p].passDescriptor; (pass & 0x8000u) && ((pass >> 24) & 0x3f) != 6)
			for (const auto index : lookups.projectedTextures)
				if (index == Lookups::kNone)
					return a_why = 3, false;
		if (object.geometryIndex >= tables.geometries.size() || !tables.geometries[object.geometryIndex].vertexAddress ||
			!tables.geometries[object.geometryIndex].indexAddress)
			return a_why = 4, false;
		// Past the current scene buffers it has no build input until they grow.
		if (!IndirectDraws::Get().FitsScene(&tables, a_slot, false))
			return a_why = 10, false;
		// A decal needs its ordered slots in its group's range (OrderDecals; one a partition for blood decals).
		if (const std::uint32_t group = ObjectDecalGroup(object.flags))
			if (a_slot >= tables.rangeOrdinal.size() || tables.rangeOrdinal[a_slot] >= tables.rangeCount[group - 1])
				return a_why = 5, false;
		return true;
	}

	void Coordinator::QueueSet(std::uint32_t a_slot)
	{
		if (a_slot >= setQueueMark.size() || setQueueMark[a_slot])
			return;
		setQueueMark[a_slot] = 1;
		setQueue.push_back(a_slot);
	}

	void Coordinator::LeaveSet(std::uint32_t a_slot, std::uint8_t a_lost, bool a_partner)
	{
		// A claim is the whole object: losing any phase it holds loses all of them.
		if (a_slot < setPhasesNext.size() && (setPhasesNext[a_slot] & a_lost))
			a_lost = setPhasesNext[a_slot];
		if (a_slot < setPhasesNext.size() && (setPhasesNext[a_slot] & a_lost)) {
			setPhasesNext[a_slot] &= static_cast<std::uint8_t>(~a_lost);
			// The engine's copy is keyed by base geometry, and is this slot's only while it owns that geometry.
			if (const auto* geometry = a_slot < setGeometry.size() ? setGeometry[a_slot] : nullptr; geometry && setBuilding) {
				const auto owner = setMemberSlot.find(geometry);
				const bool owns = owner != setMemberSlot.end() && owner->second == a_slot;
				if (setPhasesNext[a_slot]) {
					if (owns) {
						setBuilding->phases.insert_or_assign(geometry, setPhasesNext[a_slot]);
						NoteSetPhases(geometry);
					}
				} else {
					if (owns) {
						setMemberSlot.erase(owner);
						setBuilding->phases.erase(geometry);
						NoteSetPhases(geometry);
					}
					setGeometry[a_slot] = nullptr;
				}
				setSnapshotDirty = owns || setSnapshotDirty;
			}
			++setStats.leftAfterCommit;
			QueueSet(a_slot);
			// Marked so ApplySet notes the claim left even if the next commit sees no change.
			if (a_slot < tables.objects.size())
				MarkSetApply(a_slot);
		}
		if (!a_partner || !(a_lost & kSetMain) || a_slot >= tables.objects.size())
			return;
		const std::uint32_t partner = tables.IsLayer(a_slot) ? tables.layerBase[a_slot] : a_slot < tables.layerOf.size() ? tables.layerOf[a_slot] : kNoObjectSlot;
		if (partner == kNoObjectSlot || partner >= tables.objects.size())
			return;
		const auto mainPhases = static_cast<std::uint8_t>(kSetMain | Extensions::Views().lodPhases);
		if (PhasesIn(tables, partner) & mainPhases) {
			tables.setPhases[partner] = 0;
			tables.objects[partner].flags &= ~kObjectMember;
			tables.NoteChange(partner, kChangeBindings);
		}
		LeaveSet(partner, mainPhases, false);
	}

	void Coordinator::MarkSetApply(std::uint32_t a_slot)
	{
		const std::size_t objects = tables.objects.size();
		if (setApplyMark.size() < objects)
			setApplyMark.resize(objects, 0);
		if (setGeometryNext.size() < objects)
			setGeometryNext.resize(objects, nullptr);
		setGeometryNext[a_slot] = tables.objectGeometry[a_slot];
		if (const std::uint32_t at = setApplyMark[a_slot]) {
			setApply[at - 1].second = tables.objectGeometry[a_slot];
			return;
		}
		setApply.emplace_back(a_slot, tables.objectGeometry[a_slot]);
		setApplyMark[a_slot] = static_cast<std::uint32_t>(setApply.size());
	}

	std::size_t Coordinator::TakeClaimsEntry()
	{
		// An entry only the pool holds (publications, frames and the claims' readers hold theirs).
		for (std::size_t i = 0; i < claimsPool.size(); ++i)
			if (claimsPool[i].snapshot.use_count() == 1) {
				std::atomic_thread_fence(std::memory_order_acquire);  // after the last holder's release (use_count reads relaxed)
				return i;
			}
		claimsPool.push_back({ std::make_shared<SetSnapshot>(), {}, {} });
		return claimsPool.size() - 1;
	}

	void Coordinator::PublishClaims()
	{
		ZoneScopedN("CS.DCLF.Scene.PublishClaims");
		// After ApplySet, before the revocation (whose LeaveSet writes are the next publication's): setBuilding is the last commit's set
		// minus what left since.
		if (!setBuilding)
			setBuilding = std::make_shared<SetSnapshot>();
		auto& pace = paceStats;
		const std::uint8_t drawn = static_cast<std::uint8_t>(setPhaseMask);
		claimsFresh = false;
		// The paced parity's reference: the claims made whole, as an every-pass publication's were.
		if (pacedCheckDue) {
			// setBuilding's phases only: its frame and drawn are never set (the published claims take them here).
			claimsReference = std::make_shared<SetSnapshot>(*setBuilding);
			claimsReference->frame = sceneFrame;
			claimsReference->drawn = drawn;
		}
		// Nothing changed since the last claims (setBuilding unwritten, no edit of the last): shared again.
		if (claimsPublished < claimsPool.size() && publicationClaims == claimsPool[claimsPublished].snapshot && !setSnapshotDirty) {
			const auto& last = claimsPool[claimsPublished];
			if (last.edited.empty() && last.cursor.Continues(setPhaseLog, 0) && last.cursor.position == setPhaseLog.End() && last.snapshot->drawn == drawn) {
				++pace.claimsShared;
				return;
			}
		}
		const std::size_t index = TakeClaimsEntry();
		auto& entry = claimsPool[index];
		auto& phases = entry.snapshot->phases;
		// Caught up by the keys written since its last publication and the keys its revocations edited (each to setBuilding's value), or
		// copied whole when the log no longer reaches it.
		auto refresh = [&](const RE::BSGeometry* a_geometry) {
			if (const auto it = setBuilding->phases.find(a_geometry); it != setBuilding->phases.end())
				phases.insert_or_assign(a_geometry, it->second);
			else
				phases.erase(a_geometry);
		};
		if (entry.cursor.Continues(setPhaseLog, 0)) {
			const auto unread = entry.cursor.Unread(setPhaseLog);
			for (const auto* geometry : unread)
				refresh(geometry);
			for (const auto* geometry : entry.edited)
				refresh(geometry);
			++pace.claimsReplayed;
			pace.claimsKeys += unread.size() + entry.edited.size();
		} else {
			phases = setBuilding->phases;
			++pace.claimsWhole;
		}
		entry.edited.clear();
		entry.cursor.Restart(0);
		entry.cursor.Advance(setPhaseLog);
		entry.snapshot->frame = sceneFrame;
		entry.snapshot->drawn = drawn;
		publicationClaims = entry.snapshot;
		claimsPublished = index;
		claimsFresh = true;
		setSnapshotDirty = false;
		++setStats.publications;
		// The pool's readers are the entries alone; one behind the bound is copied whole.
		setPhaseLog.Trim(1u << 16);
	}

	void Coordinator::RevokeFromClaims()
	{
		if (claimsReference)
			for (const auto& [geometry, lost] : claimsRevoked)
				if (const auto it = claimsReference->phases.find(geometry); it != claimsReference->phases.end()) {
					it->second &= ~lost;
					if (!it->second)
						claimsReference->phases.erase(it);
				}
		if (claimsRevoked.empty() || claimsPublished >= claimsPool.size()) {
			claimsRevoked.clear();
			return;
		}
		// Claims shared from the last publication may be held: this one's are a copy of them (rare: a revocation with the set unchanged).
		if (!claimsFresh) {
			const std::size_t from = claimsPublished;
			const std::size_t index = TakeClaimsEntry();
			auto& entry = claimsPool[index];
			const auto& source = claimsPool[from];
			*entry.snapshot = *source.snapshot;
			entry.cursor = source.cursor;
			entry.edited = source.edited;
			publicationClaims = entry.snapshot;
			claimsPublished = index;
			claimsFresh = true;
			++paceStats.claimsWhole;
		}
		// Only the revoked claims: this publication's alone (setBuilding is the next commit's to settle; the notes requeue the slots).
		auto& entry = claimsPool[claimsPublished];
		auto& phases = entry.snapshot->phases;
		for (const auto& [geometry, lost] : claimsRevoked) {
			const auto it = phases.find(geometry);
			if (it == phases.end())
				continue;
			entry.edited.push_back(geometry);
			it->second &= ~lost;
			if (!it->second)
				phases.erase(it);
		}
		paceStats.claimsRevoked += claimsRevoked.size();
		claimsRevoked.clear();
	}

	Coordinator::SlotDecision Coordinator::DecideSlot(std::uint32_t a_slot, bool a_live, std::uint32_t a_drawn) const
	{
		// Read-only. All participating phases if ready for all, else none (whole-object claims); `wait` with reasons in order
		// (SetStats::waitingBy's index); `partial` when some were ready (counted for the report).
		SlotDecision d;
		if (!a_live || a_slot >= tables.objects.size())
			return d;
		const std::uint8_t participation = SetParticipation(a_slot, a_drawn);
		std::uint8_t phases = participation;
		const auto& views = Extensions::Views();
		if (!phases) {
			// For the observers: a CategoryRoot view's slot (a first-person one) SetParticipation left out.
			if ((a_drawn & views.rootPhases) && Diagnostics::Armed<Diagnostics::SetSlotDecided>())
				if (const auto view = ViewRootOfSlot(a_slot); view != Api::EngineView::Count)
					d.rootViewExcluded = (a_drawn & Extensions::ViewEntryOf(view).phase) != 0;
			return d;
		}
		auto waiting = [&](std::uint32_t a_why) {
			d.wait = true;
			if (d.whyCount < d.whys.size())
				d.whys[d.whyCount++] = static_cast<std::uint8_t>(a_why);
		};
		// The main phase needs its binding; shadow and occlusion phases draw from the record alone.
		if (phases & kSetMain) {
			bool main = IsResidentSlot(a_slot);
			if (std::uint32_t why = 0; main && !MainReady(a_slot, why)) {
				waiting(why);
				main = false;
			}
			if (!main)
				phases &= ~kSetMain;
		}
		// The views', in the decision's order (Extensions::ViewSet::decideOrder: the CategoryRoot views, then the rest). Reason 8: a
		// forward pipeline.
		for (std::uint32_t i = 0; i < views.decideCount; ++i) {
			const auto view = views.decideOrder[i];
			const auto& entry = Extensions::ViewEntryOf(view);
			if (!(phases & entry.phase))
				continue;
			if (entry.membership == Api::ViewMembership::CategoryRoot) {
				// A CategoryRoot view (the first person) draws with the main rows and bindings: main-ready, plus its pipelines (forward and
				// Z-prepass) in the lookups' catalog.
				bool member = IsResidentSlot(a_slot);
				if (std::uint32_t why = 0; member && !MainReady(a_slot, why)) {
					waiting(why);
					member = false;
				}
				std::uint32_t slotWhy = 0;
				if (member && !IndirectDraws::Get().ViewSlotReady(view, &tables, &lookups, lookupsCatalog.get(), a_slot, &slotWhy)) {
					waiting(8);
					member = false;
				}
				if (!member)
					phases &= static_cast<std::uint8_t>(~entry.phase);
				// For the observers: held out, and why.
				if (!member) {
					d.rootViewHeld = true;
					d.heldWhyCount = d.whyCount;
					d.rootViewSlotWhy = slotWhy;
				}
			} else if (!IndirectDraws::Get().ViewSlotReady(view, &tables, &lookups, lookupsCatalog.get(), a_slot)) {
				// A LodTechniques view (the reflection's faces) needs the slot's forward pipeline in the lookups' catalog (the one the
				// revision resolves from).
				waiting(8);
				phases &= static_cast<std::uint8_t>(~entry.phase);
			}
		}
		// The casters' phases, then the occlusion maps' (Extensions::ShadowViewSet::readinessOrder).
		const auto& shadowViews = Extensions::ShadowViewsSet();
		for (std::uint32_t r = 0; r < shadowViews.readinessCount; ++r) {
			const std::uint8_t phase = shadowViews.readinessOrder[r];
			if (std::uint32_t why = 7; (phases & phase) && !IndirectDraws::Get().PhaseReady(&tables, lookups, a_slot, phase, &why)) {
				waiting(why);
				phases &= ~phase;
			}
		}
		if (phases == participation) {
			d.phases = phases;
			return d;
		}
		d.partial = phases != 0;
		return d;
	}

	// One commit's inputs, shared by its stages (CommitSet).
	struct Coordinator::SetCommit
	{
		std::size_t objects = 0;
		bool live = false;          // the scene is built (else the set is empty)
		std::uint32_t drawn = 0;    // the phases DCLF can draw (SetCapability)
		bool gatesLive = false;     // a component asks for veto work (CommitLive)
		bool waitingMoved = false;  // a waiting mark moved (setWaiting gained or lost a slot): else the list stands as the last compaction left it
	};

	void Coordinator::CommitMembershipEvents(const SetCommit& a_commit)
	{
		// Membership events: the change log since the last commit and readiness moves for waiting slots. New phases, new tables or a
		// log the cursor cannot continue evaluate every slot.
		auto& setPhases = setPhasesNext;
		const std::size_t objects = a_commit.objects;
		const bool live = a_commit.live;
		const std::uint32_t drawn = a_commit.drawn;
		const bool everything = drawn != setPhaseMask || !setCursor.Continues(tables.changeLog, tablesGeneration) || setPhases.size() > objects;
		if (everything) {
			++setStats.resyncs;
			logger::info("[DCLF] set resync at frame {} ({} objects, scene {}): {}", sceneFrame, objects, live ? "built" : "not built",
				drawn != setPhaseMask                                 ? fmt::format("phases {:#x} -> {:#x}", setPhaseMask, drawn) :
				!setCursor.Continues(tables.changeLog, tablesGeneration) ? std::string("new tables or a change log the cursor cannot read on") :
				                                                          std::string("fewer objects"));
		}
		setPhaseMask = drawn;
		if (everything) {
			setCursor.Restart(tablesGeneration);
			setPhases.assign(objects, 0);
			setGeometry.assign(objects, nullptr);
			setLackingNext.assign(objects, 0);
			setMemberSlot.clear();
			for (std::uint32_t slot = 0; slot < objects; ++slot)
				MarkSetApply(slot);
			setBuilding->phases.clear();
			// Every pooled claims entry is copied whole at its next publication.
			setPhaseLog.Invalidate();
			setSnapshotDirty = true;
			for (const std::uint32_t slot : setWaiting)
				if (slot < setWaitingMark.size())
					setWaitingMark[slot] = 0;
			setWaiting.clear();
			for (std::uint32_t slot = 0; slot < objects; ++slot)
				QueueSet(slot);
		} else {
			setPhases.resize(objects, 0);
			setGeometry.resize(objects, nullptr);
			setLackingNext.resize(objects, 0);
			constexpr std::uint32_t kSetCauses = kChangeBindings | kChangeMembership | kChangeGeometry | kChangeShadow | kChangeSkin;
			for (const auto& change : setCursor.Unread(tables.changeLog))
				if (change.causes & kSetCauses)
					QueueSet(change.slot);
		}
		setCursor.Advance(tables.changeLog);
	}

	void Coordinator::CommitReadinessSources()
	{
		// Readiness sources: lookups version, shadow lookups generation, constants stamp, scene-buffer fit serial. A waiting slot is
		// requeued when a source it waits on moved, or any did for kWaitOther.
		const std::array<std::uint64_t, kWaitSources> readiness{ (std::uint64_t(lookups.versionCounter) << 32) ^ lookups.generation, lookups.shadowGeneration,
			tables.constantsStamp, IndirectDraws::Get().SceneFitSerial() };
		std::uint8_t moved = 0;
		for (std::uint32_t source = 0; source < kWaitSources; ++source)
			if (readiness[source] != setReadiness[source]) {
				setReadiness[source] = readiness[source];
				moved |= std::uint8_t(1u << source);
			}
		// Forward-view pipelines (reason 8) arrive in a new catalog without new lookups: a changed ready count is a kWaitOther event.
		// Counted once a catalog (immutable), not once a commit.
		{
			if (lookupsCatalog != setForwardCatalog || !setForwardCatalog) {
				std::uint64_t count = 0;
				if (lookupsCatalog)
					for (const auto& [key, entry] : lookupsCatalog->forwardEntries)
						count += entry.pipeline ? 1u : 0u;
				setForwardCatalog = lookupsCatalog;
				setForwardCount = count;
				++paceStats.forwardCounted;
			} else {
				++paceStats.forwardKept;
			}
			const std::uint64_t forwardReady = setForwardCount;
			if (forwardReady != setForwardReadiness) {
				setForwardReadiness = forwardReady;
				moved |= kWaitOther;
			}
		}
		if (moved) {
			++setStats.readinessEvents;
			for (const std::uint32_t slot : setWaiting)
				if (slot < setWaitCause.size() && (setWaitCause[slot] & (moved | kWaitOther))) {
					QueueSet(slot);
					++setStats.waitingRequeued;
				}
		}
	}

	void Coordinator::CommitShadowModeRequeues(std::size_t a_objects)
	{
		// New shadow view modes or rasterizer states (rare) may need pipelines a caster lacks: requeue every caster and waiting slot.
		// Read from the lane's lookups, never the render thread's.
		auto& setPhases = setPhasesNext;
		if (const std::uint64_t modes = lookups.shadowInputsSerial; modes != setShadowModes) {
			setShadowModes = modes;
			for (std::uint32_t slot = 0; slot < a_objects; ++slot)
				if (setPhases[slot] & ~kSetMain)
					QueueSet(slot);
			for (const std::uint32_t slot : setWaiting)
				QueueSet(slot);
		}
	}

	std::uint8_t Coordinator::CommitDecideSlot(std::uint32_t a_slot, const SetCommit& a_commit, bool& a_wait, bool* a_partial)
	{
		// DecideSlot plus the commit's side effects (wait causes and reasons, counts, observers).
		if (a_slot < setWaitCause.size())
			setWaitCause[a_slot] = 0;
		const SlotDecision decision = DecideSlot(a_slot, a_commit.live, a_commit.drawn);
		const std::uint8_t whyBefore = a_slot < setWaitWhy.size() ? setWaitWhy[a_slot] : std::uint8_t{ 0 };
		a_wait = decision.wait;
		if (a_partial && decision.partial)
			*a_partial = true;
		for (std::uint8_t w = 0; w < decision.whyCount; ++w) {
			const std::uint32_t why = decision.whys[w];
			if (!setWaitCause[a_slot])
				setWaitWhy[a_slot] = static_cast<std::uint8_t>(why);
			setWaitCause[a_slot] |= WaitCauseOf(why);
			++setStats.waitingBy[why];
			if (setStats.firstWaiting.empty())
				if (const auto* geometry = tables.objectGeometry[a_slot])
					setStats.firstWaiting = fmt::format("'{}' (slot {}, reason {})", geometry->name.c_str() ? geometry->name.c_str() : "?", a_slot, why);
		}
		if (decision.rootViewExcluded || decision.rootViewHeld)
			Diagnostics::Notify(Diagnostics::SetSlotDecided{ *this, a_slot, decision, decision.heldWhyCount ? decision.whys[0] : whyBefore });
		return decision.phases;
	}

	void Coordinator::CommitApplySlot(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, SetCommit& a_commit)
	{
		auto& setPhases = setPhasesNext;
		const bool live = a_commit.live;
		const std::uint32_t drawn = a_commit.drawn;
		// Already pending application: refresh for the geometry it holds now.
		if (setApplyMark[a_slot])
			MarkSetApply(a_slot);
		// Occluder and LodTechniques view phases (the reflection's) it participates in but misses.
		const std::uint8_t lacking =
			live ? static_cast<std::uint8_t>(SetParticipation(a_slot, drawn) & ~a_phases & Extensions::ResolvedViewBits().lackingMask) : 0;
		if (lacking != setLackingNext[a_slot]) {
			setLackingNext[a_slot] = lacking;
			MarkSetApply(a_slot);
		}
		if (a_wait != (setWaitingMark[a_slot] != 0)) {
			setWaitingMark[a_slot] = a_wait ? 1 : 0;
			a_commit.waitingMoved = true;
			if (a_wait)
				setWaiting.push_back(a_slot);  // compacted below
		}
		const std::uint8_t before = setPhases[a_slot];
		if (before == a_phases)
			return;
		setPhases[a_slot] = a_phases;
		MarkSetApply(a_slot);  // the record's kObjectMember and Tables::setPhases, at ApplySet
		if (!before || !a_phases)
			++(a_phases ? setStats.joined : setStats.left);
		if (before && !a_phases) {
			const std::uint8_t participation = live ? SetParticipation(a_slot, drawn) : std::uint8_t{ 0 };
			const std::size_t cause = !live                                                    ? 0 :
			                          !participation                                           ? 1 :
			                          ((participation & kSetMain) && !IsResidentSlot(a_slot))  ? 2 :
			                          (a_slot < setWaitCause.size() && setWaitCause[a_slot])   ? 3 :
			                                                                                     4;
			++setStats.leftBy[cause];
			if (cause == 3 && a_slot < setWaitWhy.size())
				++setStats.leftWaiting[(std::min<std::size_t>)(setWaitWhy[a_slot], setStats.leftWaiting.size() - 1)];
		}
		// The engine's copy is keyed by base geometry; a layer is a member with its base.
		if (tables.IsLayer(a_slot))
			return;
		if (const auto* old = setGeometry[a_slot]; old && !a_phases) {
			if (const auto owner = setMemberSlot.find(old); owner != setMemberSlot.end() && owner->second == a_slot) {
				setMemberSlot.erase(owner);
				setBuilding->phases.erase(old);
				NoteSetPhases(old);
			}
			setGeometry[a_slot] = nullptr;
		}
		if (a_phases)
			if (const auto* geometry = tables.objectGeometry[a_slot]) {
				setMemberSlot.insert_or_assign(geometry, a_slot);
				setBuilding->phases.insert_or_assign(geometry, a_phases);
				NoteSetPhases(geometry);
				setGeometry[a_slot] = geometry;
			}
		setSnapshotDirty = true;
	}

	void Coordinator::CommitQueuedSlots(SetCommit& a_commit)
	{
		const std::size_t objects = a_commit.objects;
		const std::uint32_t drawn = a_commit.drawn;
		const bool live = a_commit.live;
		for (std::size_t q = 0; q < setQueue.size(); ++q) {
			const std::uint32_t slot = setQueue[q];
			if (slot >= objects)
				continue;
			// A base and its layer are main-phase members together (the engine's main passes of both are the base geometry's).
			const std::uint32_t partner = tables.IsLayer(slot) ? tables.layerBase[slot] : slot < tables.layerOf.size() ? tables.layerOf[slot] : kNoObjectSlot;
			bool wait = false, partial = false;
			std::uint8_t phases = CommitDecideSlot(slot, a_commit, wait, &partial);
			if (setPartialMark.size() < objects)
				setPartialMark.resize(objects, 0);
			if ((setPartialMark[slot] != 0) != partial) {
				setPartialMark[slot] = partial ? 1 : 0;
				setPartialCount += partial ? 1u : std::uint32_t(-1);
			}
			if (partner != kNoObjectSlot && partner < objects) {
				bool partnerWait = false;
				std::uint8_t partnerPhases = CommitDecideSlot(partner, a_commit, partnerWait);
				if (!(phases & kSetMain) != !(partnerPhases & kSetMain)) {
					++setStats.waitingBy[6];
					// Either may become ready on any source.
					setWaitCause[slot] |= kWaitOther;
					setWaitCause[partner] |= kWaitOther;
					phases = 0;
					partnerPhases = 0;
				}
				// Vetoed entries' slots, read as data: gated incoming blocks hold their phases until the flip (Observe); outgoing ones drop
				// after it (Hold).
				if (a_commit.gatesLive)
					partnerPhases = VetoedPhases(partner, partnerPhases, partnerWait, drawn, live);
				CommitApplySlot(partner, partnerPhases, partnerWait, a_commit);
			}
			if (a_commit.gatesLive)
				phases = VetoedPhases(slot, phases, wait, drawn, live);
			CommitApplySlot(slot, phases, wait, a_commit);
		}
	}

	void Coordinator::CommitComponentsSettle(SetCommit& a_commit)
	{
		// The components settle: gates whose incoming geometries are all settled flip here and ride the publication (the gates' component
		// deltas: Features::Lod::Gates::Adopt). The vetoes they drop or set apply here, before the set's change is noted and the claims are snapshotted.
		restructuredTaken = false;
		settleApply.clear();
		settling = true;
		ComponentsSettle(a_commit.live);
		settling = false;
		for (const auto& [slot, phases] : settleApply)
			if (slot < a_commit.objects)
				CommitApplySlot(slot, phases, false, a_commit);
		settleApply.clear();
	}

	void Coordinator::CommitSet()
	{
		ZoneScopedN("CS.DCLF.Scene.CommitSet");
		++setStats.commits;
		SetCommit commit;
		const std::size_t objects = tables.objects.size();
		commit.objects = objects;
		// Not built (load screen): the set is empty.
		const bool live = sceneBuilt;
		commit.live = live;
		const std::uint32_t drawn = live ? SetCapability() : 0u;
		commit.drawn = drawn;
		// The commit's own copy (setPhasesNext); records and Tables::setPhases take it at the next ApplySet.
		setQueueMark.resize(objects, 0);
		setWaitingMark.resize(objects, 0);
		setLackingNext.resize(objects, 0);
		setApplyMark.resize(objects, 0);
		setWaitCause.resize(objects, 0);
		setWaitWhy.resize(objects, 0);
		setCommitFrame = sceneFrame;
		setCommitToggles = sceneInputs.togglesGeneration;
		setCommitToggleSet = sceneInputs.toggles;
		setGeometryNext.resize(objects, nullptr);
		if (!setBuilding)
			setBuilding = std::make_shared<SetSnapshot>();
		// Without a component asking for it (open gates) or a vetoed entry (held-out blocks) the commit does no veto work.
		const bool gatesLive = CommitLive();
		commit.gatesLive = gatesLive;
		for (const std::uint32_t slot : requeued)
			QueueSet(slot);
		requeued.clear();

		CommitMembershipEvents(commit);
		CommitReadinessSources();
		CommitShadowModeRequeues(objects);
		// The joins ran earlier in this pass: whatever they rebound or dropped is queued here through LeaveSet and the change log.

		CommitQueuedSlots(commit);
		if (gatesLive)
			CommitComponentsSettle(commit);
		Diagnostics::Notify(Diagnostics::SetCommitted{ *this, live, drawn, gatesLive });
		setStats.evaluated += setQueue.size();
		for (const std::uint32_t slot : setQueue)
			if (slot < objects)
				setQueueMark[slot] = 0;
		setQueue.clear();
		if (commit.waitingMoved) {
			std::erase_if(setWaiting, [&](std::uint32_t a_slot) { return a_slot >= setWaitingMark.size() || !setWaitingMark[a_slot]; });
			// A slot can be pushed twice within one commit.
			std::sort(setWaiting.begin(), setWaiting.end());
			setWaiting.erase(std::unique(setWaiting.begin(), setWaiting.end()), setWaiting.end());
			++paceStats.waitingCompacted;
		} else {
			++paceStats.waitingKept;
		}
		setStats.members += setMemberSlot.size();
		setStats.partial += setPartialCount;
		setStats.waiting += setWaiting.size();

		// The claims are made at the publication (PublishClaims), from setBuilding. ApplySet's revocation reads the log from here.
		setCommitCursor.Restart(tablesGeneration);
		setCommitCursor.Advance(tables.changeLog);
	}

	void Coordinator::ApplySet()
	{
		ZoneScopedN("CS.DCLF.Scene.ApplySet");
		const std::size_t objects = tables.objects.size();
		tables.setPhases.resize(objects, 0);
		auto& applied = tables.setPhases;
		setLacking.resize(objects, 0);
		setPhasesNext.resize(objects, 0);
		setLackingNext.resize(objects, 0);
		setPhasesApplied.resize(objects, 0);
		setGeometryApplied.resize(objects, nullptr);
		// Structural changes since the last revocation, read before this application's own notes: the claims applied now were
		// committed without them, so the revocation takes them back.
		NoteStructureChanges();
		// Geometries whose main claim changed since the last publication (PrimaryCull::NoteSetChanges at install).
		auto& joined = publicationJoined;
		auto& left = publicationLeft;
		for (const auto& [slot, geometry] : setApply) {
			if (slot < setApplyMark.size())
				setApplyMark[slot] = 0;
			if (slot >= objects)
				continue;
			// A slot freed or holding another geometry since the commit takes nothing.
			const bool same = !(tables.objects[slot].flags & kObjectFree) && tables.objectGeometry[slot] == geometry;
			const std::uint8_t phases = same ? setPhasesNext[slot] : std::uint8_t{ 0 };
			const std::uint8_t lacking = same ? setLackingNext[slot] : std::uint8_t{ 0 };
			// The claim as the engine hooks read it: by base geometry.
			const auto* wasClaimed = (setPhasesApplied[slot] & kSetMain) ? setGeometryApplied[slot] : nullptr;
			setPhasesApplied[slot] = phases;
			setGeometryApplied[slot] = phases && !tables.IsLayer(slot) ? geometry : nullptr;
			const auto* claimed = (phases & kSetMain) ? setGeometryApplied[slot] : nullptr;
			if (wasClaimed != claimed) {
				if (wasClaimed)
					left.push_back(wasClaimed);
				if (claimed)
					joined.push_back(claimed);
			}
			if (lacking != setLacking[slot]) {
				for (std::uint32_t v = 0; v < 3; ++v) {
					const std::uint8_t bit = Extensions::ResolvedViewBits().lackingPhases[v];
					setLackingCount[v] += ((lacking & bit) ? 1u : 0u) - ((setLacking[slot] & bit) ? 1u : 0u);
				}
				setLacking[slot] = lacking;
			}
			if (applied[slot] == phases)
				continue;
			// kObjectMember is the main phase (the main builds' GPU input); the shadow builds read the phases.
			applied[slot] = phases;
			if (!(tables.objects[slot].flags & kObjectFree)) {
				auto& object = tables.objects[slot];
				object.flags = (phases & kSetMain) ? (object.flags | kObjectMember) : (object.flags & ~kObjectMember);
				tables.NoteChange(slot, kChangeBindings);
			} else {
				tables.NotePublished(slot);
			}
		}
		setApply.clear();
		// The claims are the last commit's, minus what left since (LeaveSet) and what the revocation takes back: PublishClaims.
		// RevokeUndrawnClaims checks what changed since the commit (this application's own notes).
		if (setCommitCursor.Continues(tables.changeLog, tablesGeneration)) {
			revokeCursor = setCommitCursor;
		} else {
			revokeCursor.Restart(tablesGeneration);
			revokeCursor.Advance(tables.changeLog);
		}
		publicationCommitFrame = setCommitFrame;
		publicationCommitToggles = setCommitToggles;
		publicationCommitToggleSet = setCommitToggleSet;
	}

	std::uint8_t Coordinator::VetoedPhases(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, std::uint32_t a_drawn, bool a_live)
	{
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		const auto it = geometry ? tracked.find(const_cast<RE::BSGeometry*>(geometry)) : tracked.end();
		if (it == tracked.end())
			return a_phases;
		const std::uint8_t veto = it->second.veto;
		if (veto == Api::kVetoHold)
			return 0;
		if (veto != Api::kVetoObserve)
			return a_phases;
		// The slot's phases as the commit decided them, and whether whole (every phase it takes part in, not waiting): what dropping the
		// Observe applies and what its component's readiness counts (ObservedSlot).
		if (observedSlots.size() < tables.objects.size())
			observedSlots.resize(tables.objects.size());
		auto& held = observedSlots[a_slot];
		held.geometry = geometry;
		held.phases = a_phases;
		held.ready = !a_wait && a_phases == (a_live ? SetParticipation(a_slot, a_drawn) : std::uint8_t{ 0 });
		return 0;
	}

	bool Coordinator::EntryVetoed(std::uint32_t a_slot) const
	{
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		if (!geometry)
			return false;
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(geometry));
		return it != tracked.end() && it->second.veto != 0;
	}

	bool Coordinator::RestructuredSincePublication(std::uint32_t a_slot)
	{
		// The slots whose structure changed since the last publication's revocation (revokeCursor's unread kStructureCauses: a record
		// written or rewritten since): the publication this commit makes takes their claims back whole (RevokeUndrawnClaims) and claims them
		// only at the next one. Read once a commit, when first asked.
		if (!restructuredTaken) {
			restructuredTaken = true;
			restructured.clear();
			if (!IndirectDraws::Get().Failed() && revokeCursor.Continues(tables.changeLog, tablesGeneration))
				for (const auto& change : revokeCursor.Unread(tables.changeLog))
					if (change.causes & kStructureCauses)
						restructured.insert(change.slot);
		}
		return a_slot != kNoObjectSlot && restructured.contains(a_slot);
	}

	void Coordinator::NoteStructureChanges()
	{
		if (IndirectDraws::Get().Failed() || !revokeCursor.Continues(tables.changeLog, tablesGeneration))
			return;
		for (const auto& change : revokeCursor.Unread(tables.changeLog))
			if (change.causes & kStructureCauses)
				structureChanged.push_back(change.slot);
		revokeCursor.Advance(tables.changeLog);
	}

	void Coordinator::RevokeUndrawnClaims()
	{
		ZoneScopedN("CS.DCLF.Scene.RevokeUndrawnClaims");
		// Only the revoked claims are kept: RevokeFromClaims takes them out of this publication's claims (no copy of the set here).
		auto& revoked = claimsRevoked;
		revoked.clear();
		auto check = [&](std::uint32_t a_slot, bool a_structure) {
			if (a_slot >= setPhasesApplied.size() || a_slot >= setGeometryApplied.size())
				return;
			const std::uint8_t claimed = setPhasesApplied[a_slot];
			const auto* geometry = setGeometryApplied[a_slot];
			if (!claimed || !geometry)
				return;
			const bool same = a_slot < tables.objects.size() && !(tables.objects[a_slot].flags & kObjectFree) && tables.objectGeometry[a_slot] == geometry;
			std::uint8_t drawn = 0;
			if (same) {
				const auto flags = tables.objects[a_slot].flags;
				// Rows taken after the commit (extras, a fade root) may lie past the scene buffers until they grow (FitsScene).
				auto& draws = IndirectDraws::Get();
				const bool main = (flags & kObjectMember) && !(flags & kObjectNoBindings) && draws.FitsScene(&tables, a_slot, false);
				// A CategoryRoot view's member (a first-person one) draws with main bindings but has no kObjectMember; a LodTechniques
				// view's draws with the main phase.
				const auto& views = Extensions::Views();
				const auto root = static_cast<std::uint8_t>(tables.setPhases[a_slot] & views.rootPhases);
				const bool rootMember = root && !(flags & kObjectNoBindings) && draws.FitsScene(&tables, a_slot, false);
				const std::uint8_t others = draws.FitsScene(&tables, a_slot, true) ? static_cast<std::uint8_t>(tables.setPhases[a_slot] & ~(kSetMain | views.phases)) : 0;
				drawn = static_cast<std::uint8_t>((main ? (kSetMain | views.lodPhases) : 0u) | (rootMember ? root : 0u) | others);
			}
			// A structural change (shapes made without it) or any undrawn phase revokes the whole claim.
			const std::uint8_t lost = ((same && a_structure) || (claimed & ~drawn)) ? claimed : std::uint8_t{ 0 };
			if (!lost)
				return;
			if (same && a_structure && !(claimed & ~drawn))
				++revokedStructureGeometries;
			setPhasesApplied[a_slot] &= ~lost;
			if (same) {
				// Neither the accumulate phase nor the next publication's claims may re-admit it.
				const bool held = tables.setPhases[a_slot] != 0;
				tables.setPhases[a_slot] = 0;
				LeaveSet(a_slot, lost);
				if (held || (tables.objects[a_slot].flags & kObjectMember)) {
					tables.objects[a_slot].flags &= ~kObjectMember;
					tables.NoteChange(a_slot, kChangeBindings);
				}
			}
			revoked.emplace_back(geometry, lost);
		};
		if (revokeCursor.Continues(tables.changeLog, tablesGeneration)) {
			const bool stamps = !IndirectDraws::Get().Failed();
			for (const auto& change : revokeCursor.Unread(tables.changeLog)) {
				check(change.slot, false);
				if (stamps && (change.causes & kStructureCauses))
					structureChanged.push_back(change.slot);
			}
			// Plus every structural change read since the last revocation (ApplySet's).
			for (const std::uint32_t slot : structureChanged)
				check(slot, true);
		} else {
			// The log broke (new tables): every claim is checked.
			for (std::uint32_t slot = 0; slot < setPhasesApplied.size(); ++slot)
				check(slot, false);
		}
		structureChanged.clear();
		revokeCursor.Restart(tablesGeneration);
		revokeCursor.Advance(tables.changeLog);
		for (const auto& [geometry, lost] : revoked) {
			++revokedGeometries;
			if (lost & kSetMain) {
				++revokedMain;
				publicationLeft.push_back(geometry);
			}
		}
		// Out of the publication's claims (PublishClaims made them before the revocation's LeaveSet writes, which the next one replays).
		RevokeFromClaims();
	}

	void Coordinator::PublishScene()
	{
		ZoneScopedN("CS.DCLF.Scene.PublishScene");
		// A pass whose only change is deltas the frame takes and nothing a snapshot is built from reads (the placement plans, the shading
		// names, the retired imports, the light entries: no claim, table, lookup or toggle moved since the last publication, and
		// the claims shared again unrevoked) publishes the last tables again: the snapshot builder then posts the last snapshot again for
		// it (PassesThrough: the same objects), and the frame takes its deltas. Its tables would be a copy of the same content.
		const auto& pace = paceStats;
		bool deltasOnly = publishedKeyValid && !publishForced && publishedTables && setApply.empty() && !setSnapshotDirty && !nodeSetsDirty &&
		                  publicationJoined.empty() && publicationLeft.empty() &&
		                  componentSeedRequests.empty() && switchesApplied.empty() && !switchResync && !tables.ClassMemberChangesPending() &&
		                  componentDeltaRequests.empty() && CurrentPublishKey() == publishedKey;
		const auto sharedBefore = pace.claimsShared, wholeBefore = pace.claimsWhole, revokedBefore = pace.claimsRevoked;
		// Apply the committed set and revoke what cannot be drawn, so the publication draws exactly what it claims.
		ApplySet();
		PublishClaims();
		RevokeUndrawnClaims();
		deltasOnly = deltasOnly && pace.claimsShared != sharedBefore && pace.claimsWhole == wholeBefore && pace.claimsRevoked == revokedBefore &&
		             CurrentPublishKey() == publishedKey;
		// The lookups are the pass's (ResolveLookups before the commit judged readiness by them): a second resolve here could take a newer
		// catalog or shadow inputs than the commit judged.
		PublicationDeltas deltas;
		{
			ZoneScopedN("CS.DCLF.Scene.TakeDeltas");
			TakeDeltas(deltas);
		}
		if (!deltasOnly)
			PublishTables();
		PublishLookups();
		// The publication made and posted (the snapshot builder takes it).
		ZoneNamedN(postSnapshotZone, "CS.DCLF.Scene.PostSnapshotWork", true);
		auto publication = std::make_shared<ScenePublication>();
		publication->sequence = ++publicationSequence;
		publication->commitFrame = publicationCommitFrame;
		publication->togglesGeneration = publicationCommitToggles;
		publication->toggles = publicationCommitToggleSet;
		publication->tablesGeneration = tablesGeneration;
		publication->tables = publishedTables;
		publication->claims = publicationClaims ? publicationClaims : std::make_shared<const SetSnapshot>();
		publication->lackingCount = setLackingCount;
		publication->lookups = publishedLookups;
		publication->catalog = publishedCatalog;
		// The frame reads candidates only from the publication, never the coordinator.
		publication->sunCandidates = sunCandidates;
		publication->lightCandidates = lightCandidates;
		publication->lightEntriesAppeared = lightEntriesAppeared;
		// The point lights' category nodes, copied only when the set changed.
		if (std::exchange(nodeSetsDirty, false) || !publishedCategoryNodes) {
			ZoneScopedN("CS.DCLF.Scene.PublishCategoryNodes");
			publishedCategoryNodes = std::make_shared<const ankerl::unordered_dense::set<RE::NiNode*>>(categoryNodes);
		}
		publication->categoryNodes = publishedCategoryNodes;
		// Counts for MakeRevision on the snapshot builder.
		auto& revision = publication->revision;
		revision.pipelines = static_cast<std::uint32_t>(tables.pipelines.size());
		revision.shadowSlotKeys = lookups.shadowSlotKeys.size();
		revision.shadowKeys = tables.shadowKeysUsed.size();
		for (const auto& used : tables.occlusionKeysUsed)
			revision.shadowKeys += used.size();
		// The oldest unpublished event, for the latency reports.
		publication->eventNs = std::exchange(unpublishedNs, 0);
		publication->eventFrame = std::exchange(unpublishedFrame, 0u);
		if (publication->eventNs) {
			const auto nowNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			const std::uint32_t frameNow = SceneCapture::Frame();
			pumpStats.publishMs.push_back(nowNs > publication->eventNs ? static_cast<double>(nowNs - publication->eventNs) / 1.0e6 : 0.0);
			pumpStats.publishFrames.push_back(frameNow >= publication->eventFrame ? frameNow - publication->eventFrame : 0u);
		}
		++pumpStats.published;
		// Deltas first, so whoever adopts the publication finds its node on the log.
		deltas.publication = publication->sequence;
		deltas.commitFrame = publication->commitFrame;
		deltas.eventNs = publication->eventNs;
		deltas.eventFrame = publication->eventFrame;
		channels.publicationLog.Append(std::move(deltas));
		// The snapshot builder builds draws and the revision off the render thread; a skipped publication loses nothing of the log.
		IndirectDraws::Get().PostSnapshotWork(std::move(publication));
	}

	void Coordinator::TakeDeltas(PublicationDeltas& a_deltas)
	{
		a_deltas.joined = std::exchange(publicationJoined, {});
		a_deltas.left = std::exchange(publicationLeft, {});
		// Held passes' plans first, then the last walk's.
		for (auto& held : placementPlansHeld)
			a_deltas.plans.push_back(std::move(held));
		placementPlansHeld.clear();
		if (placementPlanReady)
			a_deltas.plans.push_back(std::exchange(placementPlanReady, nullptr));
		a_deltas.shading = std::exchange(shadingNamed, {});
		a_deltas.componentSeeds = std::exchange(componentSeedRequests, {});
		// No cap: PrimaryCull reads them all only on a full walk.
		a_deltas.switches = std::exchange(switchesApplied, {});
		a_deltas.switchResync = std::exchange(switchResync, false);
		a_deltas.retiredImports = std::exchange(retiredImports, {});
		for (std::size_t c = 0; c < a_deltas.classMembers.size(); ++c)
			a_deltas.classMembers[c] = std::exchange(tables.classMembers[c].changes, {});
		a_deltas.lightEntries = std::exchange(lightEntryChanges, {});
		// The components' published hooks before their deltas are taken (the LOD gates' observers read the flips pending).
		ComponentsPublished();
		a_deltas.componentDeltas = std::exchange(componentDeltaRequests, {});
	}

	void Coordinator::DropReturnedPublications()
	{
		channels.retiredPublications.Drop();
	}

	void Coordinator::TakeFrameInputs()
	{
		ZoneScopedN("CS.DCLF.Scene.TakeFrameInputs");
		// Latest wins; none keeps the last. Once a pass, at its start, so its globals and inputs are one frame's.
		const bool taken = channels.frameInputs.TakeInto(sceneInputs);
		// The toggles the whole pass evaluates, commits and publishes under (ScenePass binds them: ToggleSnapshotScope), the frame inputs'
		// (never the live ones, which the render thread may apply mid-pass); before the first frame inputs, the newest.
		if (!sceneInputs.frame)
			sceneInputs.toggles = Toggles::Get().Active();
		passSnapshot.toggles = sceneInputs.toggles;
		if (!taken)
			return;
		// The frame's own pass: it publishes whatever it changed (publication pacing, NoteScenePassDone's gate).
		passTookFrameInputs = true;
		sceneFrame = sceneInputs.frame;
		// A classification toggle or the feature coming back: drop every cached verdict before the pass reads one.
		if (sceneInputs.verdictsGeneration != verdictsApplied) {
			verdictsApplied = sceneInputs.verdictsGeneration;
			InvalidateVerdicts();
		}
	}

	void Coordinator::TakeCoordinatorInputs()
	{
		ZoneScopedN("CS.DCLF.Scene.TakeCoordinatorInputs");
		// In order: the content epochs the commit hooks sent of the journaled families (the depth commit's fade roots: a trim, which publishes
		// nothing by itself, since the hook posts every frame), then PrimaryCull's admitted roots, which force a publication.
		tables.families.TakeSent();
		if (const auto roots = channels.standInRootsPosted.Take()) {
			ComponentsAdmitted(*roots);
			publishForced = true;
		}
	}

	void Coordinator::PublishIfDue(bool a_task)
	{
		// Only when something changed, and paced by its consumer (PublicationDue): a publication the snapshot builder has not taken
		// would only be replaced in its slot, so the pass defers, and its commit's applications, deltas and log entries ride the next.
		DCLF_SCENE_PART(Publish, "CS.DCLF.Scene.Publish");
		auto& pace = paceStats;
		++pace.passes;
		if (sceneFrame != pace.lastFrame) {
			pace.lastFrame = sceneFrame;
			++pace.frames;
			pace.passesThisFrame = pace.publishedThisFrame = 0;
		}
		pace.maxPasses = (std::max)(pace.maxPasses, ++pace.passesThisFrame);
		bool needed = false;
		{
			ZoneScopedN("CS.DCLF.Scene.PublicationNeeded");
			needed = PublicationNeeded();
		}
		if (needed) {
			if (PublishReason why{}; PublicationDue(a_task, why)) {
				const std::uint32_t folded = std::exchange(passesDeferred, 0u);
				// The paced parity: a publication that folded deferred passes in, against the coordinator's whole (claims copied whole
				// and revoked as before the replay, the tables themselves).
				pacedCheckDue = folded && Policy::CheckPacedPublication(pacedPublications + 1);
				pacedPublications += folded ? 1u : 0u;
				PublishScene();
				NotePublished();
				++pace.published;
				++pace.byReason[static_cast<std::size_t>(why)];
				pace.folded += folded;
				pace.maxPublished = (std::max)(pace.maxPublished, ++pace.publishedThisFrame);
				if (std::exchange(pacedCheckDue, false) && publishedTables && publicationClaims) {
					ZoneScopedN("CS.DCLF.Scene.CheckPacedPublication");
					CheckPacedPublication(*publishedTables, *publicationClaims);
				}
				claimsReference.reset();
			} else {
				++passesDeferred;
				passDeferred = true;
				++pace.deferred;
			}
		}
	}

	void Coordinator::RunSceneWork(bool a_task)
	{
		// One pass (events, walk, joins, commit, publication), so a record bound in it is committed in it and published with it or the next
		// publishing pass (PublicationDue). The pump runs
		// it whenever a producer wakes it, any number of times a frame; PrimaryCull notes its events drop are held (primaryNotes), and
		// released references go back at Present.
		inSceneTask = a_task;
		holdPrimaryNotes = true;
		const auto workStart = std::chrono::steady_clock::now();
		// Every step between the parts has a zone of its own (CS.DCLF.Scene.<step>), so the pass's self time is what no step covers.
		{
			ZoneScopedN("CS.DCLF.Scene.TakeAheadContext");
			IndirectDraws::Get().TakeAheadContext();
		}
		++passSerial;
		{
			// The publications the frames returned: the last references of their lookups copies and deltas may go here.
			ZoneScopedN("CS.DCLF.Scene.DropReturnedPublications");
			DropReturnedPublications();
		}
		// Back on the free lists before anything allocates.
		RecycleRetired();
		// While DCLF is not running the pass applies events only, so the tracked set is current when it resumes.
		const bool full = passesFull.load(std::memory_order_acquire);
		if (full) {
			TakeCoordinatorInputs();
			// The joins' inputs posted at EarlyPrepass (ancestor map, pipeline frame, capture drain).
			ZoneScopedN("CS.DCLF.Scene.TakeAccumulateInputs");
			TakeAccumulateInputs();
		}
		{
			// Posts the drains must follow, then the engine queues drained to where each stands now.
			ZoneScopedN("CS.DCLF.Scene.TakeCategoryCapture");
			TakeCategoryCapture();
		}
		{
			// The components' own queues (the gate outcomes) are taken before the tracker's stack (an outcome is posted after its gate's
			// events, so the batch holds them) and applied after the batch (AfterBatch), unless it is a load screen's (carried, not applied):
			// those wait for the next.
			ZoneScopedN("CS.DCLF.Scene.TakeGateOutcomes");
			ComponentsTake();
		}
		{
			auto batch = CollectEvents();
			const bool applied = batch != nullptr;
			{
				ZoneScopedN("CS.DCLF.Scene.ApplyEvents");
				ApplyEvents(std::move(batch));
			}
			ZoneScopedN("CS.DCLF.Scene.ApplyGateOutcomes");
			if (applied)
				ComponentsChannelKeys(Api::ChannelApply::AfterBatch);
		}
		if (!full) {
			++pumpStats.eventsOnly;
			holdPrimaryNotes = false;
			inSceneTask = false;
			return;
		}
		{
			ZoneScopedN("CS.DCLF.Scene.ApplyConstantsPosts");
			ApplyConstantsPosts();
		}
		{
			// Technique rows to the pass's sample (a witness: a second pass of the frame finds nothing moved), before the joins use them.
			ZoneScopedN("CS.DCLF.Scene.RefreshTechniqueRows");
			RefreshTechniqueRows();
		}
		RefreshMaterialRecords();
		{
			// The bindings are made once, after the joins (nothing before the commit reads them). The walk's self time is what its parts
			// leave (the delta walk's own loop, the placement plan).
			ZoneScopedN("CS.DCLF.Scene.Walk");
			BuildFrame(Phase::Scene);
		}
		// The joins, in the same pass; members they drop from PrimaryCull are held until the publication.
		holdLostMembers = true;
		{
			DCLF_SCENE_PART(Joins, "CS.DCLF.Scene.Joins");
			{
				ZoneScopedN("CS.DCLF.Scene.Accumulate");
				BuildFrame(Phase::Accumulate);
			}
			{
				// New pipelines' PerGeometry blocks, before the commit judges them (MainReady).
				ZoneScopedN("CS.DCLF.Scene.MakeNewPipelineConstants");
				MakeNewPipelineConstants();
			}
			UpdateMaterialBindings();
			UpdateSharedBindings();
			// The lookups the commit judges readiness by, and the publication carries (one resolve a pass).
			ResolveLookups();
		}
		{
			// After the joins: their leaves and writes are on the change log it reads.
			DCLF_SCENE_PART(Commit, "CS.DCLF.Scene.Commit");
			CommitSet();
		}
		PublishIfDue(a_task);
		holdLostMembers = false;
		{
			ZoneScopedN("CS.DCLF.Scene.EndSceneFrame");
			Diagnostics::Notify<Diagnostics::ScenePoint::MirrorCheckPoint>();
			EndSceneFrame();
		}
		[[maybe_unused]] const double workMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - workStart).count();
		TracyPlot("CS.DCLF.SceneWorkMs", workMs);
		holdPrimaryNotes = false;
		inSceneTask = false;
	}

	void Coordinator::ResolveLookups()
	{
		ZoneScopedN("CS.DCLF.Scene.ResolveLookups");
		// Bindings are written in place by the scene work; changed material entries take their versions here.
		VersionMaterialBindings();
		// The newest catalog (none newer keeps the last); IndirectDraws::ResolveLookups is pure apart from requesting unseen keys.
		if (auto catalog = DrawPipelines::Get().TakeCatalog())
			lookupsCatalog = std::move(catalog);
		IndirectDraws::Get().ResolveLookups(&tables, lookupsCatalog.get(), lookups, lookupsResolve);
	}

	void Coordinator::PublishLookups()
	{
		ZoneScopedN("CS.DCLF.Scene.PublishLookups");
		auto& publication = tablesPublication;
		// A newer catalog of the same generations holds every index an older one did, so it serves unchanged lookups too.
		publishedCatalog = lookupsCatalog;
		// Unchanged: share the last copy. Else a new copy that shares unwritten chunks (SharedChunks).
		const auto key = lookups.ChangeKey();
		if (publishedLookups && key == publishedLookupsKey) {
			++publication.lookupsShared;
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		publishedLookups = lookups.Snapshot();
		publishedLookupsKey = key;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		++publication.lookupsCopied;
		publication.lookupsMs += ms;
		publication.lookupsMaxMs = std::max(publication.lookupsMaxMs, ms);
		// A new instance restarts its count.
		const std::uint64_t copies = lookups.pipelines.Copies() + lookups.materials.Copies();
		publication.lookupsChunks += copies >= lookupsChunkCopies ? copies - lookupsChunkCopies : copies;
		lookupsChunkCopies = copies;
	}

	void Coordinator::ResetLookups()
	{
		// Generations move past the old instance (stale for builds and kept bindings); versions continue, unique across instances.
		// Publications holding the old instance keep it until released.
		const auto lookupGeneration = lookups.generation;
		const auto shadowGeneration = lookups.shadowGeneration;
		const auto lookupVersion = lookups.versionCounter;
		lookups = Lookups{};
		lookups.generation = lookupGeneration + 1;
		lookups.shadowGeneration = shadowGeneration + 1;
		lookups.versionCounter = lookupVersion;
		lookupsResolve = {};
	}

	void Coordinator::RetireIntoChain()
	{
		auto& batch = retirement.Open();
		const auto before = batch.slots.size();
		batch.slots.insert(batch.slots.end(), tables.retiring.begin(), tables.retiring.end());
		tables.retiring.clear();
		const std::uint32_t epoch = tables.slotEpoch;
		tables.geometrySlots.TakeRetiring([&](std::uint32_t a_slot, std::uint32_t a_generation) { batch.slots.push_back({ Tables::kRetiredGeometrySlot, 0, a_slot, a_generation, epoch }); });
		tables.materialSlots.TakeRetiring([&](std::uint32_t a_slot, std::uint32_t a_generation) { batch.slots.push_back({ Tables::kRetiredMaterialSlot, 0, a_slot, a_generation, epoch }); });
		tables.pipelineSlots.TakeRetiring([&](std::uint32_t a_slot, std::uint32_t a_generation) { batch.slots.push_back({ Tables::kRetiredPipelineSlot, 0, a_slot, a_generation, epoch }); });
		retirementStats.retired += batch.slots.size() - before;
	}

	void Coordinator::RecycleRetired()
	{
		ZoneScopedN("CS.DCLF.Scene.RecycleRetired");
		retirementStats.batches += retirement.Drain([&](RetiredBatch&& a_batch) {
			for (const auto& retired : a_batch.slots) {
				const std::uint32_t slot = retired.slot;
				if (retired.epoch != tables.slotEpoch) {
					++retirementStats.dropped;
					continue;
				}
				++retirementStats.recycled;
				switch (retired.kind) {
				case Tables::kRetiredObject:
					if (tables.objectsRetiring)
						--tables.objectsRetiring;
					if (slot < tables.objects.size() && !tables.objectGeometry[slot])
						tables.objectFree.push_back(slot);
					break;
				case Tables::kRetiredFamilyRow:
					tables.families.Recycle(retired.extra, slot);
					break;
				case Tables::kRetiredExtras:
					tables.extraFree.push_back(slot);
					break;
				case Tables::kRetiredBones:
					tables.BoneFreeList(retired.extra).push_back(slot);
					break;
				case Tables::kRetiredStreamRow:
					tables.streamRowFree.push_back(slot);
					break;
				case Tables::kRetiredMaterialLookup:
					RetireMaterialBinding(slot);
					break;
				case Tables::kRetiredPipelineLookup:
					RetireMaskBinding(slot);
					break;
				case Tables::kRetiredGeometrySlot:
					tables.geometrySlots.Recycle(slot, retired.extra);
					break;
				case Tables::kRetiredMaterialSlot:
					tables.materialSlots.Recycle(slot, retired.extra);
					break;
				case Tables::kRetiredPipelineSlot:
					tables.pipelineSlots.Recycle(slot, retired.extra);
					break;
				case Tables::kRetiredComponentRange:
					ComponentRecycled(retired.owner, slot, retired.extra);
					break;
				}
			}
			// Imports go to the epochs (held until their executions retire); engine references are released at Present on the render
			// thread.
			for (auto& owner : a_batch.imports)
				retiredImports.push_back(std::move(owner));
			for (auto& material : a_batch.materials)
				if (material)
					EngineReleases::PushOwner(std::make_shared<MaterialReference>(std::move(material)));
			for (auto& reference : a_batch.references)
				EngineReleases::Push(std::move(reference));
			for (auto& events : a_batch.events)
				EngineReleases::PushOwner(std::move(events));
		});
	}

	namespace
	{
		// PublishTables' column sets. Each logged column with the causes that cover it (CausesBetween): a replayed slot copies it only when a
		// cause it was noted with covers it (kChangeAll: any). A slot the publication log names takes every column. a_right may be a
		// published snapshot (const: the spare's source).
		template <class Right, class Column>
		void ForLoggedColumns(SceneTables& a_left, Right& a_right, Column&& a_column)
		{
			a_column(a_left.objects, a_right.objects, kChangeAll);
			a_column(a_left.draws, a_right.draws, kChangeAll);
			a_column(a_left.ancestorValue, a_right.ancestorValue, kChangeAncestorValue);
			a_column(a_left.listedRoot, a_right.listedRoot, kChangeAll);
			a_column(a_left.hasFadeNode, a_right.hasFadeNode, kChangeAll);
			a_column(a_left.extraOffset, a_right.extraOffset, kChangeExtras);
			a_column(a_left.objectGeometry, a_right.objectGeometry, kChangeAll);
			a_column(a_left.objectIdentity, a_right.objectIdentity, kChangeAll);
			a_column(a_left.objectGroup, a_right.objectGroup, kChangeAll);
			a_column(a_left.shadowDiffuse, a_right.shadowDiffuse, kChangeShadow);
			a_column(a_left.shadowMaterial, a_right.shadowMaterial, kChangeShadow);
			a_column(a_left.fadeDistance, a_right.fadeDistance, kChangeAll);
			a_column(a_left.boneOffset, a_right.boneOffset, kChangeSkin);
			a_column(a_left.boneRows, a_right.boneRows, kChangeSkin);
			a_column(a_left.shadowTechnique, a_right.shadowTechnique, kChangeShadow);
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				a_column(a_left.occlusionTechnique[v], a_right.occlusionTechnique[v], kChangeShadow);
			a_column(a_left.streamRow, a_right.streamRow, kChangeAll);
			a_column(a_left.sceneFlags, a_right.sceneFlags, kChangeAll);
			a_column(a_left.skinPartitions, a_right.skinPartitions, kChangeSkin);
			a_column(a_left.skinLodPartitions, a_right.skinLodPartitions, kChangeSkin);
			a_column(a_left.shadowReject, a_right.shadowReject, kChangeShadow);
			// Residency is written ahead of the accumulate phase's snapshot (KeepResident): any cause.
			a_column(a_left.residentSlot, a_right.residentSlot, kChangeAll);
		}

		// Logged per-object columns sized apart from the objects (resized to the tables', then replayed by slot).
		template <class Right, class Column>
		void ForSidedColumns(SceneTables& a_left, Right& a_right, Column&& a_column)
		{
			a_column(a_left.setPhases, a_right.setPhases, std::uint8_t{ 0 });
			a_column(a_left.rangeOrdinal, a_right.rangeOrdinal, ~0u);
			a_column(a_left.layerBase, a_right.layerBase, kNoObjectSlot);
			a_column(a_left.layerOf, a_right.layerOf, kNoObjectSlot);
		}

		// Not published: the coordinator's alone - its walk stamps and templates, the slot tables' reference counts and free lists, the
		// writers' scratch (shadow texture transitions, freed stream rows, the class members' owners and changes the frame's fields keep
		// from the log), and
		// what is retiring. No reader of a publication reads them; a publication's stay as they were made (empty).
		template <class Column>
		void ForUnpublished(SceneTables& a_left, SceneTables& a_right, Column&& a_column)
		{
			a_column(a_left.objectSeen, a_right.objectSeen);
			a_column(a_left.geometryTemplate, a_right.geometryTemplate);
			a_column(a_left.geometrySlots, a_right.geometrySlots);
			a_column(a_left.pipelineSlots, a_right.pipelineSlots);
			a_column(a_left.materialSlots, a_right.materialSlots);
			a_column(a_left.geometryLastUsed, a_right.geometryLastUsed);
			a_column(a_left.objectFree, a_right.objectFree);
			a_column(a_left.boneFree, a_right.boneFree);
			a_column(a_left.boneFreeOther, a_right.boneFreeOther);
			a_column(a_left.extraFree, a_right.extraFree);
			a_column(a_left.streamRowFree, a_right.streamRowFree);
			a_column(a_left.streamRowsReleased, a_right.streamRowsReleased);
			for (std::size_t c = 0; c < a_left.classMembers.size(); ++c) {
				a_column(a_left.classMembers[c].owners, a_right.classMembers[c].owners);
				a_column(a_left.classMembers[c].changes, a_right.classMembers[c].changes);
			}
			a_column(a_left.shadowTextureSeen, a_right.shadowTextureSeen);
			a_column(a_left.shadowTextureChanges, a_right.shadowTextureChanges);
			a_column(a_left.retiring, a_right.retiring);
		}

		// Covered by the geometry log (geometryLastUsed is copied whole).
		template <class Column>
		void ForGeometryColumns(SceneTables& a_left, SceneTables& a_right, Column&& a_column)
		{
			a_column(a_left.geometries, a_right.geometries);
			a_column(a_left.geometryImports, a_right.geometryImports);
			a_column(a_left.geometrySlotKey, a_right.geometrySlotKey);
			a_column(a_left.geometryLayerKey, a_right.geometryLayerKey);
		}

		// The families copied only when their write stamp moved (the table families' own stamps: Extensions::TableFamilies::Publish).
		template <class Member>
		void ForPipelineFamily(SceneTables& a_left, SceneTables& a_right, Member&& a_member)
		{
			SceneTables::PipelineFamily(a_left, a_right, a_member);
		}

		template <class Member>
		void ForConstantsFamily(SceneTables& a_left, SceneTables& a_right, Member&& a_member)
		{
			a_member(a_left.pipelineConstants, a_right.pipelineConstants);
			a_member(a_left.techniqueConstants, a_right.techniqueConstants);
		}

		// Everything replayed or kept rather than copied whole, swapped between a_left and a_right.
		void SwapHeldOut(SceneTables& a_left, SceneTables& a_right)
		{
			ForLoggedColumns(a_left, a_right, [](auto& a_x, auto& a_y, std::uint32_t) { a_x.swap(a_y); });
			ForSidedColumns(a_left, a_right, [](auto& a_x, auto& a_y, auto) { a_x.swap(a_y); });
			ForUnpublished(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			ForPipelineFamily(a_left, a_right, [](auto& a_x, auto& a_y) { a_x.swap(a_y); });
			ForConstantsFamily(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			std::swap(a_left.families, a_right.families);
			ForGeometryColumns(a_left, a_right, [](auto& a_x, auto& a_y) { a_x.swap(a_y); });
			a_left.extraRows.swap(a_right.extraRows);
			a_left.materials.swap(a_right.materials);
			a_left.materialVersion.swap(a_right.materialVersion);
			a_left.materialFrameVersion.swap(a_right.materialFrameVersion);
			std::swap(a_left.changeLog, a_right.changeLog);
			std::swap(a_left.geometryLog, a_right.geometryLog);
			std::swap(a_left.materialLog, a_right.materialLog);
			std::swap(a_left.extrasBlockLog, a_right.extrasBlockLog);
			std::swap(a_left.publicationLog, a_right.publicationLog);
		}

		// Of the snapshots only the pool holds (each lets go of its node; the last published keeps it: a frame may still accept it), the
		// one nearest the tables: its logs' ends furthest on, the least to replay. One the logs no longer reach is copied whole.
		// a_pool.size(): none is free.
		std::size_t NearestPooledTables(std::vector<std::shared_ptr<SceneTables>>& a_pool, const std::vector<std::uint8_t>& a_replayable,
			const std::shared_ptr<SceneTables>& a_published, SceneTables& a_tables, CoordinatorPump::PaceStats& a_pace)
		{
			std::size_t index = a_pool.size();
			std::size_t first = a_pool.size();
			std::uint64_t nearest = 0;
			for (std::size_t i = 0; i < a_pool.size(); ++i) {
				auto& pooled = a_pool[i];
				if (pooled.use_count() != 1 || pooled == a_published)
					continue;
				pooled->retirementHold.reset();
				first = (std::min)(first, i);
				const bool reached = a_replayable[i] && a_tables.changeLog.Readable(pooled->changeLog.End()) && a_tables.materialLog.Readable(pooled->materialLog.End());
				const std::uint64_t reach = reached ? pooled->changeLog.End() + pooled->materialLog.End() + 1 : 0;
				if (index == a_pool.size() || reach > nearest) {
					index = i;
					nearest = reach;
				}
			}
			if (index != first)
				++a_pace.replayNearer;
			return index;
		}

		// What a pooled snapshot replays and keeps: the logs' ends it stands at, which logs still reach it, and the families whose stamps
		// stood.
		struct TablesReplayPlan
		{
			std::uint64_t position = 0, publicationPosition = 0, materialPosition = 0, geometryPosition = 0, extrasPosition = 0;
			bool replayExtras = false, replayGeometry = false, replay = false, replayMaterials = false;
			bool keepConstants = false, keepPipelines = false;
		};

		TablesReplayPlan PlanTablesReplay(const SceneTables& a_snapshot, const SceneTables& a_tables, std::uint8_t a_replayable)
		{
			TablesReplayPlan plan;
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			plan.position = snapshot.changeLog.End();
			plan.publicationPosition = snapshot.publicationLog.End();
			plan.materialPosition = snapshot.materialLog.End();
			plan.geometryPosition = snapshot.geometryLog.End();
			plan.extrasPosition = snapshot.extrasBlockLog.End();
			plan.replayExtras = a_replayable && tables.extrasBlockLog.Readable(plan.extrasPosition) &&
			                    snapshot.extrasBlockLog.base <= tables.extrasBlockLog.base && snapshot.extraRows.size() <= tables.extraRows.size();
			plan.replayGeometry = a_replayable && tables.geometryLog.Readable(plan.geometryPosition) &&
			                      snapshot.geometryLog.base <= tables.geometryLog.base && snapshot.geometries.size() <= tables.geometries.size();
			plan.replay = a_replayable && tables.changeLog.Readable(plan.position) && snapshot.changeLog.base <= tables.changeLog.base &&
			              tables.publicationLog.Readable(plan.publicationPosition) && snapshot.publicationLog.base <= tables.publicationLog.base &&
			              snapshot.objects.size() <= tables.objects.size();
			plan.replayMaterials = a_replayable && tables.materialLog.Readable(plan.materialPosition) && snapshot.materialLog.base <= tables.materialLog.base &&
			                       snapshot.materials.size() <= tables.materials.size();
			// Read before the copy writes the snapshot's stamps.
			plan.keepConstants = a_replayable && snapshot.constantsStamp == tables.constantsStamp;
			plan.keepPipelines = a_replayable && snapshot.pipelinesStamp == tables.pipelinesStamp;
			return plan;
		}

		// The rest, copied whole: swap the held-out members aside on both (restored however the copy ends), copy the rest whole.
		void CopyRestWhole(SceneTables& a_snapshot, SceneTables& a_tables)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			ZoneScopedN("CS.DCLF.Scene.PublishTables.Rest");
			SceneTables snapshotHeld, tablesHeld;
			SwapHeldOut(snapshot, snapshotHeld);
			SwapHeldOut(tables, tablesHeld);
			struct Restore
			{
				std::function<void()> undo;
				~Restore() { undo(); }
			} restore{ [&] { SwapHeldOut(tables, tablesHeld); } };
			snapshot = tables;
			SwapHeldOut(snapshot, snapshotHeld);
		}

		// The replay's object half, collected once on the lane: the object slots the logs name, sorted, with the causes each gathered
		// (a_causes; kChangeAll for a publication-log slot). A snapshot the logs no longer reach takes the logged columns and logs whole.
		void CollectReplaySlots(SceneTables& a_snapshot, SceneTables& a_tables, const TablesReplayPlan& a_plan, std::size_t a_objectSlots,
			std::vector<std::uint32_t>& a_slots, std::vector<std::uint32_t>& a_causes, Coordinator::TablesPublication& a_publication,
			CoordinatorPump::PaceStats& a_pace, bool& a_versionsKnown)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			auto& slots = a_slots;
			auto& causes = a_causes;
			const std::size_t objectSlots = a_objectSlots;
			slots.clear();
			if (a_plan.replay) {
				ZoneScopedN("CS.DCLF.Scene.PublishTables.Collect");
				snapshot.GrowObjects(objectSlots);
				ForSidedColumns(snapshot, tables, [](auto& a_to, auto& a_from, auto a_initial) { a_to.resize(a_from.size(), a_initial); });
				if (causes.size() < objectSlots)
					causes.resize(objectSlots, 0);
				auto name = [&](std::uint32_t a_slot, std::uint32_t a_causes) {
					if (a_slot >= objectSlots)
						return;
					if (!causes[a_slot])
						slots.push_back(a_slot);
					causes[a_slot] |= a_causes;
				};
				const auto changes = tables.changeLog.From(a_plan.position);
				for (const auto& change : changes)
					name(change.slot, change.causes ? change.causes : kChangeAll);
				const auto published = tables.publicationLog.From(a_plan.publicationPosition);
				for (const std::uint32_t slot : published)
					name(slot, kChangeAll);
				std::sort(slots.begin(), slots.end());
				a_pace.replayLogEntries += changes.size() + published.size();
			} else {
				++a_publication.wholeCopies;
				ForLoggedColumns(snapshot, tables, [](auto& a_to, auto& a_from, std::uint32_t) { a_to = a_from; });
				ForSidedColumns(snapshot, tables, [](auto& a_to, auto& a_from, auto) { a_to = a_from; });
				snapshot.changeLog = tables.changeLog;
				snapshot.publicationLog = tables.publicationLog;
				snapshot.geometryLog = tables.geometryLog;
				snapshot.materialLog = tables.materialLog;
				a_versionsKnown = false;
			}
		}

		// Material records by the slots the material log names (every write of a record notes its slot), collected into a_materialSlots
		// (sorted) for the replay's jobs; a snapshot the log no longer reaches by version (session-unique per write; none kept after a whole
		// copy), copied here.
		void ReplayMaterialRecords(SceneTables& a_snapshot, SceneTables& a_tables, const TablesReplayPlan& a_plan, bool a_versionsKnown,
			std::vector<std::uint32_t>& a_materialSlots, std::vector<std::uint8_t>& a_marks, Coordinator::TablesPublication& a_publication,
			CoordinatorPump::PaceStats& a_pace)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			auto& materialSlots = a_materialSlots;
			const std::size_t materialCount = tables.materials.size();
			snapshot.materials.resize(materialCount);
			materialSlots.clear();
			if (a_plan.replayMaterials) {
				// The versions with their records (a grown slot's are 0 until its first write, which the log names).
				snapshot.materialVersion.resize(materialCount, 0);
				snapshot.materialFrameVersion.resize(materialCount, 0);
				a_marks.assign(materialCount, 0);
				const auto noted = tables.materialLog.From(a_plan.materialPosition);
				for (const std::uint32_t slot : noted)
					if (slot < materialCount && !std::exchange(a_marks[slot], std::uint8_t{ 1 }))
						materialSlots.push_back(slot);
				std::sort(materialSlots.begin(), materialSlots.end());
				a_pace.replayMaterialEntries += noted.size();
				a_publication.materialsReplayed += materialSlots.size();
				a_pace.replayMaterials += materialSlots.size();
			} else {
				const auto& recordVersions = snapshot.materialVersion;
				const auto& recordFrameVersions = snapshot.materialFrameVersion;
				for (std::size_t slot = 0; slot < materialCount; ++slot) {
					if (a_versionsKnown && slot < recordVersions.size() && recordVersions[slot] == tables.materialVersion[slot] && slot < recordFrameVersions.size() &&
						recordFrameVersions[slot] == tables.materialFrameVersion[slot])
						continue;
					snapshot.materials[slot] = tables.materials[slot];
					++a_publication.materialsReplayed;
				}
				snapshot.materialVersion = tables.materialVersion;
				snapshot.materialFrameVersion = tables.materialFrameVersion;
			}
			a_publication.materialSlots += materialCount;
		}

		// After a replay's jobs: a layer link's other end, the named slots' causes cleared, and the logs brought up to the tables'.
		void FinishObjectReplay(SceneTables& a_snapshot, SceneTables& a_tables, const std::vector<std::uint32_t>& a_slots, std::vector<std::uint32_t>& a_causes,
			std::size_t a_objectSlots, Coordinator::TablesPublication& a_publication, CoordinatorPump::PaceStats& a_pace)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			auto& slots = a_slots;
			auto& causes = a_causes;
			// A layer link's other end: the layer's write notes only the layer.
			for (const std::uint32_t slot : slots)
				for (const std::uint32_t end : { tables.layerBase[slot], tables.layerOf[slot] })
					if (end < a_objectSlots && !causes[end]) {
						snapshot.layerBase[end] = tables.layerBase[end];
						snapshot.layerOf[end] = tables.layerOf[end];
					}
			for (const std::uint32_t slot : slots)
				causes[slot] = 0;
			a_publication.objectsReplayed += slots.size();
			a_pace.replaySlots += slots.size();
			// The logs: the tables' chunks shared (SharedEventLog), whatever the snapshot held let go.
			snapshot.changeLog = tables.changeLog;
			snapshot.publicationLog = tables.publicationLog;
			snapshot.geometryLog = tables.geometryLog;
			snapshot.materialLog = tables.materialLog;
		}

		// Extras rows by the blocks the block log names; else the rows and the log whole.
		void ReplayExtras(SceneTables& a_snapshot, SceneTables& a_tables, const TablesReplayPlan& a_plan)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			const std::uint64_t extrasPosition = a_plan.extrasPosition;
			if (a_plan.replayExtras) {
				snapshot.extraRows.resize(tables.extraRows.size(), 0.0f);
				for (const std::uint32_t offset : tables.extrasBlockLog.From(extrasPosition))
					if ((std::size_t(offset) + kExtraRows) * 4 <= tables.extraRows.size())
						std::copy_n(tables.extraRows.begin() + std::ptrdiff_t(offset) * 4, std::size_t(kExtraRows) * 4, snapshot.extraRows.begin() + std::ptrdiff_t(offset) * 4);
				snapshot.extrasBlockLog = tables.extrasBlockLog;  // the chunks shared
			} else {
				snapshot.extraRows = tables.extraRows;
				snapshot.extrasBlockLog = tables.extrasBlockLog;
			}
		}

		// The geometry columns on the lane (an import lease the snapshot drops is let go here, never on a pool worker): by the slots the
		// geometry log names, else whole.
		void ReplayGeometryColumns(SceneTables& a_snapshot, SceneTables& a_tables, const TablesReplayPlan& a_plan, std::vector<std::uint8_t>& a_marks,
			Coordinator::TablesPublication& a_publication)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			const std::size_t geometrySlots = tables.geometries.size();
			a_publication.geometrySlots += geometrySlots;
			if (a_plan.replayGeometry) {
				snapshot.GeometryColumns([geometrySlots](auto& a_column, auto&&... a_initial) { a_column.resize(geometrySlots, a_initial...); });
				auto& marks = a_marks;
				marks.assign(geometrySlots, 0);
				for (const std::uint32_t slot : tables.geometryLog.From(a_plan.geometryPosition)) {
					if (slot >= geometrySlots || std::exchange(marks[slot], std::uint8_t{ 1 }))
						continue;
					ForGeometryColumns(snapshot, tables, [slot](auto& a_to, auto& a_from) { a_to[slot] = a_from[slot]; });
					++a_publication.geometriesReplayed;
				}
			} else {
				++a_publication.geometryWholeCopies;
				ForGeometryColumns(snapshot, tables, [](auto& a_to, auto& a_from) { a_to = a_from; });
			}
		}
	}

	void Coordinator::ReplayTableColumns(Tables& a_snapshot, bool a_replay)
	{
		// The jobs: one per object column (its named slots, by contiguous runs, skipping slots no cause of theirs covers it for), then one
		// per chunk of material slots (records and versions, by runs). Each writes only its own column or record range of the snapshot,
		// which only the pool holds, and reads the tables, which nothing writes meanwhile: pure, so the pool may run them in any order.
		auto& snapshot = a_snapshot;
		auto& pace = paceStats;
		auto& slots = replaySlots;
		auto& causes = replaySlotCauses;
		auto& materialSlots = replayMaterialSlots;
		std::size_t columnJobs = 0;
		if (a_replay) {
			ForLoggedColumns(snapshot, tables, [&](auto&, auto&, std::uint32_t) { ++columnJobs; });
			ForSidedColumns(snapshot, tables, [&](auto&, auto&, auto) { ++columnJobs; });
		}
		const std::size_t materialJobs = (materialSlots.size() + kReplayMaterialGrain - 1) / kReplayMaterialGrain;
		const std::size_t jobs = columnJobs + materialJobs;
		struct JobCounts
		{
			std::uint64_t columns = 0, skipped = 0, runs = 0;
		};
		std::vector<JobCounts> counts(jobs);
		// The slots of a_list (sorted) a_keep takes, copied from a_from to a_to by contiguous runs.
		auto copyRuns = [](auto& a_to, const auto& a_from, std::span<const std::uint32_t> a_list, auto&& a_keep, JobCounts& a_counts) {
			const std::size_t limit = (std::min)(a_to.size(), a_from.size());
			std::size_t runBegin = 0, runEnd = 0;
			auto flush = [&] {
				if (runEnd > runBegin) {
					std::copy(a_from.begin() + std::ptrdiff_t(runBegin), a_from.begin() + std::ptrdiff_t(runEnd), a_to.begin() + std::ptrdiff_t(runBegin));
					++a_counts.runs;
				}
			};
			for (const std::uint32_t slot : a_list) {
				if (slot >= limit)
					break;
				if (!a_keep(slot)) {
					++a_counts.skipped;
					continue;
				}
				++a_counts.columns;
				if (slot != runEnd) {
					flush();
					runBegin = slot;
				}
				runEnd = std::size_t(slot) + 1;
			}
			flush();
		};
		auto job = [&](std::size_t a_job) {
			auto& jobCounts = counts[a_job];
			if (a_job < columnJobs) {
				std::size_t at = 0;
				auto column = [&](auto& a_to, auto& a_from, std::uint32_t a_gate) {
					if (at++ == a_job)
						copyRuns(a_to, a_from, slots, [&](std::uint32_t a_slot) { return (causes[a_slot] & a_gate) != 0; }, jobCounts);
				};
				ForLoggedColumns(snapshot, tables, column);
				ForSidedColumns(snapshot, tables, [&](auto& a_to, auto& a_from, auto) { column(a_to, a_from, kChangeAll); });
				return;
			}
			const std::size_t begin = (a_job - columnJobs) * kReplayMaterialGrain;
			const std::span<const std::uint32_t> range(materialSlots.data() + begin, (std::min)(kReplayMaterialGrain, materialSlots.size() - begin));
			auto all = [](std::uint32_t) { return true; };
			copyRuns(snapshot.materials, tables.materials, range, all, jobCounts);
			JobCounts versions;
			copyRuns(snapshot.materialVersion, tables.materialVersion, range, all, versions);
			copyRuns(snapshot.materialFrameVersion, tables.materialFrameVersion, range, all, versions);
		};
		auto run = [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.Scene.PublishTables.Shards");
			for (std::size_t j = a_begin; j < a_end; ++j)
				job(j);
		};
		const auto replayStart = std::chrono::steady_clock::now();
		// The pool for a replay worth the chunks (FanoutMode: the lane alone, CS_DCLF_FANOUT, the parities' serial runs).
		const bool worth = slots.size() >= kReplayMinSlots || materialSlots.size() >= kReplayMinMaterials;
		if (jobs && worth && FanoutMode(jobs, 2) == 2) {
			SceneScheduler::Executor().ParallelFor("CS.DCLF.Scene.PublishTables.Replay", jobs, 1, run);
			++pace.replayPooled;
		} else if (jobs) {
			run(0, jobs);
		}
		for (const auto& c : counts) {
			pace.replayColumns += c.columns;
			pace.replaySkipped += c.skipped;
			pace.replayRuns += c.runs;
		}
		pace.replayMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - replayStart).count();
	}

	namespace
	{
		/**
		 * @brief PublishTables' spare (EntrySpaceTable's pattern): after each publication the free pooled snapshot nearest the logs is
		 * brought up to the published one on the preparation pool, from it alone (immutable: the spare holds it until the lane takes the
		 * spare), so the next publication replays one publication's changes on the lane, not every change since the snapshot's own
		 * publication. What it brings up is what the logs name: the logged and sided object columns (with the layer links' other ends), the
		 * material records, the extras rows, and the change, publication, material and extras logs (their chunks shared). The geometry
		 * columns stay the publication's (a lease a snapshot drops is let go on the lane), as does everything copied whole or kept by
		 * stamp. One spare at a time; the coordinator's thread but for the catch-up, which its job or the lane runs.
		 */
		struct TablesSpare
		{
			/** @brief One catch-up, shared with its job (a new one per dispatch): whichever of the job and the lane claims it runs it. */
			struct State
			{
				std::atomic<bool> claimed{ false };
				std::atomic<bool> busy{ false };  // released once done: the snapshot's writes with it
			};
			SceneTables* target = nullptr;              // the pool's (which never lets go of its snapshots)
			std::shared_ptr<const SceneTables> source;  // the publication it is brought up to
			std::shared_ptr<State> state;
			// The catch-up's scratch (the job's while busy).
			std::vector<std::uint32_t> slots, causes;
			std::vector<std::uint8_t> marks;
			// Since startup: spares dispatched, run by the lane (not started), waited for (running).
			std::uint64_t dispatched = 0, runByLane = 0, waited = 0;

			TablesSpare() = default;
			TablesSpare(const TablesSpare&) = delete;
			TablesSpare& operator=(const TablesSpare&) = delete;
			/** @brief A running catch-up waited for (one not started is left: nothing reads it again). */
			~TablesSpare()
			{
				if (state && state->busy.load(std::memory_order_acquire) && state->claimed.exchange(true, std::memory_order_acq_rel))
					state->busy.wait(true, std::memory_order_acquire);
			}
		};

		/** @brief The spare, made at the first publication (after the coordinator: gone before it). */
		TablesSpare& Spare()
		{
			static TablesSpare spare;
			return spare;
		}

		/** @brief a_spare brought up to a_source (a published snapshot) by what its logs name since a_spare's ends: pure on a_spare and a_scratch. */
		void CatchUpTables(SceneTables& a_spare, const SceneTables& a_source, TablesSpare& a_scratch)
		{
			ZoneScopedN("CS.DCLF.Scene.PublishTables.Spare");
			auto& s = a_spare;
			const auto& p = a_source;
			const TablesReplayPlan plan = PlanTablesReplay(s, p, 1);
			if (plan.replay) {
				const std::size_t objectSlots = p.objects.size();
				s.GrowObjects(objectSlots);
				ForSidedColumns(s, p, [](auto& a_to, const auto& a_from, auto a_initial) { a_to.resize(a_from.size(), a_initial); });
				auto& slots = a_scratch.slots;
				auto& causes = a_scratch.causes;
				slots.clear();
				if (causes.size() < objectSlots)
					causes.resize(objectSlots, 0);
				auto name = [&](std::uint32_t a_slot, std::uint32_t a_causes) {
					if (a_slot >= objectSlots)
						return;
					if (!causes[a_slot])
						slots.push_back(a_slot);
					causes[a_slot] |= a_causes;
				};
				for (const auto& change : p.changeLog.From(plan.position))
					name(change.slot, change.causes ? change.causes : kChangeAll);
				for (const std::uint32_t slot : p.publicationLog.From(plan.publicationPosition))
					name(slot, kChangeAll);
				// As ReplayTableColumns' jobs, slot by slot: a column where a cause the slot gathered covers it.
				ForLoggedColumns(s, p, [&](auto& a_to, const auto& a_from, std::uint32_t a_gate) {
					const std::size_t limit = (std::min)(a_to.size(), a_from.size());
					for (const std::uint32_t slot : slots)
						if (slot < limit && (causes[slot] & a_gate))
							a_to[slot] = a_from[slot];
				});
				ForSidedColumns(s, p, [&](auto& a_to, const auto& a_from, auto) {
					const std::size_t limit = (std::min)(a_to.size(), a_from.size());
					for (const std::uint32_t slot : slots)
						if (slot < limit)
							a_to[slot] = a_from[slot];
				});
				// A layer link's other end: the layer's write notes only the layer.
				for (const std::uint32_t slot : slots)
					for (const std::uint32_t end : { p.layerBase[slot], p.layerOf[slot] })
						if (end < objectSlots && !causes[end]) {
							s.layerBase[end] = p.layerBase[end];
							s.layerOf[end] = p.layerOf[end];
						}
				for (const std::uint32_t slot : slots)
					causes[slot] = 0;
				s.changeLog = p.changeLog;
				s.publicationLog = p.publicationLog;
			}
			if (plan.replayMaterials) {
				const std::size_t materialCount = p.materials.size();
				s.materials.resize(materialCount);
				s.materialVersion.resize(materialCount, 0);
				s.materialFrameVersion.resize(materialCount, 0);
				auto& marks = a_scratch.marks;
				marks.assign(materialCount, 0);
				for (const std::uint32_t slot : p.materialLog.From(plan.materialPosition)) {
					if (slot >= materialCount || std::exchange(marks[slot], std::uint8_t{ 1 }))
						continue;
					s.materials[slot] = p.materials[slot];
					if (slot < p.materialVersion.size())
						s.materialVersion[slot] = p.materialVersion[slot];
					if (slot < p.materialFrameVersion.size())
						s.materialFrameVersion[slot] = p.materialFrameVersion[slot];
				}
				s.materialLog = p.materialLog;
			}
			if (plan.replayExtras) {
				s.extraRows.resize(p.extraRows.size(), 0.0f);
				for (const std::uint32_t offset : p.extrasBlockLog.From(plan.extrasPosition))
					if ((std::size_t(offset) + kExtraRows) * 4 <= p.extraRows.size())
						std::copy_n(p.extraRows.begin() + std::ptrdiff_t(offset) * 4, std::size_t(kExtraRows) * 4, s.extraRows.begin() + std::ptrdiff_t(offset) * 4);
				s.extrasBlockLog = p.extrasBlockLog;
			}
		}

		/** @brief The publication's start: the spare's catch-up done (run here if its job has not started, else waited for), its source let go. */
		void FinishTablesSpare()
		{
			auto& spare = Spare();
			if (spare.state && spare.state->busy.load(std::memory_order_acquire)) {
				if (!spare.state->claimed.exchange(true, std::memory_order_acq_rel)) {
					// Not started (the pool is busy): the lane runs it, the job finding it claimed.
					CatchUpTables(*spare.target, *spare.source, spare);
					spare.state->busy.store(false, std::memory_order_relaxed);
					++spare.runByLane;
				} else {
					// Running on a worker: waited for, as ParallelFor waits for a claimed chunk.
					spare.state->busy.wait(true, std::memory_order_acquire);
					++spare.waited;
				}
			}
			spare.source.reset();
			spare.target = nullptr;
		}

		/**
		 * @brief After a publication (a_published): the free pooled snapshot nearest the logs that they still reach (none free: no spare),
		 * brought up to it on the preparation pool (here under Policy::SerialFanout, or when the pool refuses the job).
		 */
		void DispatchTablesSpare(const std::vector<std::shared_ptr<SceneTables>>& a_pool, const std::vector<std::uint8_t>& a_replayable,
			const std::shared_ptr<SceneTables>& a_published)
		{
			std::size_t index = a_pool.size();
			std::uint64_t nearest = 0;
			for (std::size_t i = 0; i < a_pool.size(); ++i) {
				const auto& pooled = a_pool[i];
				if (pooled.use_count() != 1 || pooled == a_published || !a_replayable[i])
					continue;
				const TablesReplayPlan plan = PlanTablesReplay(*pooled, *a_published, 1);
				if (!plan.replay && !plan.replayMaterials && !plan.replayExtras)
					continue;
				const std::uint64_t reach = pooled->changeLog.End() + pooled->materialLog.End() + 1;
				if (index == a_pool.size() || reach > nearest) {
					index = i;
					nearest = reach;
				}
			}
			if (index == a_pool.size())
				return;
			auto& spare = Spare();
			spare.target = a_pool[index].get();
			spare.source = a_published;
			auto state = std::make_shared<TablesSpare::State>();
			state->busy.store(true, std::memory_order_relaxed);
			spare.state = state;
			++spare.dispatched;
			// The job reads the snapshots by address: the pool holds the spare, and the spare's source the publication, until the lane
			// finishes it (FinishTablesSpare, at the next publication).
			auto job = [target = spare.target, source = spare.source.get(), state = std::move(state)]() {
				if (state->claimed.exchange(true, std::memory_order_acq_rel))
					return;
				CatchUpTables(*target, *source, Spare());
				state->busy.store(false, std::memory_order_release);
				state->busy.notify_all();
			};
			if (Policy::SerialFanout() || !SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu,
											  "DCLF tables spare", [job](const auto&) { job(); }))
				job();
		}
	}

	void Coordinator::PublishTables()
	{
		ZoneScopedN("CS.DCLF.Scene.PublishTables");
		const auto start = std::chrono::steady_clock::now();
		auto& publication = tablesPublication;
		auto& pace = paceStats;
		std::size_t index = tablesPool.size();
		{
			ZoneScopedN("CS.DCLF.Scene.PublishTables.Pool");
			// Frees since the last publication go into the chain's open node, which this publication closes.
			RetireIntoChain();
			// The spare brought up to the last publication first (its source let go): the nearest snapshot, one publication behind.
			FinishTablesSpare();
			index = NearestPooledTables(tablesPool, tablesPoolReplayable, publishedTables, tables, pace);
		}
		if (index < tablesPool.size()) {
			++publication.reused;
		} else {
			tablesPool.emplace_back(std::make_shared<Tables>());
			tablesPoolReplayable.push_back(0);
			++publication.made;
		}
		const std::shared_ptr<Tables> target = tablesPool[index];
		Tables& snapshot = *target;

		// The snapshot equals the tables at its change log's end (WriteBoth keeps frame-start writes alike). Since then, replay: the
		// logged per-object columns by the slots the change and publication logs name (a layer link's other end with them); the logs
		// by appending what they gained; materials by the slots the material log names. The families whose stamps stood are kept.
		// Everything else is copied whole. A new snapshot, or one a log no longer reaches, is copied whole. The coordinator's own
		// columns (objectSeen, geometryTemplate) are not published.
		const TablesReplayPlan plan = PlanTablesReplay(snapshot, tables, tablesPoolReplayable[index]);
		// The material versions are held out with the records (replayed by the material log, or compared below); none are trusted after
		// a whole copy of the objects.
		bool versionsKnown = tablesPoolReplayable[index] != 0;
		CopyRestWhole(snapshot, tables);
		const auto restEnd = std::chrono::steady_clock::now();
		const double restMs = std::chrono::duration<double, std::milli>(restEnd - start).count();
		publication.restMs += restMs;
		pace.restMs += restMs;

		// The replay's two halves, collected once on the lane: the object slots the logs name, sorted, with the causes each gathered
		// (replaySlotCauses; kChangeAll for a publication-log slot), and the material slots the material log names, sorted. Their copies are
		// jobs over disjoint columns and disjoint record ranges (ReplayTableColumns).
		const std::size_t objectSlots = tables.objects.size();
		publication.objectSlots += objectSlots;
		CollectReplaySlots(snapshot, tables, plan, objectSlots, replaySlots, replaySlotCauses, publication, pace, versionsKnown);
		ReplayMaterialRecords(snapshot, tables, plan, versionsKnown, replayMaterialSlots, replayMaterialMarks, publication, pace);
		ReplayTableColumns(snapshot, plan.replay);
		if (plan.replay)
			FinishObjectReplay(snapshot, tables, replaySlots, replaySlotCauses, objectSlots, publication, pace);
		// The extras, the families and the geometry columns (each its own zone: what the families' zone spends, by part).
		ZoneNamedN(familiesZone, "CS.DCLF.Scene.PublishTables.Families", true);
		{
			ZoneScopedN("CS.DCLF.Scene.PublishTables.Extras");
			ReplayExtras(snapshot, tables, plan);
		}
		auto assign = [](auto& a_to, auto& a_from) { a_to = a_from; };
		std::uint32_t keptFamilies = 0;
		{
			// The table families: kept by stamp, their marked rows replayed, or copied whole (TableFamilies::Publish).
			ZoneScopedN("CS.DCLF.Scene.PublishTables.TableFamilies");
			keptFamilies = snapshot.families.Publish(tables.families, tablesPoolReplayable[index] != 0, publication.familiesKept);
		}
		if (!plan.keepConstants) {
			ZoneScopedN("CS.DCLF.Scene.PublishTables.Constants");
			ForConstantsFamily(snapshot, tables, assign);
		}
		if (!plan.keepPipelines) {
			ZoneScopedN("CS.DCLF.Scene.PublishTables.PipelineFamily");
			ForPipelineFamily(snapshot, tables, assign);
		}
		{
			ZoneScopedN("CS.DCLF.Scene.PublishTables.Geometry");
			ReplayGeometryColumns(snapshot, tables, plan, replayGeometryMarks, publication);
		}
		tablesPoolReplayable[index] = 1;

		Diagnostics::Notify(Diagnostics::TablesReplayed{ *this, snapshot, publication.published, plan.position, plan.geometryPosition, keptFamilies, plan.keepConstants });
		// The publication log's readers are the snapshots alone (one behind the bound is copied whole).
		tables.publicationLog.Trim(1u << 16);
		// Its node collects what is freed after it.
		{
			ZoneScopedN("CS.DCLF.Scene.PublishTables.Retirement");
			snapshot.retirementHold = retirement.Publish();
		}
		publishedTables = target;
		++publication.published;
		{
			ZoneScopedN("CS.DCLF.Scene.PublishTables.SpareDispatch");
			DispatchTablesSpare(tablesPool, tablesPoolReplayable, publishedTables);
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		publication.ms += ms;
		publication.maxMs = std::max(publication.maxMs, ms);
	}

	namespace
	{
		// CheckPacedPublication's tables half: the first difference of a publication's tables from the coordinator's a_tables (by the
		// columns every reader builds from, the log ends and the families' stamps); empty: none.
		std::string FirstTablesDifference(const SceneTables& a_published, const SceneTables& a_tables)
		{
			std::string first;
			const auto& t = a_tables;
			auto differ = [&](std::string a_what) {
				if (first.empty())
					first = std::move(a_what);
			};
			if (a_published.objects.size() != t.objects.size())
				differ(fmt::format("{} object slots against {}", a_published.objects.size(), t.objects.size()));
			else
				for (std::uint32_t slot = 0; slot < t.objects.size() && first.empty(); ++slot)
					if (const auto causes = SceneTables::CausesBetween(a_published.ColumnsOf(slot), t.ColumnsOf(slot)))
						differ(fmt::format("object slot {} (causes {:#x})", slot, causes));
			if (a_published.setPhases != t.setPhases || a_published.rangeOrdinal != t.rangeOrdinal || a_published.layerBase != t.layerBase || a_published.layerOf != t.layerOf)
				differ("a column replayed by slot (set phases, decal ordinals, layer links)");
			if (a_published.materials.size() != t.materials.size() || a_published.materialVersion != t.materialVersion || a_published.materialFrameVersion != t.materialFrameVersion)
				differ("the material versions");
			else
				for (std::size_t slot = 0; slot < t.materials.size() && first.empty(); ++slot)
					if (!(a_published.materials[slot] == t.materials[slot]))
						differ(fmt::format("material slot {}", slot));
			if (a_published.geometries.size() != t.geometries.size())
				differ("the geometry slots' count");
			else
				for (std::size_t slot = 0; slot < t.geometries.size() && first.empty(); ++slot)
					if (std::memcmp(&a_published.geometries[slot], &t.geometries[slot], sizeof(GeometryBuffers)) != 0 || a_published.geometrySlotKey[slot] != t.geometrySlotKey[slot])
						differ(fmt::format("geometry slot {}", slot));
			if (a_published.pipelines != t.pipelines || a_published.pipelineBindingVersion != t.pipelineBindingVersion)
				differ("the pipeline family");
			if (a_published.changeLog.End() != t.changeLog.End() || a_published.geometryLog.End() != t.geometryLog.End() || a_published.materialLog.End() != t.materialLog.End() ||
				a_published.extrasBlockLog.End() != t.extrasBlockLog.End() || !a_published.families.SameStamps(t.families) ||
				a_published.constantsStamp != t.constantsStamp || a_published.pipelinesStamp != t.pipelinesStamp || a_published.extraRows != t.extraRows)
				differ("a log's end, a family's stamp or the extras rows");
			return first;
		}

		// CheckPacedPublication's claims half: the first difference by key, both ways (a member only one side holds shows 0 on the other),
		// then the drawn phases, of the published a_claims from a_reference (made whole); empty: none.
		std::string FirstClaimsDifference(const SetSnapshot& a_reference, const SetSnapshot& a_claims)
		{
			std::string claimsFirst;
			for (const auto& [geometry, phases] : a_reference.phases)
				if (const std::uint8_t published = a_claims.PhasesOf(geometry); published != phases) {
					claimsFirst = fmt::format("geometry {} published {:#x}, made whole {:#x}", static_cast<const void*>(geometry), published, phases);
					break;
				}
			if (claimsFirst.empty())
				for (const auto& [geometry, phases] : a_claims.phases)
					if (a_reference.phases.find(geometry) == a_reference.phases.end()) {
						claimsFirst = fmt::format("geometry {} published {:#x}, made whole 0x0", static_cast<const void*>(geometry), phases);
						break;
					}
			if (claimsFirst.empty() && a_reference.drawn != a_claims.drawn)
				claimsFirst = fmt::format("drawn phases published {:#x}, made whole {:#x}", a_claims.drawn, a_reference.drawn);
			return claimsFirst;
		}
	}

	void Coordinator::CheckPacedPublication(const Tables& a_published, const SetSnapshot& a_claims)
	{
		// A publication that folded deferred passes in must equal the one an every-pass publication would have made at this pass: its
		// tables the coordinator's (by the columns every reader builds from, the log ends and the families' stamps), its claims the set
		// copied whole and revoked (claimsReference).
		auto& p = paceStats;
		++p.parityChecks;
		const std::string first = FirstTablesDifference(a_published, tables);
		if (!first.empty()) {
			++p.parityTables;
			if (p.parityFirst.empty())
				p.parityFirst = fmt::format("tables: {} (publication {})", first, publicationSequence);
		}
		if (!claimsReference)
			return;
		const std::string claimsFirst = FirstClaimsDifference(*claimsReference, a_claims);
		if (!claimsFirst.empty()) {
			++p.parityClaims;
			if (p.parityFirst.empty())
				p.parityFirst = fmt::format("claims: {} ({} members against {} made whole; publication {})", claimsFirst, a_claims.phases.size(),
					claimsReference->phases.size(), publicationSequence);
		}
	}
}
