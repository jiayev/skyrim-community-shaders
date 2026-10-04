#include "Internal.h"

#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"

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
		if ((object.flags & kObjectNoBindings) || !PipelineDrawable(p))
			return a_why = 0, false;
		if (m >= lookups.materials.size() || m >= tables.materialSlotKey.size() || !lookups.materials[m].resolved || lookups.materials[m].key != tables.materialSlotKey[m])
			return a_why = 1, false;
		if (tables.TechniqueOf(p).shadowMask && lookups.pipelines[p].shadowMaskIndex == Lookups::kNone)
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
		ZoneScopedN("CS.DCLF.Scene.CommitSet");
		++setStats.commits;
		const std::size_t objects = tables.objects.size();
		// A load screen: nothing is drawn, and the set is empty.
		const bool live = sceneBuilt;
		const std::uint32_t drawn = live ? SetPhasesDrawn() : 0u;
		auto& setPhases = tables.setPhases;
		setQueueMark.resize(objects, 0);
		setWaitingMark.resize(objects, 0);
		setRebinding.resize(objects, 0);
		setLacking.resize(objects, 0);
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
			setLacking.assign(objects, 0);
			setLackingCount = {};
			setMemberSlot.clear();
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
		const std::uint64_t readiness = (std::uint64_t(lookups.versionCounter) << 32) ^ (std::uint64_t(lookups.shadowGeneration) << 1) ^ lookups.generation;
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
			// phase now and at the last commit, whose pipeline slot's forward pipeline is ready. A joiner is the engine's for a frame.
			if (phases & kSetReflection) {
				const bool wasMain = (setPhases[a_slot] & kSetMain) != 0;
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
		// The geometries whose main phase changed, for the stand-in's admission and walks (PrimaryCull::NoteSetChanges).
		std::vector<const RE::BSGeometry*> joined, left;
		auto apply = [&](std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait) {
			// The occluder phases it takes part in and misses (SetLacking).
			const std::uint8_t lacking = live ? static_cast<std::uint8_t>(SetParticipation(a_slot, drawn) & ~a_phases & (kSetOccluderSky | kSetOccluderPrecipitation)) : 0;
			if (lacking != setLacking[a_slot]) {
				for (std::uint32_t v = 0; v < 2; ++v) {
					const std::uint8_t bit = v ? kSetOccluderPrecipitation : kSetOccluderSky;
					setLackingCount[v] += ((lacking & bit) ? 1u : 0u) - ((setLacking[a_slot] & bit) ? 1u : 0u);
				}
				setLacking[a_slot] = lacking;
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
			// The record's bit is the main phase's (what the main builds' GPU inputs carry); the shadow builds read the phases.
			auto& object = tables.objects[a_slot];
			object.flags = (a_phases & kSetMain) ? (object.flags | kObjectMember) : (object.flags & ~kObjectMember);
			tables.NoteChange(a_slot, kChangeBindings);
			if (!before || !a_phases)
				++(a_phases ? setStats.joined : setStats.left);
			// The engine's copy is by geometry, a base's alone: a layer draws its base's geometry and is a member with it.
			if (tables.IsLayer(a_slot))
				return;
			if ((before & kSetMain) != (a_phases & kSetMain))
				if (const auto* geometry = (a_phases & kSetMain) ? tables.objectGeometry[a_slot] : setGeometry[a_slot])
					((a_phases & kSetMain) ? joined : left).push_back(geometry);
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
		if (!joined.empty() || !left.empty())
			PrimaryCull::Get().NoteSetChanges(joined, left);
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
		PassCapture::Get().PublishSet(setSnapshot);
	}
}
