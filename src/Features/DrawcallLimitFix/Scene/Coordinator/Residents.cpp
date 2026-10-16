// The coordinator's residents (scene lane): the resident set's slots marked, dropped and kept alive, and the fade nodes held for the
// slots' listings (the components list theirs at the same points: Components.cpp; the fade roots': FadeRoots.cpp).

#include "Internal.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/FadeRoots.h"

#include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"

namespace DCLF::Scene
{
	namespace
	{
		// The pipeline templates' parity (KeepResidentsAlive; scene lane only, the coordinator is one): its counters, reported every
		// kTemplateReportChecks checks.
		Diagnostics::ParityCounter templateParity;
		std::uint64_t templateParityPasses = 0;
		constexpr std::uint32_t kTemplateParityOffset = 37;
		constexpr std::uint64_t kTemplateReportChecks = 10;
	}

	void Coordinator::MarkResidentSlot(std::uint32_t a_slot, const ResidentPatch& a_patch)
	{
		NoteResidentTouched(a_slot);
		if (residentPos.size() <= a_slot)
			residentPos.resize(std::max<std::size_t>(a_slot + 1, tables.objects.size()), kNotResident);
		// The accumulate phase notes the join, and a patch that changed, with the rest of its write.
		ComponentsResident(a_slot);
		if (residentPos[a_slot] != kNotResident) {
			residentPatches[residentPos[a_slot]] = a_patch;
			return;
		}
		residentPos[a_slot] = static_cast<std::uint32_t>(residents.size());
		residents.push_back(a_slot);
		residentPatches.push_back(a_patch);
		if (a_slot < tables.residentSlot.size())
			tables.residentSlot[a_slot] = 1;
	}

	void Coordinator::DropResidentSlot(std::uint32_t a_slot, bool a_restore)
	{
		if (!IsResidentSlot(a_slot))
			return;
		NoteResidentTouched(a_slot);
		const std::uint32_t at = residentPos[a_slot];
		const std::uint32_t last = residents.back();
		residents[at] = last;
		residentPatches[at] = residentPatches.back();
		residentPos[last] = at;
		residents.pop_back();
		residentPatches.pop_back();
		residentPos[a_slot] = kNotResident;
		ComponentsLeft(a_slot);
		// The scene work's drops reach PrimaryCull as set changes (ApplySet) or as revoked claims (RevokeUndrawnClaims); the
		// accumulate phase's, at once.
		if (a_slot < tables.objectGeometry.size() && tables.objectGeometry[a_slot]) {
			if (!holdPrimaryNotes)
				PrimaryCull::Get().NoteMemberLost(tables.objectGeometry[a_slot]);
			else if (holdLostMembers)
				channels.primaryNotes.Push({ PrimaryNote::Kind::MemberLost, tables.objectGeometry[a_slot] });
		}
		if (a_slot < tables.residentSlot.size()) {
			tables.residentSlot[a_slot] = 0;
			tables.NoteChange(a_slot, kChangeMembership);
		}
		if (a_restore)
			ResetAccumulatedHalf(a_slot);
		// A layer and its base are members together (WriteLayer): the other leaves too.
		const std::uint32_t partner = tables.IsLayer(a_slot) ? tables.layerBase[a_slot] : a_slot < tables.layerOf.size() ? tables.layerOf[a_slot] : kNoObjectSlot;
		if (partner != kNoObjectSlot)
			DropResidentSlot(partner, a_restore);
	}

	void Coordinator::EndAllResidency()
	{
		residentMaintenanceDirty = true;
		if (holdPrimaryNotes)
			channels.primaryNotes.Push({ PrimaryNote::Kind::AllMembersLost, nullptr });
		else
			PrimaryCull::Get().NoteAllMembersLost();
		ComponentsAllLeft();
		for (const std::uint32_t slot : residents) {
			ComponentsLeft(slot);
			ResetAccumulatedHalf(slot);
			residentPos[slot] = kNotResident;
			if (slot < tables.residentSlot.size()) {
				tables.residentSlot[slot] = 0;
				tables.NoteChange(slot, kChangeMembership);
			}
		}
		residents.clear();
		residentPatches.clear();
	}

	void Coordinator::KeepResidentsAlive()
	{
		ZoneScopedN("CS.DCLF.Accumulate.KeepResidents");
		auto& counted = residentCounted;
		if (counted.size() < tables.objects.size())
			counted.resize(tables.objects.size());
		if (materialMembers.size() < tables.materials.size())
			materialMembers.resize(tables.materials.size(), 0);
		std::vector<std::uint32_t> pipelinesTouched, materialsTouched;
		// A resident's keys now (none for a slot that is not resident).
		auto keysOf = [&](std::uint32_t a_slot) {
			ResidentCounted keys;
			if (!IsResidentSlot(a_slot) || a_slot >= tables.objects.size())
				return keys;
			const auto& object = tables.objects[a_slot];
			keys.pipeline = object.pipelineIndex;
			keys.material = object.materialIndex;
			return keys;
		};
		auto uncount = [&](std::uint32_t a_slot) {
			auto& was = counted[a_slot];
			if (was.pipeline != ResidentCounted::kNone) {
				pipelineMembers.Remove(was.pipeline, a_slot);
				pipelinesTouched.push_back(was.pipeline);
			}
			if (was.material != ResidentCounted::kNone && was.material < materialMembers.size()) {
				--materialMembers[was.material];
				materialsTouched.push_back(was.material);
			}
			was = {};
		};
		auto count = [&](std::uint32_t a_slot) {
			const auto keys = keysOf(a_slot);
			if (keys.pipeline == ResidentCounted::kNone)
				return;
			pipelineMembers.Add(keys.pipeline, a_slot);
			pipelinesTouched.push_back(keys.pipeline);
			if (materialMembers.size() <= keys.material)
				materialMembers.resize(std::size_t(keys.material) + 1, 0);
			++materialMembers[keys.material];
			materialsTouched.push_back(keys.material);
			counted[a_slot] = keys;
		};
		if (residentMaintenanceDirty) {
			// Every residency ended, or the tables were reset: everything counted again.
			ZoneScopedN("CS.DCLF.Accumulate.RebuildResidentMaintenance");
			counted.assign(tables.objects.size(), ResidentCounted{});
			pipelineMembers.Clear();
			materialMembers.assign(tables.materials.size(), 0);
			tables.ClearUsed();
			for (const std::uint32_t slot : residents)
				count(slot);
			ComponentsRecount(true, {});
			residentMaintenanceDirty = false;
		} else {
			// The slots named since: their old keys out (all of them first: a fade root freed and listed again for another node
			// within the frame), then their keys now.
			std::sort(residentsTouched.begin(), residentsTouched.end());
			residentsTouched.erase(std::unique(residentsTouched.begin(), residentsTouched.end()), residentsTouched.end());
			for (const std::uint32_t slot : residentsTouched)
				if (slot < counted.size())
					uncount(slot);
			for (const std::uint32_t slot : residentsTouched)
				if (slot < counted.size())
					count(slot);
			ComponentsRecount(false, residentsTouched);
		}
		residentsTouched.clear();
		// The used sets: a pipeline or material is used while a resident holds it. The joins marked theirs as they bound; one
		// whose join failed is no resident's.
		pipelinesTouched.insert(pipelinesTouched.end(), joinMarkedPipelines.begin(), joinMarkedPipelines.end());
		materialsTouched.insert(materialsTouched.end(), joinMarkedMaterials.begin(), joinMarkedMaterials.end());
		joinMarkedPipelines.clear();
		joinMarkedMaterials.clear();
		for (const std::uint32_t p : pipelinesTouched)
			tables.SetPipelineUsed(p, pipelineMembers.Count(p) != 0);
		for (const std::uint32_t m : materialsTouched)
			tables.SetMaterialUsed(m, m < materialMembers.size() && materialMembers[m] != 0);
		// A pipeline's lighting template is its first member's held property (SlotProperty). It moves where the first member may
		// change, a pipeline whose membership changed (pipelinesTouched), and where that member's entry takes another property: a leaf
		// update's swap (HoldProperties, on PushLeafUpdate) rewrites no slot, so it notes the slot (NoteResidentTouched), whose recount
		// touches its pipeline. The old reference goes to the retirement chain, so the template takes the new one this pass.
		auto templateOf = [&](std::uint32_t a_pipeline) {
			const std::uint32_t slot = pipelineMembers.First(a_pipeline);
			return slot != ~0u ? SlotProperty(slot) : nullptr;
		};
		for (const std::uint32_t p : pipelinesTouched)
			if (pipelineMembers.Count(p) && p < tables.geometryTemplate.size())
				tables.geometryTemplate[p] = templateOf(p);
		// The parity (CS_DCLF_PERSISTENT_PARITY, every 60 passes): every pipeline's template against its first member's property now;
		// a difference is a held property that moved with no recount of its slot.
		if (Diagnostics::Observe().scene && Diagnostics::ParityDue(passSerial, kTemplateParityOffset)) {
			auto& tp = templateParity;
			for (std::uint32_t p = 0; p < pipelineMembers.lists.size() && p < tables.geometryTemplate.size(); ++p)
				if (pipelineMembers.Count(p)) {
					const auto* now = templateOf(p);
					tp.Check(tables.geometryTemplate[p] == now, [&] {
						return fmt::format("pipeline {} (first member slot {}): template {} where the member holds {}", p, pipelineMembers.First(p),
							static_cast<const void*>(tables.geometryTemplate[p]), static_cast<const void*>(now));
					});
				}
			if (++templateParityPasses % kTemplateReportChecks == 0) {
				logger::info("[DCLF] pipeline template parity (the touched refresh against every pipeline's first member): {} checks, {} differ{}",
					tp.checks, tp.mismatches, tp.Verdict(true));
				tp.Reset();
			}
		}
	}

	RE::NiPointer<RE::NiAVObject> Coordinator::HeldFadeNode(std::uint32_t a_slot, const void* a_node)
	{
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		const auto it = geometry ? tracked.find(const_cast<RE::BSGeometry*>(geometry)) : tracked.end();
		if (it != tracked.end() && it->second.fadeNodeRef.get() == a_node)
			return it->second.fadeNodeRef;
		// The mirror names another node than the entry holds (a field no capture brought): taken now, as at a capture (AcquireFadeNode).
		if (it != tracked.end()) {
			HandBack(std::move(it->second.fadeNodeRef));
			if (AcquireFadeNode(it->second, a_node, "a listing"))
				return it->second.fadeNodeRef;
		}
		// Asked of the render thread: listed when its answer comes (ApplyFadeNodeAnswers).
		++referenceStats.fadeRootsUnowned;
		return nullptr;
	}

	std::size_t Coordinator::StoodInFadeRoots() const
	{
		return FadeRootsStoodIn();
	}

	void NoteFadeRootUnmirrored()
	{
		Coordinator::Get().NoteFadeRootUnmirrored();
	}
}
