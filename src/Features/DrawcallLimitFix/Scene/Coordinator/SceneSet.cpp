// The DCLF set (SceneSet.h; drawcall-limit-fix.md, "The DCLF set"): who draws an object, decided by the commit of each scene pass.
// Also the pass itself (RunSceneWork), its publication and the lookups and table snapshots it carries. Scene lane.

#include "Internal.h"

#include "Features/DrawcallLimitFix/Engine/Cull/LodGates.h"

#include "Features/DrawcallLimitFix/Draws/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Runtime/EngineReleases.h"
#include "Features/DrawcallLimitFix/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Runtime/Toggles.h"

#include <algorithm>
#include <span>

namespace DCLF::Scene
{
	std::uint32_t Coordinator::SetCapability()
	{
		// Main always; shadow/occlusion once set up (objects then wait on PhaseReady); reflection and first person once drawable.
		return kSetMain | (ActiveToggles().shadows ? IndirectDraws::Get().ShadowCapability() : 0u) |
		       (IndirectDraws::Get().ReflectionDrawable() ? kSetReflection : 0u) | (IndirectDraws::Get().FirstPersonDrawable() ? kSetFirstPerson : 0u);
	}

	std::uint8_t Coordinator::SetParticipation(std::uint32_t a_slot, std::uint32_t a_drawn) const
	{
		const auto flags = tables.objects[a_slot].flags;
		if (flags & kObjectFree)
			return 0;
		// A first-person slot takes part in the first-person view alone (the engine draws the skeleton in no world view; the sun's
		// casters run with both roots hidden): opaque, alpha-tested or forward (blended Lighting, Effect meshes, particles). Decals,
		// refraction and shadow-only objects stay the engine's (an effect decal is forward there, FirstPersonDecalForward).
		if (FirstPersonSlot(a_slot)) {
			const auto p = tables.objects[a_slot].pipelineIndex;
			const bool forward = (flags & kObjectForward) && p < tables.pipelines.size() && RasterForward(tables.pipelines[p].rasterFlags);
			const bool drawable = !(flags & kObjectShadowOnly) && !ObjectDecalGroup(flags) && p < tables.pipelines.size() &&
			                      !RasterRefraction(tables.pipelines[p].rasterFlags) && (forward || !((flags & kObjectForward) || RasterEffect(tables.pipelines[p].rasterFlags)));
			return static_cast<std::uint8_t>((drawable ? kSetFirstPerson : 0u) & a_drawn);
		}
		std::uint32_t phases = 0;
		if (!(flags & kObjectShadowOnly))
			phases |= kSetMain;
		// The reflection's faces draw the LOD techniques (dclf-lod.md).
		if (const auto p = tables.objects[a_slot].pipelineIndex; !(flags & kObjectShadowOnly) && p < tables.pipelines.size() &&
			LodLightingTechnique(tables.pipelines[p].passDescriptor))
			phases |= kSetReflection;
		if (!(flags & kObjectNoShadow))
			phases |= kSetCaster | kSetCasterPoint;
		if (a_slot < tables.occlusionTechnique[kOcclusionSky].size() && tables.occlusionTechnique[kOcclusionSky][a_slot])
			phases |= kSetOccluderSky;
		if (a_slot < tables.occlusionTechnique[kOcclusionPrecipitation].size() && tables.occlusionTechnique[kOcclusionPrecipitation][a_slot])
			phases |= kSetOccluderPrecipitation;
		return static_cast<std::uint8_t>(phases & a_drawn);
	}

	bool Coordinator::FirstPersonSlot(std::uint32_t a_slot) const
	{
		if (!firstPersonCategory || a_slot >= tables.objectGeometry.size() || !tables.objectGeometry[a_slot])
			return false;
		const auto entry = tracked.find(tables.objectGeometry[a_slot]);
		return entry != tracked.end() && FirstPersonEntry(entry->second.categoryNode);
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
			if (a_slot >= tables.decalOrdinal.size() || tables.decalOrdinal[a_slot] >= tables.decalCount[group - 1])
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
		constexpr std::uint8_t kMainPhases = kSetMain | kSetReflection;
		if (PhasesIn(tables, partner) & kMainPhases) {
			tables.setPhases[partner] = 0;
			tables.objects[partner].flags &= ~kObjectMember;
			tables.NoteChange(partner, kChangeBindings);
		}
		LeaveSet(partner, kMainPhases, false);
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
		if (!phases) {
			// For the observers: a first-person slot SetParticipation left out.
			d.firstPersonExcluded = (a_drawn & kSetFirstPerson) && Diagnostics::Armed<Diagnostics::SetSlotDecided>() && FirstPersonSlot(a_slot);
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
		// First person draws with the main rows and bindings (FirstPerson.cpp): main-ready, plus its first-person pipelines (forward and
		// Z-prepass) in the lookups' catalog. Reason 8: a forward pipeline.
		if (phases & kSetFirstPerson) {
			bool firstPerson = IsResidentSlot(a_slot);
			if (std::uint32_t why = 0; firstPerson && !MainReady(a_slot, why)) {
				waiting(why);
				firstPerson = false;
			}
			std::uint32_t slotWhy = 0;
			if (firstPerson && !IndirectDraws::Get().FirstPersonSlotReady(&tables, &lookups, lookupsCatalog.get(), a_slot, &slotWhy)) {
				waiting(8);
				firstPerson = false;
			}
			if (!firstPerson)
				phases &= ~kSetFirstPerson;
			// For the observers: held out, and why.
			if (!firstPerson) {
				d.firstPersonHeld = true;
				d.heldWhyCount = d.whyCount;
				d.firstPersonSlotWhy = slotWhy;
			}
		}
		// The reflection's faces need the slot's forward pipeline in the lookups' catalog (the one the revision resolves from).
		if ((phases & kSetReflection) && !IndirectDraws::Get().ReflectionSlotReady(&tables, lookupsCatalog.get(), a_slot)) {
			waiting(8);
			phases &= ~kSetReflection;
		}
		for (const std::uint8_t phase : { kSetCaster, kSetCasterPoint, kSetOccluderSky, kSetOccluderPrecipitation })
			if (std::uint32_t why = 7; (phases & phase) && !IndirectDraws::Get().PhaseReady(&tables, lookups, a_slot, phase, &why)) {
				waiting(why);
				phases &= ~phase;
			}
		if (phases == participation) {
			d.phases = phases;
			return d;
		}
		d.partial = phases != 0;
		return d;
	}

	void Coordinator::CommitSet()
	{
		ZoneScopedN("CS.DCLF.Scene.CommitSet");
		++setStats.commits;
		const std::size_t objects = tables.objects.size();
		// Not built (load screen): the set is empty.
		const bool live = sceneBuilt;
		const std::uint32_t drawn = live ? SetCapability() : 0u;
		// The commit's own copy; records and Tables::setPhases take it at the next ApplySet.
		auto& setPhases = setPhasesNext;
		setQueueMark.resize(objects, 0);
		setWaitingMark.resize(objects, 0);
		setLackingNext.resize(objects, 0);
		setApplyMark.resize(objects, 0);
		setWaitCause.resize(objects, 0);
		setWaitWhy.resize(objects, 0);
		setCommitFrame = sceneFrame;
		setCommitToggles = sceneInputs.togglesGeneration;
		setGeometryNext.resize(objects, nullptr);
		auto markApply = [&](std::uint32_t a_slot) { MarkSetApply(a_slot); };
		if (!setBuilding)
			setBuilding = std::make_shared<SetSnapshot>();
		// Without open gates or held-out blocks the commit does no gate work.
		const bool gatesLive = !lodGates.empty() || gateHeldOutCount != 0;
		for (const std::uint32_t slot : gateQueued)
			QueueSet(slot);
		gateQueued.clear();

		// Membership events: the change log since the last commit and readiness moves for waiting slots. New phases, new tables or a
		// log the cursor cannot continue evaluate every slot.
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
				markApply(slot);
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
		// New shadow view modes or rasterizer states (rare) may need pipelines a caster lacks: requeue every caster and waiting slot.
		// Read from the lane's lookups, never the render thread's.
		if (const std::uint64_t modes = lookups.shadowInputsSerial; modes != setShadowModes) {
			setShadowModes = modes;
			for (std::uint32_t slot = 0; slot < objects; ++slot)
				if (setPhases[slot] & ~kSetMain)
					QueueSet(slot);
			for (const std::uint32_t slot : setWaiting)
				QueueSet(slot);
		}
		// The joins ran earlier in this pass: whatever they rebound or dropped is queued here through LeaveSet and the change log.

		// Whether a waiting mark moved (setWaiting gained or lost a slot): else the list stands as the last compaction left it.
		bool waitingMoved = false;
		// DecideSlot plus the commit's side effects (wait causes and reasons, counts, observers).
		auto wanted = [&](std::uint32_t a_slot, bool& a_wait, bool* a_partial = nullptr) -> std::uint8_t {
			if (a_slot < setWaitCause.size())
				setWaitCause[a_slot] = 0;
			const SlotDecision decision = DecideSlot(a_slot, live, drawn);
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
			if (decision.firstPersonExcluded || decision.firstPersonHeld)
				Diagnostics::Notify(Diagnostics::SetSlotDecided{ *this, a_slot, decision, decision.heldWhyCount ? decision.whys[0] : whyBefore });
			return decision.phases;
		};
		auto apply = [&](std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait) {
			// Already pending application: refresh for the geometry it holds now.
			if (setApplyMark[a_slot])
				markApply(a_slot);
			// Occluder and reflection phases it participates in but misses.
			const std::uint8_t lacking = live ? static_cast<std::uint8_t>(SetParticipation(a_slot, drawn) & ~a_phases & (kSetOccluderSky | kSetOccluderPrecipitation | kSetReflection)) : 0;
			if (lacking != setLackingNext[a_slot]) {
				setLackingNext[a_slot] = lacking;
				markApply(a_slot);
			}
			if (a_wait != (setWaitingMark[a_slot] != 0)) {
				setWaitingMark[a_slot] = a_wait ? 1 : 0;
				waitingMoved = true;
				if (a_wait)
					setWaiting.push_back(a_slot);  // compacted below
			}
			const std::uint8_t before = setPhases[a_slot];
			if (before == a_phases)
				return;
			setPhases[a_slot] = a_phases;
			markApply(a_slot);  // the record's kObjectMember and Tables::setPhases, at ApplySet
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
		};
		for (std::size_t q = 0; q < setQueue.size(); ++q) {
			const std::uint32_t slot = setQueue[q];
			if (slot >= objects)
				continue;
			// A base and its layer are main-phase members together (the engine's main passes of both are the base geometry's).
			const std::uint32_t partner = tables.IsLayer(slot) ? tables.layerBase[slot] : slot < tables.layerOf.size() ? tables.layerOf[slot] : kNoObjectSlot;
			bool wait = false, partial = false;
			std::uint8_t phases = wanted(slot, wait, &partial);
			if (setPartialMark.size() < objects)
				setPartialMark.resize(objects, 0);
			if ((setPartialMark[slot] != 0) != partial) {
				setPartialMark[slot] = partial ? 1 : 0;
				setPartialCount += partial ? 1u : std::uint32_t(-1);
			}
			if (partner != kNoObjectSlot && partner < objects) {
				bool partnerWait = false;
				std::uint8_t partnerPhases = wanted(partner, partnerWait);
				if (!(phases & kSetMain) != !(partnerPhases & kSetMain)) {
					++setStats.waitingBy[6];
					// Either may become ready on any source.
					setWaitCause[slot] |= kWaitOther;
					setWaitCause[partner] |= kWaitOther;
					phases = 0;
					partnerPhases = 0;
				}
				// Gated incoming blocks hold their phases until the flip; outgoing ones drop after it.
				if (gatesLive)
					partnerPhases = GateWithhold(partner, partnerPhases, partnerWait, drawn, live);
				apply(partner, partnerPhases, partnerWait);
			}
			if (gatesLive)
				phases = GateWithhold(slot, phases, wait, drawn, live);
			apply(slot, phases, wait);
		}
		// Gates whose incoming geometries are all settled flip here and ride the publication (PublicationDeltas::gateFlips).
		if (gatesLive) {
			std::vector<std::pair<std::uint32_t, std::uint8_t>> flipped;
			UpdateGates(live, flipped);
			for (const auto& [slot, phases] : flipped)
				if (slot < objects)
					apply(slot, phases, false);
		}
		Diagnostics::Notify(Diagnostics::SetCommitted{ *this, live, drawn, gatesLive });
		setStats.evaluated += setQueue.size();
		for (const std::uint32_t slot : setQueue)
			if (slot < objects)
				setQueueMark[slot] = 0;
		setQueue.clear();
		if (waitingMoved) {
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
					const std::uint8_t bit = v == 2 ? kSetReflection : v ? kSetOccluderPrecipitation : kSetOccluderSky;
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
				// A first-person member draws with main bindings but has no kObjectMember.
				const bool firstPerson = (tables.setPhases[a_slot] & kSetFirstPerson) && !(flags & kObjectNoBindings) && draws.FitsScene(&tables, a_slot, false);
				const std::uint8_t others = draws.FitsScene(&tables, a_slot, true) ? static_cast<std::uint8_t>(tables.setPhases[a_slot] & ~(kSetMain | kSetReflection | kSetFirstPerson)) : 0;
				drawn = static_cast<std::uint8_t>((main ? (kSetMain | kSetReflection) : 0u) | (firstPerson ? kSetFirstPerson : 0u) | others);
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
		// Apply the committed set and revoke what cannot be drawn, so the publication draws exactly what it claims.
		ApplySet();
		PublishClaims();
		RevokeUndrawnClaims();
		// The lookups are the pass's (ResolveLookups before the commit judged readiness by them): a second resolve here could take a newer
		// catalog or shadow inputs than the commit judged.
		PublicationDeltas deltas;
		{
			ZoneScopedN("CS.DCLF.Scene.TakeDeltas");
			TakeDeltas(deltas);
		}
		PublishTables();
		PublishLookups();
		// The publication made and posted (the snapshot builder takes it).
		ZoneNamedN(postSnapshotZone, "CS.DCLF.Scene.PostSnapshotWork", true);
		auto publication = std::make_shared<ScenePublication>();
		publication->sequence = ++publicationSequence;
		publication->commitFrame = publicationCommitFrame;
		publication->togglesGeneration = publicationCommitToggles;
		// The LOD forced-release generation this pass's events are complete for (taken before its drains).
		publication->lodForcedGeneration = sceneInputs.lodForcedGeneration;
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
		a_deltas.fadeSeeds = std::exchange(fadeSeedRequests, {});
		a_deltas.treeSeeds = std::exchange(treeSeedRequests, {});
		// No cap: PrimaryCull reads them all only on a full walk.
		a_deltas.switches = std::exchange(switchesApplied, {});
		a_deltas.switchResync = std::exchange(switchResync, false);
		a_deltas.retiredImports = std::exchange(retiredImports, {});
		a_deltas.actorWetness = std::exchange(tables.actorWetnessChanges, {});
		a_deltas.lightEntries = std::exchange(lightEntryChanges, {});
		if (!gateFlipsMade.empty())
			Diagnostics::Notify<Diagnostics::ScenePoint::GateFlipsPublished>();
		a_deltas.gateFlips = std::exchange(gateFlipsMade, {});
	}

	void Coordinator::DropReturnedPublications()
	{
		channels.retiredPublications.Drop();
	}

	void Coordinator::TakeFrameInputs()
	{
		ZoneScopedN("CS.DCLF.Scene.TakeFrameInputs");
		// Latest wins; none keeps the last. Once a pass, at its start, so its globals and inputs are one frame's.
		if (!channels.frameInputs.TakeInto(sceneInputs))
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
		// In order: the content epoch of the depth commit's fade roots, then PrimaryCull's fade ownership. Each forces a publication.
		if (const std::uint64_t contentEpoch = channels.fadeRootsSentPosted.exchange(Channels::kNoFadeRootsSent, std::memory_order_acq_rel); contentEpoch != Channels::kNoFadeRootsSent) {
			tables.fadeRootsJournal.BeginBuild(contentEpoch);
			publishForced = true;
		}
		if (const auto owned = channels.fadeOwnedPosted.Take()) {
			ApplyFadeRootsOwned(*owned);
			publishForced = true;
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
			// The joins' inputs posted at EarlyPrepass (room map, pipeline frame, capture drain).
			ZoneScopedN("CS.DCLF.Scene.TakeAccumulateInputs");
			TakeAccumulateInputs();
		}
		{
			// Posts the drains must follow, then the engine queues drained to where each stands now.
			ZoneScopedN("CS.DCLF.Scene.TakeCategoryCapture");
			TakeCategoryCapture();
		}
		{
			// Gate outcomes are taken before the tracker's stack (an outcome is posted after its gate's events, so the batch holds them) and
			// applied after the batch, unless it is a load screen's (carried, not applied): those wait for the next.
			ZoneScopedN("CS.DCLF.Scene.TakeGateOutcomes");
			TakeGateOutcomes();
		}
		{
			auto batch = CollectEvents();
			const bool applied = batch != nullptr;
			{
				ZoneScopedN("CS.DCLF.Scene.ApplyEvents");
				ApplyEvents(std::move(batch));
			}
			ZoneScopedN("CS.DCLF.Scene.ApplyGateOutcomes");
			ApplyGateOutcomes(applied);
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
		tables.geometrySlots.TakeRetiring([&](std::uint32_t a_slot, std::uint32_t a_generation) { batch.slots.push_back({ Tables::kRetiredGeometrySlot, a_slot, a_generation, epoch }); });
		tables.materialSlots.TakeRetiring([&](std::uint32_t a_slot, std::uint32_t a_generation) { batch.slots.push_back({ Tables::kRetiredMaterialSlot, a_slot, a_generation, epoch }); });
		tables.pipelineSlots.TakeRetiring([&](std::uint32_t a_slot, std::uint32_t a_generation) { batch.slots.push_back({ Tables::kRetiredPipelineSlot, a_slot, a_generation, epoch }); });
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
				case Tables::kRetiredTree:
					tables.NoteTreesWrite();
					tables.treeFree.push_back(slot);
					break;
				case Tables::kRetiredFadeRoot:
					tables.NoteFadeRootsWrite();
					tables.fadeRootFree.push_back(slot);
					break;
				case Tables::kRetiredExtras:
					tables.extraFree.push_back(slot);
					break;
				case Tables::kRetiredBones:
					tables.BoneFreeList(retired.extra).push_back(slot);
					break;
				case Tables::kRetiredFaceStream:
					tables.faceStreamFree.push_back(slot);
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
				case Tables::kRetiredFaceRegion:
					faceRegionFree.push_back({ slot, retired.extra });
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
			// Of the snapshots only the pool holds (each lets go of its node; the last published keeps it: a frame may still accept it), the
			// one nearest the tables: its logs' ends furthest on, the least to replay. One the logs no longer reach is copied whole.
			std::size_t first = tablesPool.size();
			std::uint64_t nearest = 0;
			for (std::size_t i = 0; i < tablesPool.size(); ++i) {
				auto& pooled = tablesPool[i];
				if (pooled.use_count() != 1 || pooled == publishedTables)
					continue;
				pooled->retirementHold.reset();
				first = (std::min)(first, i);
				const bool reached = tablesPoolReplayable[i] && tables.changeLog.Readable(pooled->changeLog.End()) && tables.materialLog.Readable(pooled->materialLog.End());
				const std::uint64_t reach = reached ? pooled->changeLog.End() + pooled->materialLog.End() + 1 : 0;
				if (index == tablesPool.size() || reach > nearest) {
					index = i;
					nearest = reach;
				}
			}
			if (index != first)
				++pace.replayNearer;
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
		// Each logged column with the causes that cover it (CausesBetween): a replayed slot copies it only when a cause it was noted with
		// covers it (kChangeAll: any). A slot the publication log names takes every column.
		auto loggedColumns = [](Tables& a_left, Tables& a_right, auto&& a_column) {
			a_column(a_left.objects, a_right.objects, kChangeAll);
			a_column(a_left.draws, a_right.draws, kChangeAll);
			a_column(a_left.lights, a_right.lights, kChangeLights);
			a_column(a_left.treeAnim, a_right.treeAnim, kChangeTree);
			a_column(a_left.sunEntryNode, a_right.sunEntryNode, kChangeAll);
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
			a_column(a_left.faceStream, a_right.faceStream, kChangeAll);
			a_column(a_left.sceneFlags, a_right.sceneFlags, kChangeAll);
			a_column(a_left.skinPartitions, a_right.skinPartitions, kChangeSkin);
			a_column(a_left.skinLodPartitions, a_right.skinLodPartitions, kChangeSkin);
			a_column(a_left.shadowReject, a_right.shadowReject, kChangeShadow);
			// Residency is written ahead of the accumulate phase's snapshot (KeepResident): any cause.
			a_column(a_left.residentSlot, a_right.residentSlot, kChangeAll);
		};
		// Logged per-object columns sized apart from the objects (resized to the tables', then replayed by slot).
		auto sidedColumns = [](Tables& a_left, Tables& a_right, auto&& a_column) {
			a_column(a_left.setPhases, a_right.setPhases, std::uint8_t{ 0 });
			a_column(a_left.decalOrdinal, a_right.decalOrdinal, ~0u);
			a_column(a_left.layerBase, a_right.layerBase, kNoObjectSlot);
			a_column(a_left.layerOf, a_right.layerOf, kNoObjectSlot);
		};
		// Not published: the coordinator's alone - its walk stamps and templates, the slot tables' reference counts and free lists, the
		// writers' scratch (shadow texture transitions, freed face streams, the wetness index the frame keeps from the log's changes), and
		// what is retiring. No reader of a publication reads them; a publication's stay as they were made (empty).
		auto unpublished = [](Tables& a_left, Tables& a_right, auto&& a_column) {
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
			a_column(a_left.faceStreamFree, a_right.faceStreamFree);
			a_column(a_left.faceStreamsReleased, a_right.faceStreamsReleased);
			a_column(a_left.actorWetness, a_right.actorWetness);
			a_column(a_left.actorWetnessChanges, a_right.actorWetnessChanges);
			a_column(a_left.shadowTextureSeen, a_right.shadowTextureSeen);
			a_column(a_left.shadowTextureChanges, a_right.shadowTextureChanges);
			a_column(a_left.retiring, a_right.retiring);
		};
		// Covered by the geometry log (geometryLastUsed is copied whole).
		auto geometryColumns = [](Tables& a_left, Tables& a_right, auto&& a_column) {
			a_column(a_left.geometries, a_right.geometries);
			a_column(a_left.geometryImports, a_right.geometryImports);
			a_column(a_left.geometrySlotKey, a_right.geometrySlotKey);
			a_column(a_left.geometryLayerKey, a_right.geometryLayerKey);
		};
		// The families copied only when their write stamp moved.
		auto treeFamily = [](Tables& a_left, Tables& a_right, auto&& a_member) { Tables::TreeFamily(a_left, a_right, a_member); };
		auto fadeRootFamily = [](Tables& a_left, Tables& a_right, auto&& a_member) { Tables::FadeRootFamily(a_left, a_right, a_member); };
		auto pipelineFamily = [](Tables& a_left, Tables& a_right, auto&& a_member) { Tables::PipelineFamily(a_left, a_right, a_member); };
		auto constantsFamily = [](Tables& a_left, Tables& a_right, auto&& a_member) {
			a_member(a_left.pipelineConstants, a_right.pipelineConstants);
			a_member(a_left.techniqueConstants, a_right.techniqueConstants);
		};
		auto heldOut = [&](Tables& a_left, Tables& a_right) {
			loggedColumns(a_left, a_right, [](auto& a_x, auto& a_y, std::uint32_t) { a_x.swap(a_y); });
			sidedColumns(a_left, a_right, [](auto& a_x, auto& a_y, auto) { a_x.swap(a_y); });
			unpublished(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			pipelineFamily(a_left, a_right, [](auto& a_x, auto& a_y) { a_x.swap(a_y); });
			constantsFamily(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			treeFamily(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			fadeRootFamily(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			geometryColumns(a_left, a_right, [](auto& a_x, auto& a_y) { a_x.swap(a_y); });
			a_left.extraRows.swap(a_right.extraRows);
			a_left.materials.swap(a_right.materials);
			a_left.materialVersion.swap(a_right.materialVersion);
			a_left.materialFrameVersion.swap(a_right.materialFrameVersion);
			std::swap(a_left.changeLog, a_right.changeLog);
			std::swap(a_left.geometryLog, a_right.geometryLog);
			std::swap(a_left.materialLog, a_right.materialLog);
			std::swap(a_left.extrasBlockLog, a_right.extrasBlockLog);
			std::swap(a_left.publicationLog, a_right.publicationLog);
		};
		const std::uint64_t position = snapshot.changeLog.End();
		const std::uint64_t publicationPosition = snapshot.publicationLog.End();
		const std::uint64_t materialPosition = snapshot.materialLog.End();
		const std::uint64_t geometryPosition = snapshot.geometryLog.End();
		const std::uint64_t extrasPosition = snapshot.extrasBlockLog.End();
		const bool replayExtras = tablesPoolReplayable[index] && tables.extrasBlockLog.Readable(extrasPosition) &&
		                          snapshot.extrasBlockLog.base <= tables.extrasBlockLog.base && snapshot.extraRows.size() <= tables.extraRows.size();
		const bool replayGeometry = tablesPoolReplayable[index] && tables.geometryLog.Readable(geometryPosition) &&
		                            snapshot.geometryLog.base <= tables.geometryLog.base && snapshot.geometries.size() <= tables.geometries.size();
		const bool replay = tablesPoolReplayable[index] && tables.changeLog.Readable(position) && snapshot.changeLog.base <= tables.changeLog.base &&
		                    tables.publicationLog.Readable(publicationPosition) && snapshot.publicationLog.base <= tables.publicationLog.base &&
		                    snapshot.objects.size() <= tables.objects.size();
		const bool replayMaterials = tablesPoolReplayable[index] && tables.materialLog.Readable(materialPosition) && snapshot.materialLog.base <= tables.materialLog.base &&
		                             snapshot.materials.size() <= tables.materials.size();
		// Read before the copy writes the snapshot's stamps.
		const bool keepTrees = tablesPoolReplayable[index] && snapshot.treesStamp == tables.treesStamp;
		const bool keepFadeRoots = tablesPoolReplayable[index] && snapshot.fadeRootsStamp == tables.fadeRootsStamp;
		const bool keepConstants = tablesPoolReplayable[index] && snapshot.constantsStamp == tables.constantsStamp;
		const bool keepPipelines = tablesPoolReplayable[index] && snapshot.pipelinesStamp == tables.pipelinesStamp;
		// The material versions are held out with the records (replayed by the material log, or compared below); none are trusted after
		// a whole copy of the objects.
		bool versionsKnown = tablesPoolReplayable[index] != 0;
		{
			// Swap the held-out members aside on both (restored however the copy ends), copy the rest whole.
			ZoneScopedN("CS.DCLF.Scene.PublishTables.Rest");
			Tables snapshotHeld, tablesHeld;
			heldOut(snapshot, snapshotHeld);
			heldOut(tables, tablesHeld);
			struct Restore
			{
				std::function<void()> undo;
				~Restore() { undo(); }
			} restore{ [&] { heldOut(tables, tablesHeld); } };
			snapshot = tables;
			heldOut(snapshot, snapshotHeld);
		}
		const auto restEnd = std::chrono::steady_clock::now();
		const double restMs = std::chrono::duration<double, std::milli>(restEnd - start).count();
		publication.restMs += restMs;
		pace.restMs += restMs;

		// The replay's two halves, collected once on the lane: the object slots the logs name, sorted, with the causes each gathered
		// (replaySlotCauses; kChangeAll for a publication-log slot), and the material slots the material log names, sorted. Their copies are
		// jobs over disjoint columns and disjoint record ranges (below).
		const std::size_t objectSlots = tables.objects.size();
		publication.objectSlots += objectSlots;
		auto& slots = replaySlots;
		auto& causes = replaySlotCauses;
		slots.clear();
		if (replay) {
			ZoneScopedN("CS.DCLF.Scene.PublishTables.Collect");
			snapshot.GrowObjects(objectSlots);
			sidedColumns(snapshot, tables, [](auto& a_to, auto& a_from, auto a_initial) { a_to.resize(a_from.size(), a_initial); });
			if (causes.size() < objectSlots)
				causes.resize(objectSlots, 0);
			auto name = [&](std::uint32_t a_slot, std::uint32_t a_causes) {
				if (a_slot >= objectSlots)
					return;
				if (!causes[a_slot])
					slots.push_back(a_slot);
				causes[a_slot] |= a_causes;
			};
			const auto changes = tables.changeLog.From(position);
			for (const auto& change : changes)
				name(change.slot, change.causes ? change.causes : kChangeAll);
			const auto published = tables.publicationLog.From(publicationPosition);
			for (const std::uint32_t slot : published)
				name(slot, kChangeAll);
			std::sort(slots.begin(), slots.end());
			pace.replayLogEntries += changes.size() + published.size();
		} else {
			++publication.wholeCopies;
			loggedColumns(snapshot, tables, [](auto& a_to, auto& a_from, std::uint32_t) { a_to = a_from; });
			sidedColumns(snapshot, tables, [](auto& a_to, auto& a_from, auto) { a_to = a_from; });
			snapshot.changeLog = tables.changeLog;
			snapshot.publicationLog = tables.publicationLog;
			snapshot.geometryLog = tables.geometryLog;
			snapshot.materialLog = tables.materialLog;
			versionsKnown = false;
		}
		// Material records by the slots the material log names (every write of a record notes its slot); a snapshot the log no longer
		// reaches by version (session-unique per write; none kept after a whole copy).
		const std::size_t materialCount = tables.materials.size();
		snapshot.materials.resize(materialCount);
		auto& materialSlots = replayMaterialSlots;
		materialSlots.clear();
		if (replayMaterials) {
			// The versions with their records (a grown slot's are 0 until its first write, which the log names).
			snapshot.materialVersion.resize(materialCount, 0);
			snapshot.materialFrameVersion.resize(materialCount, 0);
			replayMaterialMarks.assign(materialCount, 0);
			const auto noted = tables.materialLog.From(materialPosition);
			for (const std::uint32_t slot : noted)
				if (slot < materialCount && !std::exchange(replayMaterialMarks[slot], std::uint8_t{ 1 }))
					materialSlots.push_back(slot);
			std::sort(materialSlots.begin(), materialSlots.end());
			pace.replayMaterialEntries += noted.size();
			publication.materialsReplayed += materialSlots.size();
			pace.replayMaterials += materialSlots.size();
		} else {
			const auto& recordVersions = snapshot.materialVersion;
			const auto& recordFrameVersions = snapshot.materialFrameVersion;
			for (std::size_t slot = 0; slot < materialCount; ++slot) {
				if (versionsKnown && slot < recordVersions.size() && recordVersions[slot] == tables.materialVersion[slot] && slot < recordFrameVersions.size() &&
					recordFrameVersions[slot] == tables.materialFrameVersion[slot])
					continue;
				snapshot.materials[slot] = tables.materials[slot];
				++publication.materialsReplayed;
			}
			snapshot.materialVersion = tables.materialVersion;
			snapshot.materialFrameVersion = tables.materialFrameVersion;
		}
		publication.materialSlots += materialCount;

		{
			// The jobs: one per object column (its named slots, by contiguous runs, skipping slots no cause of theirs covers it for), then one
			// per chunk of material slots (records and versions, by runs). Each writes only its own column or record range of the snapshot,
			// which only the pool holds, and reads the tables, which nothing writes meanwhile: pure, so the pool may run them in any order.
			std::size_t columnJobs = 0;
			if (replay) {
				loggedColumns(snapshot, tables, [&](auto&, auto&, std::uint32_t) { ++columnJobs; });
				sidedColumns(snapshot, tables, [&](auto&, auto&, auto) { ++columnJobs; });
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
					loggedColumns(snapshot, tables, column);
					sidedColumns(snapshot, tables, [&](auto& a_to, auto& a_from, auto) { column(a_to, a_from, kChangeAll); });
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

		if (replay) {
			// A layer link's other end: the layer's write notes only the layer.
			for (const std::uint32_t slot : slots)
				for (const std::uint32_t end : { tables.layerBase[slot], tables.layerOf[slot] })
					if (end < objectSlots && !causes[end]) {
						snapshot.layerBase[end] = tables.layerBase[end];
						snapshot.layerOf[end] = tables.layerOf[end];
					}
			for (const std::uint32_t slot : slots)
				causes[slot] = 0;
			publication.objectsReplayed += slots.size();
			pace.replaySlots += slots.size();
			// Drop the trimmed head, append what was gained.
			auto replayLog = [](auto& a_to, const auto& a_from) {
				const std::uint64_t end = a_to.End();
				if (!a_from.Readable(end) || a_to.base > a_from.base) {
					a_to = a_from;
					return;
				}
				const std::size_t dropped = static_cast<std::size_t>(std::min<std::uint64_t>(a_from.base - a_to.base, a_to.entries.size()));
				a_to.entries.erase(a_to.entries.begin(), a_to.entries.begin() + std::ptrdiff_t(dropped));
				a_to.base = a_from.base;
				const auto gained = a_from.From(end);
				a_to.entries.insert(a_to.entries.end(), gained.begin(), gained.end());
			};
			replayLog(snapshot.changeLog, tables.changeLog);
			replayLog(snapshot.publicationLog, tables.publicationLog);
			replayLog(snapshot.geometryLog, tables.geometryLog);
			replayLog(snapshot.materialLog, tables.materialLog);
		}
		// The extras, the families and the geometry columns.
		ZoneNamedN(familiesZone, "CS.DCLF.Scene.PublishTables.Families", true);
		// Extras rows by the blocks the block log names.
		if (replayExtras) {
			snapshot.extraRows.resize(tables.extraRows.size(), 0.0f);
			for (const std::uint32_t offset : tables.extrasBlockLog.From(extrasPosition))
				if ((std::size_t(offset) + kExtraRows) * 4 <= tables.extraRows.size())
					std::copy_n(tables.extraRows.begin() + std::ptrdiff_t(offset) * 4, std::size_t(kExtraRows) * 4, snapshot.extraRows.begin() + std::ptrdiff_t(offset) * 4);
			const auto gained = tables.extrasBlockLog.From(extrasPosition);
			const std::size_t dropped = static_cast<std::size_t>(std::min<std::uint64_t>(tables.extrasBlockLog.base - snapshot.extrasBlockLog.base, snapshot.extrasBlockLog.entries.size()));
			snapshot.extrasBlockLog.entries.erase(snapshot.extrasBlockLog.entries.begin(), snapshot.extrasBlockLog.entries.begin() + std::ptrdiff_t(dropped));
			snapshot.extrasBlockLog.base = tables.extrasBlockLog.base;
			snapshot.extrasBlockLog.entries.insert(snapshot.extrasBlockLog.entries.end(), gained.begin(), gained.end());
		} else {
			snapshot.extraRows = tables.extraRows;
			snapshot.extrasBlockLog = tables.extrasBlockLog;
		}
		auto assign = [](auto& a_to, auto& a_from) { a_to = a_from; };
		if (keepTrees)
			++publication.treesKept;
		else
			treeFamily(snapshot, tables, assign);
		if (keepFadeRoots)
			++publication.fadeRootsKept;
		else
			fadeRootFamily(snapshot, tables, assign);
		if (!keepConstants)
			constantsFamily(snapshot, tables, assign);
		if (!keepPipelines)
			pipelineFamily(snapshot, tables, assign);
		// The geometry columns on the lane (an import lease the snapshot drops is let go here, never on a pool worker).
		const std::size_t geometrySlots = tables.geometries.size();
		publication.geometrySlots += geometrySlots;
		if (replayGeometry) {
			snapshot.GeometryColumns([geometrySlots](auto& a_column, auto&&... a_initial) { a_column.resize(geometrySlots, a_initial...); });
			auto& marks = replayGeometryMarks;
			marks.assign(geometrySlots, 0);
			for (const std::uint32_t slot : tables.geometryLog.From(geometryPosition)) {
				if (slot >= geometrySlots || std::exchange(marks[slot], std::uint8_t{ 1 }))
					continue;
				geometryColumns(snapshot, tables, [slot](auto& a_to, auto& a_from) { a_to[slot] = a_from[slot]; });
				++publication.geometriesReplayed;
			}
		} else {
			++publication.geometryWholeCopies;
			geometryColumns(snapshot, tables, [](auto& a_to, auto& a_from) { a_to = a_from; });
		}
		tablesPoolReplayable[index] = 1;

		Diagnostics::Notify(Diagnostics::TablesReplayed{ *this, snapshot, publication.published, position, geometryPosition, keepTrees, keepFadeRoots, keepConstants });
		// The publication log's readers are the snapshots alone (one behind the bound is copied whole).
		tables.publicationLog.Trim(1u << 16);
		// Its node collects what is freed after it.
		snapshot.retirementHold = retirement.Publish();
		publishedTables = target;
		++publication.published;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		publication.ms += ms;
		publication.maxMs = std::max(publication.maxMs, ms);
	}

	void Coordinator::CheckPacedPublication(const Tables& a_published, const SetSnapshot& a_claims)
	{
		// A publication that folded deferred passes in must equal the one an every-pass publication would have made at this pass: its
		// tables the coordinator's (by the columns every reader builds from, the log ends and the families' stamps), its claims the set
		// copied whole and revoked (claimsReference).
		auto& p = paceStats;
		++p.parityChecks;
		std::string first;
		const auto& t = tables;
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
		if (a_published.setPhases != t.setPhases || a_published.decalOrdinal != t.decalOrdinal || a_published.layerBase != t.layerBase || a_published.layerOf != t.layerOf)
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
			a_published.extrasBlockLog.End() != t.extrasBlockLog.End() || a_published.treesStamp != t.treesStamp || a_published.fadeRootsStamp != t.fadeRootsStamp ||
			a_published.constantsStamp != t.constantsStamp || a_published.pipelinesStamp != t.pipelinesStamp || a_published.extraRows != t.extraRows)
			differ("a log's end, a family's stamp or the extras rows");
		if (!first.empty()) {
			++p.parityTables;
			if (p.parityFirst.empty())
				p.parityFirst = fmt::format("tables: {} (publication {})", first, publicationSequence);
		}
		if (!claimsReference)
			return;
		// The first difference by key, both ways (a member only one side holds shows 0 on the other), then the drawn phases.
		std::string claimsFirst;
		for (const auto& [geometry, phases] : claimsReference->phases)
			if (const std::uint8_t published = a_claims.PhasesOf(geometry); published != phases) {
				claimsFirst = fmt::format("geometry {} published {:#x}, made whole {:#x}", static_cast<const void*>(geometry), published, phases);
				break;
			}
		if (claimsFirst.empty())
			for (const auto& [geometry, phases] : a_claims.phases)
				if (claimsReference->phases.find(geometry) == claimsReference->phases.end()) {
					claimsFirst = fmt::format("geometry {} published {:#x}, made whole 0x0", static_cast<const void*>(geometry), phases);
					break;
				}
		if (claimsFirst.empty() && claimsReference->drawn != a_claims.drawn)
			claimsFirst = fmt::format("drawn phases published {:#x}, made whole {:#x}", a_claims.drawn, claimsReference->drawn);
		if (!claimsFirst.empty()) {
			++p.parityClaims;
			if (p.parityFirst.empty())
				p.parityFirst = fmt::format("claims: {} ({} members against {} made whole; publication {})", claimsFirst, a_claims.phases.size(),
					claimsReference->phases.size(), publicationSequence);
		}
	}
}
