#include "Internal.h"

#include "Features/DrawcallLimitFix/Engine/ImportTimings.h"

#include "Features/DrawcallLimitFix/Draws/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Common/AsyncWorker.h"
#include "Features/DrawcallLimitFix/Common/EngineReleases.h"
#include "Features/DrawcallLimitFix/Common/Toggles.h"

// The DCLF set (SceneSet.h; drawcall-limit-fix.md, "The DCLF set"): who draws an object, decided once a frame.

namespace DCLF
{
	std::uint32_t SceneStore::SetCapability()
	{
		// The main camera's passes are DCLF's whenever it runs; the shadow and occlusion views' once their setup is complete (each
		// object then waits for its own pipelines: PhaseReady); the reflection's once its resources exist.
		return kSetMain | (ActiveToggles().shadows ? IndirectDraws::Get().ShadowCapability() : 0u) |
		       (IndirectDraws::Get().ReflectionDrawable() ? kSetReflection : 0u);
	}

	std::uint8_t SceneStore::SetParticipation(std::uint32_t a_slot, std::uint32_t a_drawn) const
	{
		const auto flags = tables.objects[a_slot].flags;
		if (flags & kObjectFree)
			return 0;
		std::uint32_t phases = 0;
		if (!(flags & kObjectShadowOnly))
			phases |= kSetMain;
		// The reflection's faces draw the LOD techniques (dclf-lod.md, "The census").
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

	bool SceneStore::MainReady(std::uint32_t a_slot, std::uint32_t& a_why) const
	{
		// What a main build's pair resolution needs from the lookups (MainBuild::ResolvePair, WriteMaterialRow, WritePipelineRow): the
		// scene lane's own, as this pass resolved them (ResolveLookups), which the publication carries to the builds. What it checks
		// beyond these (constant groups that fit, the samplers a material names) is the tables' own structure: a member it fails is a
		// defect, which set parity reports.
		const auto& object = tables.objects[a_slot];
		const std::uint32_t p = object.pipelineIndex, m = object.materialIndex;
		if ((object.flags & kObjectNoBindings) || !PipelineDrawableIn(tables, lookups, p))
			return a_why = 0, false;
		if (m >= lookups.materials.size() || m >= tables.materialSlotKey.size() || !lookups.materials[m].resolved || lookups.materials[m].key != tables.materialSlotKey[m])
			return a_why = 1, false;
		// Its pipeline's and technique's constants, as the coordinator holds them (posted by the frame's evaluations).
		if (!tables.PipelineConstantsCurrent(p) || !tables.TechniqueConstantsValid(p))
			return a_why = 9, false;
		if (tables.TechniqueShadowMask(p) && lookups.pipelines[p].shadowMaskIndex == Lookups::kNone)
			return a_why = 2, false;
		if (!lookups.samplersResolved || lookups.nullTexture == Lookups::kNone)
			return a_why = 3, false;
		// A ProjectedUV pipeline binds the projected textures where its material binds none (but the Hair technique, which binds none).
		if (const auto pass = tables.pipelines[p].passDescriptor; (pass & 0x8000u) && ((pass >> 24) & 0x3f) != 6)
			for (const auto index : lookups.projectedTextures)
				if (index == Lookups::kNone)
					return a_why = 3, false;
		if (object.geometryIndex >= tables.geometries.size() || !tables.geometries[object.geometryIndex].vertexAddress ||
			!tables.geometries[object.geometryIndex].indexAddress)
			return a_why = 4, false;
		// Within the scene buffers the main builds ahead are made against: past them it has no input until their growth.
		if (!IndirectDraws::Get().FitsScene(&tables, a_slot, false))
			return a_why = 10, false;
		// A decal draws at its slot in its group's range (OrderDecals), and one slot holds one sequence: one of several partitions
		// has none.
		if (const std::uint32_t group = ObjectDecalGroup(object.flags))
			if (a_slot >= tables.decalOrdinal.size() || tables.decalOrdinal[a_slot] >= tables.decalCount[group - 1] ||
				(a_slot < tables.skinPartitions.size() && tables.skinPartitions[a_slot]))
				return a_why = 5, false;
		return true;
	}

	void SceneStore::QueueSet(std::uint32_t a_slot)
	{
		if (a_slot >= setQueueMark.size() || setQueueMark[a_slot])
			return;
		setQueueMark[a_slot] = 1;
		setQueue.push_back(a_slot);
	}

	void SceneStore::LeaveSet(std::uint32_t a_slot, std::uint8_t a_lost, bool a_partner)
	{
		// A claim is the whole object (U5): losing any phase it holds loses all of them.
		if (a_slot < setPhasesNext.size() && (setPhasesNext[a_slot] & a_lost))
			a_lost = setPhasesNext[a_slot];
		if (a_slot < setPhasesNext.size() && (setPhasesNext[a_slot] & a_lost)) {
			setPhasesNext[a_slot] &= static_cast<std::uint8_t>(~a_lost);
			// The engine's copy is by geometry, a base's alone (CommitSet's apply).
			// The claim is the slot's only while it owns the geometry's (another slot may hold it since).
			if (const auto* geometry = a_slot < setGeometry.size() ? setGeometry[a_slot] : nullptr; geometry && setBuilding) {
				const auto owner = setMemberSlot.find(geometry);
				const bool owns = owner != setMemberSlot.end() && owner->second == a_slot;
				if (setPhasesNext[a_slot]) {
					if (owns)
						setBuilding->phases.insert_or_assign(geometry, setPhasesNext[a_slot]);
				} else {
					if (owns) {
						setMemberSlot.erase(owner);
						setBuilding->phases.erase(geometry);
					}
					setGeometry[a_slot] = nullptr;
				}
				setSnapshotDirty = owns || setSnapshotDirty;
			}
			++setStats.leftAfterCommit;
			QueueSet(a_slot);
		}
		if (!a_partner || !(a_lost & kSetMain) || a_slot >= tables.objects.size())
			return;
		const std::uint32_t partner = tables.IsLayer(a_slot) ? tables.layerBase[a_slot] : a_slot < tables.layerOf.size() ? tables.layerOf[a_slot] : kNoObjectSlot;
		if (partner == kNoObjectSlot || partner >= tables.objects.size())
			return;
		constexpr std::uint8_t kMainPhases = kSetMain | kSetReflection;
		// The partner leaves whole (U5).
		if (PhasesIn(tables, partner) & kMainPhases) {
			tables.setPhases[partner] = 0;
			tables.objects[partner].flags &= ~kObjectMember;
			tables.NoteChange(partner, kChangeBindings);
		}
		LeaveSet(partner, kMainPhases, false);
	}

	void SceneStore::RefreshSetSnapshot()
	{
		if ((!setSnapshotDirty && setSnapshot) || !setBuilding)
			return;
		auto snapshot = std::make_shared<SetSnapshot>(*setBuilding);
		snapshot->frame = sceneFrame;
		snapshot->drawn = static_cast<std::uint8_t>(setPhaseMask);
		setSnapshot = std::move(snapshot);
		setSnapshotDirty = false;
		++setStats.publications;
	}

	void SceneStore::CommitSet()
	{
		ZoneScopedN("CS.DCLF.Scene.CommitSet");
		++setStats.commits;
		const std::size_t objects = tables.objects.size();
		// A load screen: nothing is drawn, and the set is empty.
		const bool live = sceneBuilt;
		const std::uint32_t drawn = live ? SetCapability() : 0u;
		// The commit's own copy: the records and Tables::setPhases take it at the next ApplySet.
		auto& setPhases = setPhasesNext;
		setQueueMark.resize(objects, 0);
		setWaitingMark.resize(objects, 0);
		setRebinding.resize(objects, 0);
		setLackingNext.resize(objects, 0);
		setApplyMark.resize(objects, 0);
		setWaitCause.resize(objects, 0);
		setWaitWhy.resize(objects, 0);
		setCommitFrame = sceneFrame;
		setCommitToggles = sceneInputs.togglesGeneration;
		const bool timeline = TimelineEnabled();
		// A slot whose phases or lacking phases this commit changed, for ApplySet, with the geometry it holds now (an earlier commit's
		// entry not yet applied takes this one's geometry: the decision is now for it).
		setGeometryNext.resize(objects, nullptr);
		auto markApply = [&](std::uint32_t a_slot) {
			setGeometryNext[a_slot] = tables.objectGeometry[a_slot];
			if (const std::uint32_t at = setApplyMark[a_slot]) {
				setApply[at - 1].second = tables.objectGeometry[a_slot];
				return;
			}
			setApply.emplace_back(a_slot, tables.objectGeometry[a_slot]);
			setApplyMark[a_slot] = static_cast<std::uint32_t>(setApply.size());
		};
		if (!setBuilding)
			setBuilding = std::make_shared<SetSnapshot>();

		// What can have changed an object's membership, as events: the change log since the last commit (every write of a record,
		// its residency or its bindings, whoever made it), the waiting slots when a lookup they wait for moved, a new set of
		// phases. A log this cursor cannot read on, new tables or new phases evaluate every slot.
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
			// Every slot's record and lacking phases are taken again by ApplySet, whatever this commit decides for it.
			for (std::uint32_t slot = 0; slot < objects; ++slot)
				markApply(slot);
			setBuilding->phases.clear();
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
		// Readiness moved, by source: anything the lookups resolve is a new version of them (Lookups::NextVersion), the shadow lookups
		// have their own generation, the constants their stamp, the scene buffers their fit serial. A waiting slot is taken again when
		// a source it waits on moved (SetWaitCause), or any moved for one waiting on something else.
		const std::array<std::uint64_t, kWaitSources> readiness{ (std::uint64_t(lookups.versionCounter) << 32) ^ lookups.generation, lookups.shadowGeneration,
			tables.constantsStamp, IndirectDraws::Get().SceneFitSerial() };
		std::uint8_t moved = 0;
		for (std::uint32_t source = 0; source < kWaitSources; ++source)
			if (readiness[source] != setReadiness[source]) {
				setReadiness[source] = readiness[source];
				moved |= std::uint8_t(1u << source);
			}
		if (moved) {
			++setStats.readinessEvents;
			for (const std::uint32_t slot : setWaiting)
				if (slot < setWaitCause.size() && (setWaitCause[slot] & (moved | kWaitOther))) {
					QueueSet(slot);
					++setStats.waitingRequeued;
				}
		}
		// The shadow views' modes and rasterizer states changed: a member's casting may need pipelines it has not got, so every
		// member and waiting slot is taken again (rare: a kind of view seen for the first time).
		if (const std::uint64_t modes = IndirectDraws::Get().ShadowReadinessSerial(); modes != setShadowModes) {
			setShadowModes = modes;
			for (std::uint32_t slot = 0; slot < objects; ++slot)
				if (setPhases[slot] & ~kSetMain)
					QueueSet(slot);
			for (const std::uint32_t slot : setWaiting)
				QueueSet(slot);
		}
		// The frame globals a membership pass reads changed: this frame's accumulate phase binds every resident again
		// (BindByMembership), so none of them is a member this frame.
		const bool rebindAll = live && sceneInputs.membershipWitness != membershipWitness;
		if (rebindAll)
			for (const std::uint32_t slot : residents)
				QueueSet(slot);
		// The records the walk wrote this frame whose binding does not stand: this frame's accumulate phase binds them again
		// (BindByMembership's rule), with slots that may not be ready, so they leave before it does.
		for (const std::uint32_t slot : bindQueue) {
			if (slot >= objects || !IsResidentSlot(slot))
				continue;
			auto* geometry = tables.objectGeometry[slot];
			const auto entry = geometry ? tracked.find(geometry) : tracked.end();
			if (entry == tracked.end() || (tables.IsLayer(slot) ? entry->second.layerSlot : entry->second.slot) != slot || entry->second.objectStamp != objectStamp ||
				entry->second.candidateReason != Ineligible::None)
				continue;
			if (!MemberBindingStands(slot, entry->second)) {
				setRebinding[slot] = 1;
				QueueSet(slot);
			}
		}
		// Fades DCLF does not model (PassCapture::FadingAtRegistration: blended, or a decal's): the registrations that met one in
		// a member, and the members whose fade has ended since.
		PassCapture::Get().TakeUnmodelledFades([&](const RE::BSGeometry* a_geometry) {
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
			if (entry == tracked.end() || !setFadeHeld.insert(a_geometry).second)
				return;
			if (entry->second.slot != kNoObjectSlot)
				QueueSet(entry->second.slot);
			if (entry->second.layerSlot != kNoObjectSlot)
				QueueSet(entry->second.layerSlot);
		});
		for (auto it = setFadeHeld.begin(); it != setFadeHeld.end();) {
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(*it));
			// Its fade node's currentFade, the mirror's (T6b1b).
			const auto* fadeNode = entry != tracked.end() ? mirror.Leaf(entry->first).fadeNode : nullptr;
			const bool fading = fadeNode && fadeNode->currentFade < 1.0f;
			if (entry != tracked.end() && fading) {
				++it;
				continue;
			}
			if (entry != tracked.end()) {
				if (entry->second.slot != kNoObjectSlot)
					QueueSet(entry->second.slot);
				if (entry->second.layerSlot != kNoObjectSlot)
					QueueSet(entry->second.layerSlot);
			}
			it = setFadeHeld.erase(it);
		}

		// The set parity's pass (below) decides every slot again without the commit's side effects (counters, causes, lags).
		bool parityPass = false;
		// One slot's phases now (0: not a member): every phase it takes part in when it is ready for all of them, none otherwise (U5: a
		// claim is the whole object); a_wait: it takes part in a phase it is not ready for. a_partial: some of its phases were ready
		// (what the per-phase claims would have drawn; the report counts them).
		auto wanted = [&](std::uint32_t a_slot, bool& a_wait, bool* a_partial = nullptr) -> std::uint8_t {
			a_wait = false;
			if (a_slot < setWaitCause.size() && !parityPass)
				setWaitCause[a_slot] = 0;
			if (!live || a_slot >= objects)
				return 0;
			const std::uint8_t participation = SetParticipation(a_slot, drawn);
			std::uint8_t phases = participation;
			if (!phases)
				return 0;
			auto waiting = [&](std::uint32_t a_why) {
				a_wait = true;
				if (parityPass)
					return;
				if (!setWaitCause[a_slot])
					setWaitWhy[a_slot] = static_cast<std::uint8_t>(a_why);
				setWaitCause[a_slot] |= WaitCauseOf(a_why);
				++setStats.waitingBy[a_why];
				if (setStats.firstWaiting.empty())
					if (const auto* geometry = tables.objectGeometry[a_slot])
						setStats.firstWaiting = fmt::format("'{}' (slot {}, reason {})", geometry->name.c_str() ? geometry->name.c_str() : "?", a_slot, a_why);
			};
			// The main phase needs its binding; the shadow and occlusion phases draw from the record alone (a shadow-only caster has
			// no binding).
			if (phases & kSetMain) {
				bool main = IsResidentSlot(a_slot);
				if (main && (rebindAll || setRebinding[a_slot])) {
					setStats.rebinding += parityPass ? 0 : 1;
					main = false;
				}
				if (const auto* geometry = tables.objectGeometry[a_slot]; main && geometry && setFadeHeld.contains(geometry))
					main = false;
				if (std::uint32_t why = 0; main && !MainReady(a_slot, why)) {
					waiting(why);
					main = false;
				}
				if (!main)
					phases &= ~kSetMain;
			}
			// The reflection's faces draw from the frame's scene list (IndirectDraws::ExecuteReflection): its pipeline slot's forward
			// pipeline ready.
			if ((phases & kSetReflection) && !IndirectDraws::Get().PhaseReady(&tables, lookups, a_slot, kSetReflection)) {
				waiting(8);
				phases &= ~kSetReflection;
			}
			for (const std::uint8_t phase : { kSetCaster, kSetCasterPoint, kSetOccluderSky, kSetOccluderPrecipitation })
				if (std::uint32_t why = 7; (phases & phase) && !IndirectDraws::Get().PhaseReady(&tables, lookups, a_slot, phase, &why)) {
					waiting(why);
					phases &= ~phase;
				}
			// Whole or nothing.
			if (phases == participation)
				return phases;
			if (a_partial)
				*a_partial = phases != 0;
			return 0;
		};
		auto apply = [&](std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait) {
			// Waiting for its application: decided again, for the geometry it holds now.
			if (setApplyMark[a_slot])
				markApply(a_slot);
			// The occluder and reflection phases it takes part in and misses (SetLacking).
			const std::uint8_t lacking = live ? static_cast<std::uint8_t>(SetParticipation(a_slot, drawn) & ~a_phases & (kSetOccluderSky | kSetOccluderPrecipitation | kSetReflection)) : 0;
			if (lacking != setLackingNext[a_slot]) {
				setLackingNext[a_slot] = lacking;
				markApply(a_slot);
			}
			if (a_wait != (setWaitingMark[a_slot] != 0)) {
				setWaitingMark[a_slot] = a_wait ? 1 : 0;
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
			if (timeline && !before && a_phases)
				NoteTimelineJoin(a_slot);
			// The engine's copy is by geometry, a base's alone: a layer draws its base's geometry and is a member with it.
			if (tables.IsLayer(a_slot))
				return;
			if (const auto* old = setGeometry[a_slot]; old && !a_phases) {
				if (const auto owner = setMemberSlot.find(old); owner != setMemberSlot.end() && owner->second == a_slot) {
					setMemberSlot.erase(owner);
					setBuilding->phases.erase(old);
				}
				setGeometry[a_slot] = nullptr;
			}
			if (a_phases)
				if (const auto* geometry = tables.objectGeometry[a_slot]) {
					setMemberSlot.insert_or_assign(geometry, a_slot);
					setBuilding->phases.insert_or_assign(geometry, a_phases);
					setGeometry[a_slot] = geometry;
				}
			setSnapshotDirty = true;
		};
		for (std::size_t q = 0; q < setQueue.size(); ++q) {
			const std::uint32_t slot = setQueue[q];
			if (slot >= objects)
				continue;
			// A base and its layer are main-phase members together: the engine's main passes of both are its geometry's. Either one
			// waiting is enough for a readiness event to evaluate the two again.
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
				// Members together, whole (U5).
				if (!(phases & kSetMain) != !(partnerPhases & kSetMain)) {
					++setStats.waitingBy[6];
					// The waiting one is taken again with the other (whose verdict may change with any source).
					setWaitCause[slot] |= kWaitOther;
					setWaitCause[partner] |= kWaitOther;
					phases = 0;
					partnerPhases = 0;
				}
				apply(partner, partnerPhases, partnerWait);
			}
			apply(slot, phases, wait);
		}
		// (Before the queue's marks are cleared: a slot this commit took again while its binding is (setRebinding) is decided as it was.)
		// CS_DCLF_SET_PARITY, every 60th commit: every slot's phases and waiting decided again, against what the commit's queue (its
		// events and requeues by cause) left: a slot the requeues missed differs.
		if (SwitchEnabled(Switch::SetParity) && ParityDue(sceneFrame, 23)) {
			parityPass = true;
			std::uint32_t differ = 0;
			std::string first;
			for (std::uint32_t slot = 0; slot < objects; ++slot) {
				bool wait = false;
				std::uint8_t phases = wanted(slot, wait);
				const std::uint32_t partner = tables.IsLayer(slot) ? tables.layerBase[slot] : slot < tables.layerOf.size() ? tables.layerOf[slot] : kNoObjectSlot;
				if (partner != kNoObjectSlot && partner < objects) {
					bool partnerWait = false;
					if (!(phases & kSetMain) != !(wanted(partner, partnerWait) & kSetMain))
						phases = 0;
				}
				if (phases == setPhases[slot] && wait == (setWaitingMark[slot] != 0))
					continue;
				if (!differ++) {
					const auto* geometry = tables.objectGeometry[slot];
					first = fmt::format("'{}' (slot {}): phases {:#x} kept, {:#x} now; waiting {} kept, {} now (its causes {:#x})", geometry && geometry->name.c_str() ? geometry->name.c_str() : "?",
						slot, setPhases[slot], phases, setWaitingMark[slot] != 0, wait, setWaitCause[slot]);
				}
			}
			parityPass = false;
			++setStats.commitParityChecks;
			setStats.commitParityDiffer += differ;
			static std::uint32_t logged = 0;
			if (differ && logged++ < 8)
				logger::warn("[DCLF] set commit parity at frame {}: {} slots differ from a full evaluation; first {} <- COMMIT", sceneFrame, differ, first);
		}
		setStats.evaluated += setQueue.size();
		for (const std::uint32_t slot : setQueue)
			if (slot < objects) {
				setQueueMark[slot] = 0;
				setRebinding[slot] = 0;
			}
		setQueue.clear();
		std::erase_if(setWaiting, [&](std::uint32_t a_slot) { return a_slot >= setWaitingMark.size() || !setWaitingMark[a_slot]; });
		// Dropped once: a slot can be pushed again after it left and came back within one commit.
		std::sort(setWaiting.begin(), setWaiting.end());
		setWaiting.erase(std::unique(setWaiting.begin(), setWaiting.end()), setWaiting.end());
		setStats.members += setMemberSlot.size();
		setStats.partial += setPartialCount;
		setStats.waiting += setWaiting.size();

		RefreshSetSnapshot();
		// Where the log stood at this commit: the applied set's revocation reads from here (ApplySet).
		setCommitCursor.Restart(tablesGeneration);
		setCommitCursor.Advance(tables.changeLog);
	}

	void SceneStore::ApplySet()
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
		// The structural changes since the last publication's revocation, read before this application's own notes: the claims
		// applied now were committed without them, and the revocation takes them back.
		NoteStructureChanges();
		// The geometries whose main claim this changes against the last publication's, for the stand-in's admission and walks
		// (PrimaryCull::NoteSetChanges, at the frame's start that installs the publication): whatever commits lie between.
		auto& joined = publicationJoined;
		auto& left = publicationLeft;
		for (const auto& [slot, geometry] : setApply) {
			if (slot < setApplyMark.size())
				setApplyMark[slot] = 0;
			if (slot >= objects)
				continue;
			// The record the commit decided for: a slot freed or holding another geometry since takes nothing.
			const bool same = !(tables.objects[slot].flags & kObjectFree) && tables.objectGeometry[slot] == geometry;
			const std::uint8_t phases = same ? setPhasesNext[slot] : std::uint8_t{ 0 };
			const std::uint8_t lacking = same ? setLackingNext[slot] : std::uint8_t{ 0 };
			// The claim as the engine's hooks read it, by its base geometry (a layer is claimed with its base).
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
			// The record's bit is the main phase's (what the main builds' GPU inputs carry); the shadow builds read the phases. The
			// coordinator's tables alone (step 6e E3): the frame reads them as the publication made from them.
			applied[slot] = phases;
			if (!(tables.objects[slot].flags & kObjectFree)) {
				auto& object = tables.objects[slot];
				object.flags = (phases & kSetMain) ? (object.flags | kObjectMember) : (object.flags & ~kObjectMember);
				tables.NoteChange(slot, kChangeBindings);
			}
		}
		setApply.clear();
		// The commit's snapshot, with what left the set since it (LeaveSet).
		RefreshSetSnapshot();
		publicationClaims = setSnapshot;
		// What changed since the applied commit (the rest of its walk, its accumulate phase's drops) is what RevokeUndrawnClaims
		// checks next, before the publication.
		if (setCommitCursor.Continues(tables.changeLog, tablesGeneration)) {
			revokeCursor = setCommitCursor;
		} else {
			revokeCursor.Restart(tablesGeneration);
			revokeCursor.Advance(tables.changeLog);
		}
		publicationCommitFrame = setCommitFrame;
		publicationCommitToggles = setCommitToggles;
	}

	void SceneStore::WithdrawSet()
	{
		ZoneScopedN("CS.DCLF.Scene.WithdrawSet");
		if (setWithdrawn)
			return;
		setWithdrawn = true;
		// Every claim the engine's hooks were told of taken back (step 6e E3: the frame's alone; the tables and the publications keep
		// their set, and the next installation applies its claims whole).
		std::vector<const RE::BSGeometry*> left;
		if (notedClaims)
			for (const auto& [geometry, phases] : notedClaims->phases)
				if (phases & kSetMain)
					left.push_back(geometry);
		notedClaims.reset();
		installNotes.clear();
		auto none = std::make_shared<SetSnapshot>();
		none->frame = frame;
		PassCapture::Get().PublishSet(std::move(none));
		SunAccumulation::Get().PublishExclusion(nullptr);
		if (!left.empty())
			PrimaryCull::Get().NoteSetChanges({}, left);
	}

	void SceneStore::NoteStructureChanges()
	{
		if (!IndirectDraws::Get().RevisionClaims() || !revokeCursor.Continues(tables.changeLog, tablesGeneration))
			return;
		for (const auto& change : revokeCursor.Unread(tables.changeLog))
			if (change.causes & kStructureCauses)
				structureChanged.push_back(change.slot);
		revokeCursor.Advance(tables.changeLog);
	}

	void SceneStore::RevokeUndrawnClaims()
	{
		ZoneScopedN("CS.DCLF.Scene.RevokeUndrawnClaims");
		std::vector<std::pair<const RE::BSGeometry*, std::uint8_t>> revoked;
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
				// Within the scene buffers the builds ahead are made against too (FitsScene): rows the record took after its commit (an
				// extras row, a fade root) may lie past them until their growth, and a build gives such an object no input.
				auto& draws = IndirectDraws::Get();
				const bool main = (flags & kObjectMember) && !(flags & kObjectNoBindings) && draws.FitsScene(&tables, a_slot, false);
				const std::uint8_t others = draws.FitsScene(&tables, a_slot, true) ? static_cast<std::uint8_t>(tables.setPhases[a_slot] & ~(kSetMain | kSetReflection)) : 0;
				drawn = static_cast<std::uint8_t>((main ? (kSetMain | kSetReflection) : 0u) | others);
			}
			// A structural change since the last revocation (R3b): the shapes were made without it, so the claim goes whole; and a claim
			// is the whole object (U5), so one not drawn in any of its phases goes whole too.
			const std::uint8_t lost = ((same && a_structure) || (claimed & ~drawn)) ? claimed : std::uint8_t{ 0 };
			if (!lost)
				return;
			if (same && a_structure && !(claimed & ~drawn))
				++revokedStructureGeometries;
			setPhasesApplied[a_slot] &= ~lost;
			if (same) {
				// Out of the set in what it no longer draws: the accumulate phase must not make it a member again, and the next
				// publication's claims (the commit's snapshot) must not either.
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
			const bool stamps = IndirectDraws::Get().RevisionClaims();
			for (const auto& change : revokeCursor.Unread(tables.changeLog)) {
				check(change.slot, false);
				if (stamps && (change.causes & kStructureCauses))
					structureChanged.push_back(change.slot);
			}
			// Those structural changes, with every one read since the last revocation (ApplySet's, before its own notes).
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
		if (revoked.empty())
			return;
		// The publication's claims without them (CommitSet's snapshot stays the commits': the revocation's notes queue the slots).
		if (publicationClaims) {
			auto snapshot = std::make_shared<SetSnapshot>(*publicationClaims);
			for (const auto& [geometry, lost] : revoked) {
				const auto it = snapshot->phases.find(geometry);
				if (it == snapshot->phases.end())
					continue;
				it->second &= ~lost;
				if (!it->second)
					snapshot->phases.erase(it);
			}
			publicationClaims = std::move(snapshot);
		}
		for (const auto& [geometry, lost] : revoked) {
			++revokedGeometries;
			if (lost & kSetMain) {
				++revokedMain;
				publicationLeft.push_back(geometry);
			}
		}
	}

	void SceneStore::PublishScene()
	{
		ZoneScopedN("CS.DCLF.Scene.PublishScene");
		// The set the commits decided, into the tables, and the claims they cannot draw taken back: what the publication's frames
		// draw is what it claims (step 6e E3).
		ApplySet();
		RevokeUndrawnClaims();
		// The lookups for the tables published now (T6b2c step 5): resolved against the pipeline lane's newest catalog, then copied
		// (or the last copy again), so the publication carries the entries parallel to its tables and the catalog they are of.
		ResolveLookups();
		// What the passes since the last publication made for the frame (T6b3a), out of the coordinator's state before the copy (the
		// actors' wetness changes ride in the tables).
		PublicationDeltas deltas;
		TakeDeltas(deltas);
		PublishTables();
		PublishLookups();
		auto publication = std::make_shared<ScenePublication>();
		publication->sequence = ++publicationSequence;
		publication->commitFrame = publicationCommitFrame;
		publication->togglesGeneration = publicationCommitToggles;
		publication->tablesGeneration = tablesGeneration;
		publication->tables = publishedTables;
		publication->claims = publicationClaims ? publicationClaims : std::make_shared<const SetSnapshot>();
		publication->lackingCount = setLackingCount;
		publication->lookups = publishedLookups;
		publication->catalog = publishedCatalog;
		// The candidates as this pass left them (T6b3a: the frame takes the newest publication's, never the coordinator's).
		publication->sunCandidates = sunCandidates;
		publication->sunGeneration = sunCandidatesGeneration;
		publication->lightCandidates = lightCandidates;
		publication->lightGeneration = lightCandidatesGeneration;
		publication->lightEntriesAppeared = lightEntriesAppeared;
		// The category nodes the point lights' culls ask of the frame, copied again only when the set changed (the light entries are
		// the log's changes: TakeDeltas).
		if (std::exchange(nodeSetsDirty, false) || !publishedCategoryNodes)
			publishedCategoryNodes = std::make_shared<const ankerl::unordered_dense::set<RE::NiNode*>>(categoryNodes);
		publication->categoryNodes = publishedCategoryNodes;
		// The scene revision's request (MakeRevisionShapes at the frame's start): these tables and lookups, with their counts.
		auto& revision = publication->revision;
		revision.publication = publication->sequence;
		revision.commitFrame = publication->commitFrame;
		revision.passSerial = passSerial;
		revision.tablesGeneration = tablesGeneration;
		revision.tables = publishedTables;
		revision.lookups = publishedLookups;
		revision.catalog = publishedCatalog;
		revision.pipelines = static_cast<std::uint32_t>(tables.pipelines.size());
		revision.shadowSlotKeys = lookups.shadowSlotKeys.size();
		revision.shadowKeys = tables.shadowKeysUsed.size();
		for (const auto& used : tables.occlusionKeysUsed)
			revision.shadowKeys += used.size();
		// The deltas first: whoever adopts the publication finds its node on the log.
		deltas.publication = publication->sequence;
		deltas.commitFrame = publication->commitFrame;
		publicationLog.Append(std::move(deltas));
		// To the snapshot builder (T6b3b): its draws built ahead (step 6e E3b) and its revision made and recorded off the render thread,
		// posted to the frame whole; a publication it skips (a newer one arrived first) loses nothing of the log.
		IndirectDraws::Get().PostSnapshotWork(std::move(publication));
	}

	void SceneStore::TakeDeltas(PublicationDeltas& a_deltas)
	{
		a_deltas.joined = std::exchange(publicationJoined, {});
		a_deltas.left = std::exchange(publicationLeft, {});
		if (placementPlanReady)
			a_deltas.plans.push_back(std::exchange(placementPlanReady, nullptr));
		a_deltas.shading = std::exchange(shadingNamed, {});
		a_deltas.fadeSeeds = std::exchange(fadeSeedRequests, {});
		a_deltas.treeSeeds = std::exchange(treeSeedRequests, {});
		// No cap (T6b3a): every switch applied reaches PrimaryCull, which reads them all only when the walk asks (a full walk).
		a_deltas.switches = std::exchange(switchesApplied, {});
		a_deltas.switchResync = std::exchange(switchResync, false);
		a_deltas.retiredImports = std::exchange(retiredImports, {});
		a_deltas.actorWetness = std::exchange(tables.actorWetnessChanges, {});
		a_deltas.lightEntries = std::exchange(lightEntryChanges, {});
	}

	bool SceneStore::DeltasPending() const
	{
		return !publicationJoined.empty() || !publicationLeft.empty() || placementPlanReady || !shadingNamed.empty() || !fadeSeedRequests.empty() ||
		       !treeSeedRequests.empty() || !switchesApplied.empty() || switchResync || !retiredImports.empty() || !tables.actorWetnessChanges.empty() ||
		       !lightEntryChanges.empty();
	}

	void SceneStore::ReturnPublication(std::shared_ptr<const ScenePublication>&& a_publication)
	{
		// The coordinator drops it at its next pass (T6b3a: no retirement on the render thread). Its tables stay in the pool, which the
		// coordinator alone writes; its lookups', catalog's and draws' owners are cleanup-queue owners.
		if (a_publication)
			retiredPublications.Push(std::move(a_publication));
	}

	void SceneStore::DropReturnedPublications()
	{
		retiredPublications.Drain([](std::shared_ptr<const ScenePublication>&& a_publication) { const auto dropped = std::move(a_publication); });
	}

	void SceneStore::InstallClaims()
	{
		if (!installed)
			return;
		const bool whole = std::exchange(setWithdrawn, false);
		if (!whole && !installPending)
			return;
		installPending = false;
		auto& primary = PrimaryCull::Get();
		PassCapture::Get().PublishSet(installed->claims);
		frameLackingCount = installed->lackingCount;
		if (whole) {
			// After a withdrawal the engine's hooks hold no claim: every main claim joins.
			std::vector<const RE::BSGeometry*> joined;
			for (const auto& [geometry, phases] : installed->claims->phases)
				if (phases & kSetMain)
					joined.push_back(geometry);
			if (!joined.empty())
				primary.NoteSetChanges(joined, {});
		} else {
			for (const auto& [joined, left] : installNotes)
				if (!joined.empty() || !left.empty())
					primary.NoteSetChanges(joined, left);
		}
		installNotes.clear();
		notedClaims = installed->claims;
	}

	void SceneStore::BeginFrame()
	{
		// Switch events are taken on the render thread only (PushSwitch); ApplyEvents runs on the coordinator.
		switchEventThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		++frame;
		// The frame's engine globals (step 6e F2), before anything of the frame reads them; the scene work's passes take them with the
		// frame inputs (T6b3a).
		const auto captureStart = std::chrono::steady_clock::now();
		// The Lighting shader first (once): the globals' material sources read it (T6b2c step 8: no longer learned at the capture drain).
		CaptureLightingShader();
		frameGlobals = FrameGlobals::Capture();
		SceneCapture::SetMainThread(::GetCurrentThreadId());
		ImportTimings::NoteMainThread(::GetCurrentThreadId());
		SceneCapture::SetFrame(frame);
		captureNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - captureStart).count());
		++captureFrames;
		// What the coordinator's passes take of the frame (T6b3a): posted after the capture, before the kick.
		PostFrameInputs();
	}

	void SceneStore::PostFrameInputs()
	{
		auto inputs = std::make_unique<FrameInputs>();
		inputs->globals = frameGlobals;
		inputs->frame = frame;
		// The membership witness, sampled here (PrimaryCull reads the same sample this frame): the commit's and the binds'.
		inputs->membershipWitness = PrimaryCull::Get().SampleMembershipWitness();
		inputs->togglesGeneration = Toggles::Get().Generation();
		inputs->verdictsGeneration = verdictsRequested;
		frameInputsSlot.Post(std::move(inputs));
	}

	void SceneStore::TakeFrameInputs()
	{
		// Latest wins: a pass that finds none keeps the last ones it took (the accumulate pass, after its frame's scene pass).
		if (!frameInputsSlot.TakeInto(sceneInputs))
			return;
		sceneFrame = sceneInputs.frame;
		// A toggle that enters the classification, or the feature back on: every cached verdict dropped before the pass reads one.
		if (sceneInputs.verdictsGeneration != verdictsApplied) {
			verdictsApplied = sceneInputs.verdictsGeneration;
			InvalidateVerdicts();
		}
	}

	void SceneStore::TakeCoordinatorInputs()
	{
		// In the order the frame's start applied them before (T6b3a): the fade roots the depth commit holds, PrimaryCull's fade
		// ownership, its reseed of every owned root.
		if (const std::uint64_t held = fadeRootsSentPosted.exchange(kNoFadeRootsSent, std::memory_order_acq_rel); held != kNoFadeRootsSent)
			tables.fadeRootsJournal.BeginBuild(held);
		if (const auto owned = fadeOwnedPosted.Take())
			ApplyFadeRootsOwned(*owned);
		if (fadeReseedPosted.exchange(false, std::memory_order_acq_rel))
			ApplyReseedOwnedFadeRoots();
	}

	void SceneStore::RunSceneWork(bool a_task)
	{
		// The frame's events, which the render thread ingested and posted before the kick (BeginSceneFrame): what they drop of
		// PrimaryCull's is held (primaryNotes), and the references they let go of are released at Present.
		inSceneTask = a_task;
		holdPrimaryNotes = true;
		const auto workStart = std::chrono::steady_clock::now();
		// T6b3a: the frame's inputs (the globals, the frame number, the membership witness, the toggles; the verdicts dropped when asked),
		// and the leases the render thread gave back, dropped here.
		TakeFrameInputs();
		IndirectDraws::Get().TakeAheadContext();
		++passSerial;
		DropReturnedPublications();
		// The coordinator's tables moved since its last publication (events Present applied, a frame whose accumulate work did not run),
		// or a pass made something for the frame since: published at this pass's start, after what the render thread posted for it
		// (as the frame's start published it before, with the coordinator idle).
		const bool republish = !publishedTables || publishedTables->changeLog.End() != tables.changeLog.End() ||
		                       publishedTables->versionCounter != tables.versionCounter || publishedTables->objects.size() != tables.objects.size() ||
		                       publishedTables->pipelines.size() != tables.pipelines.size() || publishedTables->materials.size() != tables.materials.size() ||
		                       !setApply.empty() || DeltasPending();
		TakeCoordinatorInputs();
		if (republish) {
			++tablesPublication.republished;
			PublishScene();
		}
		// What no publication names any more, back on the free lists before anything allocates.
		RecycleRetired();
		ApplyEvents();
		ApplyConstantsPosts();
		// The technique rows to this frame's sample (T6b2c: the coordinator's, from its FrameGlobals).
		RefreshTechniqueRows();
		// The material records (T6b2c step 7): the captured materials' slots evaluated again, the frame components and the texture
		// transforms to this frame's sample, all from the captures ApplyEvents drained and the frame's FrameGlobals.
		RefreshMaterialRecords();
		// The material bindings (T6b2c): the answers since the last pass, and the records the rewrites changed.
		UpdateMaterialBindings();
		// The shared, projected, mask and shadow texture bindings (T6b2c): the answers, the technique rows' masks, the last walk's textures.
		UpdateSharedBindings();
		BuildFrame(Phase::Scene);
		// The lookups the commit judges readiness by (T6b2c step 5): the walk's pipelines and shadow keys resolved against the pipeline
		// lane's newest catalog, the material entries written since versioned.
		ResolveLookups();
		CommitSet();
		EndSceneFrame();
		// The mirror's parity (step 6e F3), with the frame's events all drained.
		CheckMirror();
		[[maybe_unused]] const double workMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - workStart).count();
		TracyPlot("CS.DCLF.SceneWorkMs", workMs);
		holdPrimaryNotes = false;
		inSceneTask = false;
	}

	void SceneStore::ResolveLookups()
	{
		ZoneScopedN("CS.DCLF.Scene.ResolveLookups");
		// The bindings are written straight in where the scene work makes them (UpdateMaterialBindings, UpdateSharedBindings, the
		// retirements); the material entries changed since take their versions here.
		VersionMaterialBindings();
		// The pipeline lane's newest catalog (latest wins: none newer keeps the last), and against it the pipeline entries and the shadow
		// pipelines of the tables as they stand (IndirectDraws::ResolveLookups: pure, the catalog and the tables alone, but the requests
		// for keys the lane has not been asked for, which any thread may make).
		if (auto catalog = DrawPipelines::Get().TakeCatalog())
			lookupsCatalog = std::move(catalog);
		IndirectDraws::Get().ResolveLookups(&tables, lookupsCatalog.get(), lookups, lookupsResolve);
	}

	void SceneStore::PublishLookups()
	{
		ZoneScopedN("CS.DCLF.Scene.PublishLookups");
		auto& publication = tablesPublication;
		// The catalog is always the newest the lookups were resolved against: a newer one of the same generations holds every index an
		// older one did (a new generation moves the lookups' generation), so it serves unchanged lookups as well.
		publishedCatalog = lookupsCatalog;
		// Unchanged since the last copy (no write, no counter moved): that copy again, shared, at no cost. Else a new copy, which shares
		// every chunk not written since (SharedChunks) and copies the rest whole.
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
		// The chunks written again since the last copy (an instance made new starts its count again).
		const std::uint64_t copies = lookups.pipelines.Copies() + lookups.materials.Copies();
		publication.lookupsChunks += copies >= lookupsChunkCopies ? copies - lookupsChunkCopies : copies;
		lookupsChunkCopies = copies;
	}

	void SceneStore::ResetLookups()
	{
		// What a build or a kept binding holds of the last instance is stale: the generations move past it, the versions continue (unique
		// across instances), the log and the instance are new (a reader of the last one starts again). The publications still holding the
		// last instance keep it whole until they are released.
		const auto lookupGeneration = lookups.generation;
		const auto shadowGeneration = lookups.shadowGeneration;
		const auto lookupVersion = lookups.versionCounter;
		lookups = Lookups{};
		lookups.generation = lookupGeneration + 1;
		lookups.shadowGeneration = shadowGeneration + 1;
		lookups.versionCounter = lookupVersion;
		lookupsResolve = {};
	}

	void SceneStore::HandOverAtFrameStart(std::shared_ptr<const void> a_adopted)
	{
		ZoneScopedN("CS.DCLF.Scene.HandOverAtFrameStart");
		// T6b3a: nothing of the coordinator's is read here. The notes the scene work held for PrimaryCull are delivered (the join delivers
		// them first while it stays).
		DeliverPrimaryNotes();
		// T6b3b: the publication of the snapshot the frame adopted (IndirectDraws::AdoptSnapshot), installed when newer than the installed
		// one; a snapshot made again for the installed one (new recordings, the frame's inputs moved) brings none.
		auto adopted = std::static_pointer_cast<const ScenePublication>(std::move(a_adopted));
		const bool fresh = adopted && (!installed || adopted->sequence > installed->sequence);
		// The frame inputs of every publication up to the installed one, in order: the log holds one the builder skipped too.
		using Log = PublicationLog<PublicationDeltas>;
		Log::Node* at = deltasCursor ? deltasCursor : publicationLog.First();
		const std::uint64_t upTo = fresh ? adopted->sequence : (installed ? installed->sequence : 0);
		auto append = [](auto& a_to, auto& a_from) {
			a_to.insert(a_to.end(), std::make_move_iterator(a_from.begin()), std::make_move_iterator(a_from.end()));
			a_from.clear();
		};
		for (Log::Node* next = Log::Next(at); next && next->value.publication <= upTo; next = Log::Next(at)) {
			at = next;
			auto& deltas = at->value;
			append(framePlans, deltas.plans);
			append(frameShading, deltas.shading);
			append(frameFadeSeeds, deltas.fadeSeeds);
			append(frameTreeSeeds, deltas.treeSeeds);
			append(frameSwitchChanges, deltas.switches);
			frameSwitchResync = frameSwitchResync || std::exchange(deltas.switchResync, false);
			append(frameRetiredImports, deltas.retiredImports);
			for (const auto& change : deltas.actorWetness) {
				if (change.clear)
					frameActorWetness.Clear();
				else
					frameActorWetness.Set(change.slot, change.group, change.identity, change.resetValue);
			}
			deltas.actorWetness.clear();
			for (const auto& change : deltas.lightEntries) {
				if (change.kind > 0)
					frameLightEntrySet.insert(change.node);
				else if (change.kind < 0)
					frameLightEntrySet.erase(change.node);
				else
					frameLightEntrySet.clear();
			}
			deltas.lightEntries.clear();
		}
		deltasCursor = at;
		frameSwitchResync = frameSwitchResync || std::exchange(switchResyncNext, false);
		if (fresh) {
			// The claims' changes of every publication up to it, in order (never past the frame inputs' cursor).
			const std::uint64_t sequence = adopted->sequence;
			Log::Node* note = notesCursor ? notesCursor : publicationLog.First();
			Log::Node* const bound = deltasCursor ? deltasCursor : publicationLog.First();
			while (note != bound) {
				Log::Node* next = Log::Next(note);
				if (!next || next->value.publication > sequence)
					break;
				note = next;
				auto& deltas = note->value;
				// T6b0: a commit's time to its installation, by the geometries it joined.
				publicationStats.installDelay[AgeBucket(frame - deltas.commitFrame)] += deltas.joined.size();
				installNotes.emplace_back(std::move(deltas.joined), std::move(deltas.left));
			}
			notesCursor = note;
			publicationLog.Release(note);
			TracyPlot("CS.DCLF.InstallDelayFrames", static_cast<std::int64_t>(frame - adopted->commitFrame));
			// The publications the builder skipped past, and the replaced one back to the coordinator, which drops it.
			publicationStats.skipped += installed ? sequence - installed->sequence - 1 : 0;
			ReturnPublication(std::move(installed));
			installed = std::move(adopted);
			installPending = true;
			++publicationStats.installed;
		} else {
			++publicationStats.kept;
		}
		// The frame's tables, lookups and catalog are one publication's (T6b2c step 5): the installed one's, none before the first (empty
		// tables and lookups). The indices its epochs resolve are of the set versions it binds (FrameIndirectState, FrameShadowIndirectState
		// read the held catalog), and its entries are parallel to its tables.
		acceptedTables = installed ? installed->tables : nullptr;
		frameLookups = installed ? installed->lookups : nullptr;
		frameCatalog = installed ? installed->catalog : nullptr;
		frameTablesGeneration = installed ? installed->tablesGeneration : 0u;
		DrawPipelines::Get().HoldCatalog(frameCatalog);
		// The candidates the installed publication carries (the coordinator's as its pass left them).
		if (installed) {
			frameSunCandidates = installed->sunCandidates;
			frameSunGeneration = installed->sunGeneration;
			frameLightCandidates = installed->lightCandidates;
			frameLightGeneration = installed->lightGeneration;
			frameLightEntriesAppeared = installed->lightEntriesAppeared;
			frameCategoryNodes = installed->categoryNodes;
		}
		publishedSunGeneration = frameSunGeneration;
	}

	std::vector<SceneStore::ShadingItem> SceneStore::PeekShadingItems() const
	{
		// Those the log holds past the frame's cursor (published, not taken), then those named since the last publication (the parities'
		// inline scene work: the coordinator is idle).
		std::vector<ShadingItem> items;
		using Log = PublicationLog<PublicationDeltas>;
		for (const Log::Node* next = Log::Next(deltasCursor ? deltasCursor : publicationLog.First()); next; next = Log::Next(const_cast<Log::Node*>(next)))
			items.insert(items.end(), next->value.shading.begin(), next->value.shading.end());
		items.insert(items.end(), shadingNamed.begin(), shadingNamed.end());
		return items;
	}

	void SceneStore::DeliverPrimaryNotes()
	{
		auto& primary = PrimaryCull::Get();
		primaryNotes.Drain([&primary](PrimaryNote&& a_note) {
			switch (a_note.kind) {
			case PrimaryNote::Kind::HiddenKey:
				primary.NoteHiddenKey(a_note.key);
				break;
			case PrimaryNote::Kind::MemberLost:
				primary.NoteMemberLost(static_cast<const RE::BSGeometry*>(a_note.key));
				break;
			case PrimaryNote::Kind::AllMembersLost:
				primary.NoteAllMembersLost();
				break;
			}
		});
	}

	void SceneStore::RetireIntoChain()
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

	void SceneStore::RecycleRetired()
	{
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
					tables.boneFree[std::min<std::size_t>(retired.extra / 3, tables.boneFree.size() - 1)].push_back(slot);
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
			// The imports to the epochs (the publication log: TakeRetiredImports, held until their executions retire); the engine's
			// references released at Present on the render thread, pushed now (T6b3a: no hand-over at a join).
			for (auto& owner : a_batch.imports)
				retiredImports.push_back(std::move(owner));
			for (auto& material : a_batch.materials)
				materialsReleased.Push(std::move(material));
			for (auto& reference : a_batch.references)
				EngineReleases::Push(std::move(reference));
			for (auto& events : a_batch.events)
				batchesReleased.Push(std::move(events));
		});
	}

	void SceneStore::PublishTables()
	{
		ZoneScopedN("CS.DCLF.Scene.PublishTables");
		const auto start = std::chrono::steady_clock::now();
		auto& publication = tablesPublication;
		// What was freed since the last publication goes into the chain's open node, which the publication made now closes.
		RetireIntoChain();
		// A snapshot no frame holds names nothing any more: its node let go (the node of the one published last stays: a frame may
		// still accept it).
		for (const auto& pooled : tablesPool)
			if (pooled.use_count() == 1 && pooled != publishedTables)
				pooled->retirementHold.reset();
		// A snapshot only the pool holds (no frame accepted it, or every holder let go): written again, reusing its storage.
		std::size_t index = tablesPool.size();
		for (std::size_t i = 0; i < tablesPool.size(); ++i)
			if (tablesPool[i].use_count() == 1 && tablesPool[i] != publishedTables) {
				index = i;
				break;
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

		// Step 6d: the snapshot equals the tables as they stood at its change log's end (its last publication, and the frame's start's
		// writes if a frame accepted it: WriteBoth writes both alike). What changed since is written again:
		// - the per-object columns the change log covers (ColumnsOf, CausesBetween), by the slots it names since that end;
		// - the logs themselves, by appending what they gained (they only append, trim their head and invalidate);
		// - the material records by their versions (session-unique per write: an equal version is an equal record).
		// Everything else is copied whole, with those held out of the copy. A snapshot the log no longer reaches (trimmed past,
		// invalidated: the tables were cleared) or just made is copied whole.
		auto loggedColumns = [](Tables& a_left, Tables& a_right, auto&& a_column) {
			a_column(a_left.objects, a_right.objects);
			a_column(a_left.draws, a_right.draws);
			a_column(a_left.lights, a_right.lights);
			a_column(a_left.treeAnim, a_right.treeAnim);
			a_column(a_left.sunEntryNode, a_right.sunEntryNode);
			a_column(a_left.hasFadeNode, a_right.hasFadeNode);
			a_column(a_left.extraOffset, a_right.extraOffset);
			a_column(a_left.objectGeometry, a_right.objectGeometry);
			a_column(a_left.objectIdentity, a_right.objectIdentity);
			a_column(a_left.objectGroup, a_right.objectGroup);
			a_column(a_left.shadowDiffuse, a_right.shadowDiffuse);
			a_column(a_left.shadowMaterial, a_right.shadowMaterial);
			a_column(a_left.fadeDistance, a_right.fadeDistance);
			a_column(a_left.boneOffset, a_right.boneOffset);
			a_column(a_left.boneRows, a_right.boneRows);
			a_column(a_left.shadowTechnique, a_right.shadowTechnique);
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				a_column(a_left.occlusionTechnique[v], a_right.occlusionTechnique[v]);
			a_column(a_left.faceStream, a_right.faceStream);
			a_column(a_left.sceneFlags, a_right.sceneFlags);
			a_column(a_left.skinPartitions, a_right.skinPartitions);
			a_column(a_left.skinLodPartitions, a_right.skinLodPartitions);
			a_column(a_left.shadowReject, a_right.shadowReject);
			a_column(a_left.residentSlot, a_right.residentSlot);
		};
		// The geometry columns the geometry log covers (geometryLastUsed, the walk's, is copied whole).
		auto geometryColumns = [](Tables& a_left, Tables& a_right, auto&& a_column) {
			a_column(a_left.geometries, a_right.geometries);
			a_column(a_left.geometryImports, a_right.geometryImports);
			a_column(a_left.geometrySlotKey, a_right.geometrySlotKey);
			a_column(a_left.geometryLayerKey, a_right.geometryLayerKey);
		};
		// The families copied only when their write stamp moved.
		auto treeFamily = [](Tables& a_left, Tables& a_right, auto&& a_member) {
			a_member(a_left.trees, a_right.trees);
			a_member(a_left.treeRefs, a_right.treeRefs);
			a_member(a_left.treeFree, a_right.treeFree);
			a_member(a_left.treeNode, a_right.treeNode);
			a_member(a_left.treeIndex, a_right.treeIndex);
			a_member(a_left.treeObjects, a_right.treeObjects);
			a_member(a_left.objectTree, a_right.objectTree);
		};
		auto fadeRootFamily = [](Tables& a_left, Tables& a_right, auto&& a_member) {
			a_member(a_left.fadeRoots, a_right.fadeRoots);
			a_member(a_left.fadeRootRefs, a_right.fadeRootRefs);
			a_member(a_left.fadeRootFree, a_right.fadeRootFree);
			a_member(a_left.fadeRootNode, a_right.fadeRootNode);
			a_member(a_left.fadeRootIndex, a_right.fadeRootIndex);
			a_member(a_left.fadeRootSwitch, a_right.fadeRootSwitch);
			a_member(a_left.objectFadeRoot, a_right.objectFadeRoot);
		};
		auto constantsFamily = [](Tables& a_left, Tables& a_right, auto&& a_member) {
			a_member(a_left.pipelineConstants, a_right.pipelineConstants);
			a_member(a_left.techniqueConstants, a_right.techniqueConstants);
		};
		auto heldOut = [&](Tables& a_left, Tables& a_right) {
			loggedColumns(a_left, a_right, [](auto& a_x, auto& a_y) { a_x.swap(a_y); });
			constantsFamily(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			treeFamily(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			fadeRootFamily(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			geometryColumns(a_left, a_right, [](auto& a_x, auto& a_y) { a_x.swap(a_y); });
			a_left.extraRows.swap(a_right.extraRows);
			a_left.materials.swap(a_right.materials);
			std::swap(a_left.changeLog, a_right.changeLog);
			std::swap(a_left.geometryLog, a_right.geometryLog);
			std::swap(a_left.materialLog, a_right.materialLog);
			std::swap(a_left.extrasBlockLog, a_right.extrasBlockLog);
		};
		const std::uint64_t position = snapshot.changeLog.End();
		const std::uint64_t geometryPosition = snapshot.geometryLog.End();
		const std::uint64_t extrasPosition = snapshot.extrasBlockLog.End();
		const bool replayExtras = tablesPoolReplayable[index] && tables.extrasBlockLog.Readable(extrasPosition) &&
		                          snapshot.extrasBlockLog.base <= tables.extrasBlockLog.base && snapshot.extraRows.size() <= tables.extraRows.size();
		const bool replayGeometry = tablesPoolReplayable[index] && tables.geometryLog.Readable(geometryPosition) &&
		                            snapshot.geometryLog.base <= tables.geometryLog.base && snapshot.geometries.size() <= tables.geometries.size();
		const bool replay = tablesPoolReplayable[index] && tables.changeLog.Readable(position) && snapshot.changeLog.base <= tables.changeLog.base &&
		                    snapshot.objects.size() <= tables.objects.size();
		// Read before the copy writes the snapshot's stamps.
		const bool keepTrees = tablesPoolReplayable[index] && snapshot.treesStamp == tables.treesStamp;
		const bool keepFadeRoots = tablesPoolReplayable[index] && snapshot.fadeRootsStamp == tables.fadeRootsStamp;
		const bool keepConstants = tablesPoolReplayable[index] && snapshot.constantsStamp == tables.constantsStamp;
		std::vector<std::uint32_t> recordVersions, recordFrameVersions;
		recordVersions.swap(snapshot.materialVersion);
		recordFrameVersions.swap(snapshot.materialFrameVersion);
		{
			// The snapshot's held-out members aside, the tables' aside (put back however the copy ends), the rest copied whole.
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
		publication.restMs += std::chrono::duration<double, std::milli>(restEnd - start).count();

		const std::size_t objectSlots = tables.objects.size();
		publication.objectSlots += objectSlots;
		if (replay) {
			snapshot.GrowObjects(objectSlots);
			replaySlotMarks.resize(std::max(replaySlotMarks.size(), objectSlots), 0);
			std::vector<std::uint32_t> copied;
			for (const auto& change : tables.changeLog.From(position)) {
				const std::uint32_t slot = change.slot;
				if (slot >= objectSlots || std::exchange(replaySlotMarks[slot], std::uint8_t{ 1 }))
					continue;
				copied.push_back(slot);
				loggedColumns(snapshot, tables, [slot](auto& a_to, auto& a_from) { a_to[slot] = a_from[slot]; });
			}
			for (const std::uint32_t slot : copied)
				replaySlotMarks[slot] = 0;
			publication.objectsReplayed += copied.size();
			// The logs: the head the tables trimmed dropped, what they gained appended.
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
			replayLog(snapshot.geometryLog, tables.geometryLog);
			replayLog(snapshot.materialLog, tables.materialLog);
		} else {
			++publication.wholeCopies;
			loggedColumns(snapshot, tables, [](auto& a_to, auto& a_from) { a_to = a_from; });
			snapshot.changeLog = tables.changeLog;
			snapshot.geometryLog = tables.geometryLog;
			snapshot.materialLog = tables.materialLog;
			recordVersions.clear();
			recordFrameVersions.clear();
		}
		// The extras rows by the blocks the block log names (written, allocated, freed); the log itself as the others.
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
		// The geometry columns by the geometry log (the logs above are replayed after their positions were taken).
		const std::size_t geometrySlots = tables.geometries.size();
		publication.geometrySlots += geometrySlots;
		if (replayGeometry) {
			snapshot.GeometryColumns([geometrySlots](auto& a_column, auto&&... a_initial) { a_column.resize(geometrySlots, a_initial...); });
			std::vector<std::uint8_t> marks(geometrySlots, 0);
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

		const std::size_t slots = tables.materials.size();
		snapshot.materials.resize(slots);
		for (std::size_t slot = 0; slot < slots; ++slot) {
			if (slot < recordVersions.size() && recordVersions[slot] == tables.materialVersion[slot] && slot < recordFrameVersions.size() &&
				recordFrameVersions[slot] == tables.materialFrameVersion[slot])
				continue;
			snapshot.materials[slot] = tables.materials[slot];
			++publication.materialsReplayed;
		}
		publication.materialSlots += slots;

		if (SwitchEnabled(Switch::PersistentParity) && publication.published % 60 == 0) {
			++publication.parityChecks;
			publication.parityMaterials += slots;
			for (std::size_t slot = 0; slot < slots; ++slot)
				if (!(snapshot.materials[slot] == tables.materials[slot]) && publication.parityDiffer++ == 0)
					logger::error("[DCLF] tables replay (6d): material slot {} differs from the tables (version {})", slot, tables.materialVersion[slot]);
			publication.parityObjects += objectSlots;
			if (snapshot.objects.size() != objectSlots) {
				if (publication.parityObjectsDiffer++ == 0)
					logger::error("[DCLF] tables replay (6d): {} object slots against the tables' {}", snapshot.objects.size(), objectSlots);
			} else {
				for (std::uint32_t slot = 0; slot < objectSlots; ++slot)
					if (const auto causes = Tables::CausesBetween(snapshot.ColumnsOf(slot), tables.ColumnsOf(slot)); causes && publication.parityObjectsDiffer++ == 0)
						logger::error("[DCLF] tables replay (6d): object slot {} differs from the tables (causes {:#x}; replayed from log {}, now {})", slot, causes,
							position, tables.changeLog.End());
			}
			publication.parityGeometries += geometrySlots;
			if (snapshot.geometries.size() != geometrySlots) {
				++publication.parityGeometriesDiffer;
			} else {
				for (std::uint32_t slot = 0; slot < geometrySlots; ++slot) {
					const auto& a = snapshot.geometryImports[slot];
					const auto& b = tables.geometryImports[slot];
					if ((std::memcmp(&snapshot.geometries[slot], &tables.geometries[slot], sizeof(GeometryRecord)) != 0 || a.vertexGeneration != b.vertexGeneration ||
							a.indexGeneration != b.indexGeneration || a.vertexOwner != b.vertexOwner || a.indexOwner != b.indexOwner ||
							snapshot.geometrySlotKey[slot] != tables.geometrySlotKey[slot] || snapshot.geometryLayerKey[slot] != tables.geometryLayerKey[slot]) &&
						publication.parityGeometriesDiffer++ == 0)
						logger::error("[DCLF] tables replay (6d): geometry slot {} differs from the tables (replayed from log {}, now {})", slot, geometryPosition,
							tables.geometryLog.End());
				}
			}
			// The kept families against the tables' (vectors by their bytes, maps by their entries, the journal by its version).
			auto sameMember = [](const auto& a_left, const auto& a_right) {
				using T = std::decay_t<decltype(a_left)>;
				if constexpr (requires { a_left.find(a_left.begin()->first); }) {
					if (a_left.size() != a_right.size())
						return false;
					for (const auto& [key, value] : a_left)
						if (const auto it = a_right.find(key); it == a_right.end() || !(it->second == value))
							return false;
					return true;
				} else {
					static_assert(std::is_trivially_copyable_v<typename T::value_type>);
					return a_left.size() == a_right.size() &&
					       (a_left.empty() || std::memcmp(a_left.data(), a_right.data(), a_left.size() * sizeof(a_left[0])) == 0);
				}
			};
			auto sameFamily = [&](auto&& a_family) {
				bool same = true;
				a_family(snapshot, tables, [&](auto& a_x, auto& a_y) { same = same && sameMember(a_x, a_y); });
				return same;
			};
			if (keepTrees && !sameFamily(treeFamily) && publication.parityFamiliesDiffer++ == 0)
				logger::error("[DCLF] tables replay (6d): the trees were kept (stamp {}) but differ from the tables'", tables.treesStamp);
			if (keepConstants) {
				bool same = snapshot.pipelineConstants.size() == tables.pipelineConstants.size() && snapshot.techniqueConstants.size() == tables.techniqueConstants.size();
				for (std::size_t p = 0; same && p < tables.pipelineConstants.size(); ++p)
					same = snapshot.pipelineConstants[p].version == tables.pipelineConstants[p].version && snapshot.pipelineConstants[p].valid == tables.pipelineConstants[p].valid;
				for (std::size_t r = 0; same && r < tables.techniqueConstants.size(); ++r)
					same = snapshot.techniqueConstants[r].constantsVersion == tables.techniqueConstants[r].constantsVersion &&
					       snapshot.techniqueConstants[r].bindingVersion == tables.techniqueConstants[r].bindingVersion && snapshot.techniqueConstants[r].valid == tables.techniqueConstants[r].valid;
				if (!same && publication.parityFamiliesDiffer++ == 0)
					logger::error("[DCLF] tables replay (6d): the constants were kept (stamp {}) but differ from the tables'", tables.constantsStamp);
			}
			if (keepFadeRoots && !sameFamily(fadeRootFamily) && publication.parityFamiliesDiffer++ == 0)
				logger::error("[DCLF] tables replay (6d): the fade roots were kept (stamp {}) but differ from the tables'", tables.fadeRootsStamp);
			auto sameLog = [](const auto& a_left, const auto& a_right) {
				return a_left.base == a_right.base && a_left.entries.size() == a_right.entries.size() &&
				       (a_left.entries.empty() || std::memcmp(a_left.entries.data(), a_right.entries.data(), a_left.entries.size() * sizeof(a_left.entries[0])) == 0);
			};
			if (!sameLog(snapshot.changeLog, tables.changeLog) || !sameLog(snapshot.geometryLog, tables.geometryLog) || !sameLog(snapshot.materialLog, tables.materialLog) ||
				!sameLog(snapshot.extrasBlockLog, tables.extrasBlockLog) || snapshot.extraRows.size() != tables.extraRows.size() ||
				(!tables.extraRows.empty() && std::memcmp(snapshot.extraRows.data(), tables.extraRows.data(), tables.extraRows.size() * sizeof(float)) != 0))
				if (publication.parityLogsDiffer++ == 0)
					logger::error("[DCLF] tables replay (6d): a log or the extras rows differ from the tables'");
		}
		// The node it holds collects what is freed after it (named by it and the publications before it).
		snapshot.retirementHold = retirement.Publish();
		publishedTables = target;
		++publication.published;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		publication.ms += ms;
		publication.maxMs = std::max(publication.maxMs, ms);
	}

	void SceneStore::KickSceneTask(std::function<void()> a_work, const char* a_name)
	{
		// The scene's lane (step 6c): the frame's builds never queue behind it. Joined at the next frame's start, or at Present when it
		// has ended by then (T6b1d: never waited for there).
		sceneTaskInFlight.store(true, std::memory_order_relaxed);
		sceneTask = std::static_pointer_cast<void>(std::make_shared<AsyncWorker::JobHandle>(
			AsyncWorker::Get().SubmitScene(a_name, [this, work = std::move(a_work)](std::stop_token) {
				sceneLaneThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
				sceneWorkThread = true;
				// T6b1d: no engine memory without a lease (EngineReadWindow::Touch counts the rest).
				const bool marked = std::exchange(EngineReadWindow::sceneWork, true);
				// The frame's engine globals, never the engine's (step 6e F2): the frame inputs' it takes from their slot (T6b3a; the pass
				// takes them again, which finds none newer).
				TakeFrameInputs();
				FrameGlobals::Scope scope(sceneInputs.globals);
				work();
				EngineReadWindow::sceneWork = marked;
			})));
	}

	void SceneStore::JoinSceneTask()
	{
		if (auto job = std::static_pointer_cast<AsyncWorker::JobHandle>(std::exchange(sceneTask, nullptr))) {
			// The frame's work must be done before anything reads the store: no budget, no inline fallback.
			const auto result = AsyncWorker::Get().Wait(*job, std::chrono::hours(1));
			if (result == AsyncWorker::WaitResult::Failed && !std::exchange(sceneTaskFailedLogged, true))
				logger::error("[DCLF] the scene task threw; the frame's tables are whatever it left");
		}
		sceneTaskInFlight.store(false, std::memory_order_relaxed);
		// What the work held for PrimaryCull, now (T6b3a: a queue; the frame's start drains it too, so the join is not what delivers it).
		DeliverPrimaryNotes();
	}

	bool SceneStore::TryJoinSceneTask()
	{
		if (const auto job = std::static_pointer_cast<AsyncWorker::JobHandle>(sceneTask); job && !AsyncWorker::Ended(*job)) {
			++presentsRunning;
			return false;
		}
		++presentsJoined;
		JoinSceneTask();
		return true;
	}
}
