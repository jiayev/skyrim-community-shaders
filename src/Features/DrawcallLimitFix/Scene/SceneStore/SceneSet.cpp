#include "Internal.h"
#include "Features/DrawcallLimitFix/Common/FrameTrace.h"

#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Common/AsyncWorker.h"
#include "Features/DrawcallLimitFix/Common/Toggles.h"

// The DCLF set (SceneSet.h; drawcall-limit-fix.md, "The DCLF set"): who draws an object, decided once a frame.

namespace DCLF
{
	std::uint32_t SceneStore::SetPhasesDrawn()
	{
		// The main camera's passes are DCLF's whenever it runs; a shadow mode's views once its last shadow epoch drew them (a kind of
		// view seen for the first time, or a frame it could not draw, leaves the next frame's casters of the mode to the engine).
		return kSetMain | (ActiveToggles().shadows ? IndirectDraws::Get().ShadowPhasesDrawn() : 0u) |
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
		// What a main build's pair resolution needs from the render thread's lookups (MainBuild::ResolvePair, WriteMaterialRow,
		// WritePipelineRow). What it checks beyond these (constant groups that fit, the samplers a material names) is the
		// tables' own structure: a member it fails is a defect, which set parity reports.
		const auto& object = tables.objects[a_slot];
		const std::uint32_t p = object.pipelineIndex, m = object.materialIndex;
		if ((object.flags & kObjectNoBindings) || !PipelineDrawableIn(tables, lookupsView, p))
			return a_why = 0, false;
		if (m >= lookupsView.materials.size() || m >= tables.materialSlotKey.size() || !lookupsView.materials[m].resolved || lookupsView.materials[m].key != tables.materialSlotKey[m])
			return a_why = 1, false;
		if (tables.TechniqueShadowMask(p) && lookupsView.pipelines[p].shadowMaskIndex == Lookups::kNone)
			return a_why = 2, false;
		if (!lookupsView.samplersResolved || lookupsView.nullTexture == Lookups::kNone)
			return a_why = 3, false;
		// A ProjectedUV pipeline binds the projected textures where its material binds none (but the Hair technique, which binds none).
		if (const auto pass = tables.pipelines[p].passDescriptor; (pass & 0x8000u) && ((pass >> 24) & 0x3f) != 6)
			for (const auto index : lookupsView.projectedTextures)
				if (index == Lookups::kNone)
					return a_why = 3, false;
		if (object.geometryIndex >= tables.geometries.size() || !tables.geometries[object.geometryIndex].vertexAddress ||
			!tables.geometries[object.geometryIndex].indexAddress)
			return a_why = 4, false;
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

	void SceneStore::CommitSet()
	{
		DCLF_FRAME_TRACE("CommitSet");  // TEMP frame trace
		ZoneScopedN("CS.DCLF.Scene.CommitSet");
		++setStats.commits;
		const std::size_t objects = tables.objects.size();
		// A load screen: nothing is drawn, and the set is empty.
		const bool live = sceneBuilt;
		const std::uint32_t drawn = live ? SetPhasesDrawn() : 0u;
		// The commit's own copy: the records and Tables::setPhases take it at the next ApplySet.
		auto& setPhases = setPhasesNext;
		setQueueMark.resize(objects, 0);
		setWaitingMark.resize(objects, 0);
		setRebinding.resize(objects, 0);
		setLackingNext.resize(objects, 0);
		setApplyMark.resize(objects, 0);
		setCommitFrame = frame;
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
		setPhaseMask = drawn;
		if (everything) {
			++setStats.resyncs;
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
		// The reflection's joiners of the last commit, whose main membership is now a frame old.
		std::swap(setLagged, setLaggedNext);
		setLaggedNext.clear();
		for (const std::uint32_t slot : setLagged)
			if (slot < objects)
				QueueSet(slot);
		setLagged.clear();
		// Readiness moved: anything the lookups resolve is a new version of them (Lookups::NextVersion), the shadow lookups
		// have their own generation.
		const std::uint64_t readiness = (std::uint64_t(lookupsView.versionCounter) << 32) ^ (std::uint64_t(lookupsView.shadowGeneration) << 1) ^ lookupsView.generation;
		if (readiness != setReadiness) {
			setReadiness = readiness;
			++setStats.readinessEvents;
			for (const std::uint32_t slot : setWaiting)
				QueueSet(slot);
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
		// Fade roots became DCLF's to service or stopped being: an occluder under one is ready only while it is (PhaseReady).
		if (fadeOwnershipSerial != setFadeOwnership) {
			setFadeOwnership = fadeOwnershipSerial;
			for (std::uint32_t slot = 0; slot < objects; ++slot)
				if (setPhases[slot] & (kSetOccluderSky | kSetOccluderPrecipitation))
					QueueSet(slot);
			for (const std::uint32_t slot : setWaiting)
				QueueSet(slot);
		}
		// The frame globals a membership pass reads changed: this frame's accumulate phase binds every resident again
		// (BindByMembership), so none of them is a member this frame.
		const bool rebindAll = live && PrimaryCull::Get().SampleMembershipWitness() != membershipWitness;
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
			if (!MemberBindingStands(slot, *geometry, entry->second)) {
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
			const auto* property = entry != tracked.end() ? entry->first->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			const bool fading = property && property->fadeNode && property->fadeNode->GetRuntimeData().currentFade < 1.0f;
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

		// One slot's phases now, each with its readiness (0: not a member); a_wait: it takes part in a phase it is not ready for.
		auto wanted = [&](std::uint32_t a_slot, bool& a_wait) -> std::uint8_t {
			a_wait = false;
			if (!live || a_slot >= objects)
				return 0;
			std::uint8_t phases = SetParticipation(a_slot, drawn);
			if (!phases)
				return 0;
			auto waiting = [&](std::uint32_t a_why) {
				a_wait = true;
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
					++setStats.rebinding;
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
			// The reflection's faces draw from the last frame's depth inputs (IndirectDraws::ExecuteReflection): a member of the main
			// phase now and in the claims in effect (an application can wait for its revision, so not the last commit's), whose
			// pipeline slot's forward pipeline is ready. A joiner is the engine's for a frame.
			if (phases & kSetReflection) {
				const bool wasMain = a_slot < tables.setPhases.size() && (tables.setPhases[a_slot] & kSetMain) != 0;
				if (!(phases & kSetMain) || !wasMain || !IndirectDraws::Get().PhaseReady(a_slot, kSetReflection)) {
					if (phases & kSetMain) {
						waiting(8);
						if (!wasMain)
							setLaggedNext.push_back(a_slot);
					}
					phases &= ~kSetReflection;
				}
			}
			for (const std::uint8_t phase : { kSetCaster, kSetCasterPoint, kSetOccluderSky, kSetOccluderPrecipitation })
				if ((phases & phase) && !IndirectDraws::Get().PhaseReady(a_slot, phase)) {
					waiting(7);
					phases &= ~phase;
				}
			return phases;
		};
		auto apply = [&](std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait) {
			// Waiting for its application: decided again, for the geometry it holds now.
			if (setApplyMark[a_slot])
				markApply(a_slot);
			// The occluder phases it takes part in and misses (SetLacking).
			const std::uint8_t lacking = live ? static_cast<std::uint8_t>(SetParticipation(a_slot, drawn) & ~a_phases & (kSetOccluderSky | kSetOccluderPrecipitation)) : 0;
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
			bool wait = false;
			std::uint8_t phases = wanted(slot, wait);
			if (partner != kNoObjectSlot && partner < objects) {
				bool partnerWait = false;
				std::uint8_t partnerPhases = wanted(partner, partnerWait);
				if (!(phases & kSetMain) != !(partnerPhases & kSetMain)) {
					++setStats.waitingBy[6];
					phases &= ~(kSetMain | kSetReflection);
					partnerPhases &= ~(kSetMain | kSetReflection);
				}
				apply(partner, partnerPhases, partnerWait);
			}
			apply(slot, phases, wait);
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
		setStats.waiting += setWaiting.size();

		if (setSnapshotDirty || !setSnapshot) {
			auto snapshot = std::make_shared<SetSnapshot>(*setBuilding);
			snapshot->frame = frame;
			snapshot->drawn = static_cast<std::uint8_t>(drawn);
			setSnapshot = std::move(snapshot);
			setSnapshotDirty = false;
			++setStats.publications;
		}
		// Where the log stood at this commit: the applied set's revocation reads from here (ApplySet).
		setCommitCursor.Restart(tablesGeneration);
		setCommitCursor.Advance(tables.changeLog);
	}

	void SceneStore::ApplySet()
	{
		DCLF_FRAME_TRACE("ApplySet");  // TEMP frame trace
		ZoneScopedN("CS.DCLF.Scene.ApplySet");
		const std::size_t objects = tables.objects.size();
		WriteBoth([&](Tables& a_tables) { a_tables.setPhases.resize(objects, 0); });
		auto& applied = tables.setPhases;
		setLacking.resize(objects, 0);
		setPhasesNext.resize(objects, 0);
		setLackingNext.resize(objects, 0);
		setPhasesApplied.resize(objects, 0);
		setGeometryApplied.resize(objects, nullptr);
		// The structural changes since the join that made the selected revision (the last revocation), read before this
		// application's own notes: the claims applied now were committed without them, and the join takes them back.
		NoteStructureChanges();
		// After a withdrawal, every slot the commits decided, whatever changed since: the withdrawal took every claim back.
		if (std::exchange(setWithdrawn, false)) {
			setApplyMark.resize(objects, 0);
			setGeometryNext.resize(objects, nullptr);
			for (std::uint32_t slot = 0; slot < objects; ++slot) {
				if (setApplyMark[slot] || (!setPhasesNext[slot] && !setLackingNext[slot]))
					continue;
				setApply.emplace_back(slot, setGeometryNext[slot]);
				setApplyMark[slot] = static_cast<std::uint32_t>(setApply.size());
			}
		}
		// The geometries whose main claim this changes, for the stand-in's admission and walks (PrimaryCull::NoteSetChanges): from
		// the claims as they were (a revoked one included) to the claims applied, whatever commits lie between.
		std::vector<const RE::BSGeometry*> joined, left;
		for (const auto& [slot, geometry] : setApply) {
			if (slot < setApplyMark.size())
				setApplyMark[slot] = 0;
			if (slot >= objects)
				continue;
			// The record the commit decided for: a slot freed since (a detach at Present) or holding another geometry takes nothing.
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
				for (std::uint32_t v = 0; v < 2; ++v) {
					const std::uint8_t bit = v ? kSetOccluderPrecipitation : kSetOccluderSky;
					setLackingCount[v] += ((lacking & bit) ? 1u : 0u) - ((setLacking[slot] & bit) ? 1u : 0u);
				}
				setLacking[slot] = lacking;
			}
			if (applied[slot] == phases)
				continue;
			// The record's bit is the main phase's (what the main builds' GPU inputs carry); the shadow builds read the phases. Into the
			// frame's snapshot as into the tables (step 6c).
			WriteBoth([&](Tables& a_tables) {
				a_tables.setPhases[slot] = phases;
				if (!(a_tables.objects[slot].flags & kObjectFree)) {
					auto& object = a_tables.objects[slot];
					object.flags = (phases & kSetMain) ? (object.flags | kObjectMember) : (object.flags & ~kObjectMember);
					a_tables.NoteChange(slot, kChangeBindings);
				}
			});
		}
		setApply.clear();
		PassCapture::Get().PublishSet(setSnapshot);
		if (!joined.empty() || !left.empty())
			PrimaryCull::Get().NoteSetChanges(joined, left);
		// What changed since the applied commit (the rest of its walk, its accumulate phase's drops) is what RevokeUndrawnClaims checks
		// next, at the frame's start (step 6c: the frame reads the snapshot, so nothing changes under it afterwards).
		if (setCommitCursor.Continues(tables.changeLog, tablesGeneration)) {
			revokeCursor = setCommitCursor;
		} else {
			revokeCursor.Restart(tablesGeneration);
			revokeCursor.Advance(tables.changeLog);
		}
	}

	void SceneStore::WithdrawSet()
	{
		ZoneScopedN("CS.DCLF.Scene.WithdrawSet");
		if (setWithdrawn)
			return;
		setWithdrawn = true;
		std::vector<const RE::BSGeometry*> left;
		for (std::uint32_t slot = 0; slot < setPhasesApplied.size(); ++slot) {
			if ((setPhasesApplied[slot] & kSetMain) && slot < setGeometryApplied.size() && setGeometryApplied[slot])
				left.push_back(setGeometryApplied[slot]);
			setPhasesApplied[slot] = 0;
		}
		std::fill(setGeometryApplied.begin(), setGeometryApplied.end(), nullptr);
		WriteBoth([&](Tables& a_tables) {
			auto& applied = a_tables.setPhases;
			for (std::uint32_t slot = 0; slot < applied.size(); ++slot) {
				if (!std::exchange(applied[slot], std::uint8_t{ 0 }) || slot >= a_tables.objects.size())
					continue;
				if (auto& object = a_tables.objects[slot]; !(object.flags & kObjectFree) && (object.flags & kObjectMember)) {
					object.flags &= ~kObjectMember;
					a_tables.NoteChange(slot, kChangeBindings);
				}
			}
		});
		// No claims for the engine's hooks: every phase the engine's (drawn 0: the occlusion maps too); no sun exclusion (it names the
		// casters the last shadow epoch drew).
		auto none = std::make_shared<SetSnapshot>();
		none->frame = frame;
		PassCapture::Get().PublishSet(std::move(none));
		SunAccumulation::Get().PublishExclusion(nullptr);
		if (!left.empty())
			PrimaryCull::Get().NoteSetChanges({}, left);
		// What the frame's work changes from here is what RevokeUndrawnClaims checks (nothing is claimed).
		structureChanged.clear();
		revokeCursor.Restart(tablesGeneration);
		revokeCursor.Advance(tables.changeLog);
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
				const bool main = (flags & kObjectMember) && !(flags & kObjectNoBindings);
				drawn = static_cast<std::uint8_t>((main ? (kSetMain | kSetReflection) : 0u) | (tables.setPhases[a_slot] & ~(kSetMain | kSetReflection)));
			}
			// A structural change since the selected revision's join (R3b): its shapes were made without it, so the claim goes whole.
			const std::uint8_t lost = (same && a_structure) ? claimed : static_cast<std::uint8_t>(claimed & ~drawn);
			if (!lost)
				return;
			if (same && a_structure && !(claimed & ~drawn))
				++revokedStructureGeometries;
			setPhasesApplied[a_slot] &= ~lost;
			if (same) {
				// Out of the frame's set in what it no longer draws: the accumulate phase must not make it a member again.
				WriteBoth([&](Tables& a_tables) {
					a_tables.setPhases[a_slot] &= ~lost;
					if ((lost & kSetMain) && (a_tables.objects[a_slot].flags & kObjectMember)) {
						a_tables.objects[a_slot].flags &= ~kObjectMember;
						a_tables.NoteChange(a_slot, kChangeBindings);
					}
				});
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
			// Those structural changes, with every one read since the selected revision's join (ApplySet's, before its own notes).
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
		auto& capture = PassCapture::Get();
		if (const auto current = capture.CurrentSet()) {
			auto snapshot = std::make_shared<SetSnapshot>(*current);
			for (const auto& [geometry, lost] : revoked) {
				const auto it = snapshot->phases.find(geometry);
				if (it == snapshot->phases.end())
					continue;
				it->second &= ~lost;
				if (!it->second)
					snapshot->phases.erase(it);
			}
			capture.PublishSet(std::move(snapshot));
		}
		for (const auto& [geometry, lost] : revoked) {
			++revokedGeometries;
			if (lost & kSetMain) {
				++revokedMain;
				PrimaryCull::Get().NoteMemberLost(geometry);
			}
		}
	}

	void SceneStore::BeginFrame()
	{
		// Switch events are taken on the render thread only (PushSwitch); ApplyEvents runs on the coordinator.
		switchEventThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		++frame;
		publishedSunGeneration = sunCandidatesGeneration;
	}

	void SceneStore::RunSceneWork(bool a_task)
	{
		// The frame's events, which the render thread ingested before the kick (BeginSceneFrame): what they drop of PrimaryCull's is
		// held for the join, and the references they let go of are released at Present.
		inSceneTask = a_task;
		holdPrimaryNotes = true;
		ApplyEvents();
		BuildFrame(Phase::Scene);
		CommitSet();
		EndSceneFrame();
		holdPrimaryNotes = false;
		inSceneTask = false;
		sceneWorkPending = true;
	}

	void SceneStore::HandOverAtFrameStart()
	{
		// To the coordinator.
		if (fadeRootsSentHeld)
			tables.fadeRootsJournal.BeginBuild(*std::exchange(fadeRootsSentHeld, std::nullopt));
		if (pendingFadeOwned)
			ApplyFadeRootsOwned(*std::exchange(pendingFadeOwned, std::nullopt));
		if (std::exchange(pendingFadeReseed, false))
			ApplyReseedOwnedFadeRoots();
		if (std::exchange(lookupsResetPending, false)) {
			const auto lookupGeneration = lookups.generation;
			const auto shadowGeneration = lookups.shadowGeneration;
			const auto lookupVersion = lookups.versionCounter;
			lookups = Lookups{};
			lookups.generation = lookupGeneration + 1;
			lookups.shadowGeneration = shadowGeneration + 1;
			lookups.versionCounter = lookupVersion;
		}
		if (const std::array<std::uint64_t, 3> key{ lookups.versionCounter, lookups.generation, lookups.shadowGeneration }; key != lookupsViewKey) {
			lookupsView = lookups;
			lookupsViewKey = key;
		} else {
			lookupsView.samplersResolved = lookups.samplersResolved;
			lookupsView.nullTexture = lookups.nullTexture;
			lookupsView.projectedTextures = lookups.projectedTextures;
		}
		// To the frame.
		auto append = [](auto& a_to, auto& a_from) {
			a_to.insert(a_to.end(), std::make_move_iterator(a_from.begin()), std::make_move_iterator(a_from.end()));
			a_from.clear();
		};
		append(frameRetiredMaterialSlots, tables.retiredMaterialSlots);
		append(frameRetiredPipelineSlots, tables.retiredPipelineSlots);
		append(frameShadowTextureChanges, tables.shadowTextureChanges);
		append(frameRetiredImports, retiredImports);
		append(frameSwitchChanges, switchesApplied);
		frameSwitchResync = frameSwitchResync || std::exchange(switchResync, false) || !SwitchEventsLive();
		frameSunCandidates = sunCandidates;
		frameSunGeneration = sunCandidatesGeneration;
		frameLightCandidates = lightCandidates;
		frameLightGeneration = lightCandidatesGeneration;
		frameLightEntriesAppeared = lightEntriesAppeared;
	}

	void SceneStore::AcceptTables()
	{
		// Nothing published yet (the first frames): published now, with the coordinator idle.
		if (!publishedTables)
			PublishTables();
		acceptedTables = publishedTables;
		// The publication and the tables are equal here: nothing writes the tables between the scene work's end and the frame's start
		// but the frame's start itself (WriteBoth). A write that did is a defect, loud: the tables are published again now.
		if (acceptedTables->changeLog.End() != tables.changeLog.End() || acceptedTables->versionCounter != tables.versionCounter ||
			acceptedTables->objects.size() != tables.objects.size() || acceptedTables->pipelines.size() != tables.pipelines.size() ||
			acceptedTables->materials.size() != tables.materials.size()) {
			if (tablesPublication.republished++ == 0)
				logger::error("[DCLF] frame {}: the tables changed after their publication (log {} -> {}, versions {} -> {}); published again at the frame's start",
					frame, acceptedTables->changeLog.End(), tables.changeLog.End(), acceptedTables->versionCounter, tables.versionCounter);
			PublishTables();
			acceptedTables = publishedTables;
		}
	}

	void SceneStore::PublishTables()
	{
		ZoneScopedN("CS.DCLF.Scene.PublishTables");
		const auto start = std::chrono::steady_clock::now();
		// A snapshot only the pool holds (no frame accepted it, or every holder let go): written again, reusing its storage.
		std::shared_ptr<Tables> target;
		for (auto& pooled : tablesPool)
			if (pooled.use_count() == 1 && pooled != publishedTables) {
				target = pooled;
				break;
			}
		if (target) {
			++tablesPublication.reused;
		} else {
			target = tablesPool.emplace_back(std::make_shared<Tables>());
			++tablesPublication.made;
		}
		*target = tables;
		publishedTables = target;
		++tablesPublication.published;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		tablesPublication.ms += ms;
		tablesPublication.maxMs = std::max(tablesPublication.maxMs, ms);
	}

	void SceneStore::KickSceneTask(std::function<void()> a_work, const char* a_name)
	{
		// The scene's lane (step 6c): the frame's builds never queue behind it. Joined at Present (or the next frame's start).
		sceneTaskInFlight.store(true, std::memory_order_relaxed);
		sceneTask = std::static_pointer_cast<void>(std::make_shared<AsyncWorker::JobHandle>(
			AsyncWorker::Get().SubmitScene(a_name, [this, work = std::move(a_work)](std::stop_token) {
				sceneLaneThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
				sceneWorkThread = true;
				work();
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
		FinishSceneWork();
	}

	void SceneStore::FinishSceneWork()
	{
		if (std::exchange(accumulateWorkPending, false))
			FinishAccumulateWork();
		if (!std::exchange(sceneWorkPending, false))
			return;
		auto& primary = PrimaryCull::Get();
		for (const void* key : hiddenKeysHeld)
			primary.NoteHiddenKey(key);
		hiddenKeysHeld.clear();
		if (std::exchange(allMembersLostHeld, false))
			primary.NoteAllMembersLost();
		auto& draws = IndirectDraws::Get();
		// The shapes a scene revision made now would have (R3c), before the jobs below read the tables' buffers.
		draws.MakeRevisionShapes();
		// The stood-in fade roots' write-back and the early shadow build read the frame's snapshot: kicked at the next frame's start.
	}
}
