// The LOD gates' scene side (Engine/Cull/LodGates.h has the engine side; dclf-async-publication.md, "T6b5, the scene side"): the
// coordinator's gates, opened by their events, the commit's withholding and flips, and the outcomes that retire them (drained from the
// engine side: LodGates::DrainOutcomes). Scene lane.

#include "Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LodGates.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/GateAdoption.h"

#include <algorithm>
#include <type_traits>

namespace DCLF::Scene
{
	void Coordinator::ApplyGateEvent(std::uint64_t a_token, std::span<const void* const> a_incoming, std::span<const void* const> a_outgoing, std::uint64_t a_stampNs,
		std::uint32_t a_stampFrame)
	{
		if (!a_token)
			return;
		const auto [it, inserted] = lodGates.try_emplace(a_token);
		if (!inserted) {
			// One event a token (LodGates' are unique): a second is a defect, and the first stands.
			static std::uint32_t logged = 0;
			if (logged++ < 4)
				logger::warn("[DCLF] LOD gate {}: a second gate event for the same token, ignored <- GATE TOKEN", a_token);
			return;
		}
		++gateStats.opened;
		{
			auto& gate = it->second;
			gate.stampNs = a_stampNs ? a_stampNs : GateNowNs();
			gate.stampFrame = a_stampFrame;
		}
		// A root another gate still names (its block swapped again before that gate's outcome): this gate's from here (the coordinator's
		// mark, MarkRoot). A root that changes side is gated only while incoming (an outgoing block's hide is the engine's own): the
		// incoming own their hide.
		const std::uint32_t component = GateAdoption::component.index;
		const Api::CoordinatorComponentContext context{ tables, ComponentState(component), *this, component };
		// The commit runs live (the gates settle) while any gate stands.
		context.SetCommitLive(true);
		auto take = [&](const void* a_root, std::uint8_t a_side) {
			if (const auto displaced = context.MarkRoot(a_root, a_token, a_side, a_side == kGateIncoming)) {
				if (displaced.component.index == component)
					if (const auto other = lodGates.find(displaced.token); other != lodGates.end()) {
						std::erase(other->second.incoming, a_root);
						std::erase(other->second.outgoing, a_root);
					}
				++gateStats.superseded;
			}
		};
		for (const void* root : a_incoming) {
			if (!root)
				continue;
			// Its own hide the gate's from here (HiddenForWalk): classified, written, bound and committed while the engine keeps it hidden.
			take(root, kGateIncoming);
			lodGates.find(a_token)->second.incoming.push_back(root);
			// The tracked geometries under it (its attach applied before this event: tracked by an earlier pass, or by this batch's adds after
			// the mirror's events, which AddGeometry tags); a root or node the mirror lacks a record of holds the gate until it has one.
			if (TagGateRoot(a_token, root, kGateIncoming))
				++lodGates.find(a_token)->second.unmirrored;
		}
		for (const void* root : a_outgoing) {
			if (!root)
				continue;
			take(root, kGateOutgoing);
			lodGates.find(a_token)->second.outgoing.push_back(root);
			TagGateRoot(a_token, root, kGateOutgoing);
		}
		if (lodGates.find(a_token)->second.incoming.empty())
			++gateStats.emptyIncoming;
	}

	Api::CoordinatorComponentContext Coordinator::GateContext()
	{
		const std::uint32_t component = GateAdoption::component.index;
		return { tables, ComponentState(component), *this, component };
	}

	bool Coordinator::TagGateRoot(std::uint64_t a_token, const void* a_root, std::uint8_t a_side)
	{
		// One walk of the mirror per root (not per pass): the subtree as the tracking walks it (SubtreeEntries), each tracked geometry tagged.
		auto& entries = gateEntries;
		entries.clear();
		bool lacking = false;
		GateContext().SubtreeEntries(a_root, entries, lacking);
		for (const void* geometry : entries)
			TagGate(static_cast<const RE::BSGeometry*>(geometry), a_token, a_side);
		return lacking;
	}

	void Coordinator::TagGate(const RE::BSGeometry* a_geometry, std::uint64_t a_token, std::uint8_t a_side)
	{
		const auto tag = gateTags.find(a_geometry);
		if (tag != gateTags.end() && tag->second.token == a_token && tag->second.side == a_side)
			return;
		const auto gate = lodGates.find(a_token);
		if (gate == lodGates.end())
			return;
		if (tag != gateTags.end())
			UntagGate(a_geometry);
		gateTags[a_geometry] = GateTag{ a_token, a_side };
		const auto context = GateContext();
		auto* geometry = const_cast<RE::BSGeometry*>(a_geometry);
		if (a_side == kGateIncoming) {
			gate->second.incomingGeometries.insert(geometry);
			++gateStats.taggedIncoming;
			// Withheld while its gate is open, its slots' readiness recorded (Observe: ready only by an evaluation under this gate, an earlier
			// gate's record of the slot is not this one's); a hold-out it had goes. Classified again with its root gated (a verdict taken while
			// the gate's hide counted stands no more; AddGeometry's own is pending already), and its slots committed again by the next commit.
			context.SetEntryVeto(a_geometry, gate->second.state == LodGate::State::Open ? Api::kVetoObserve : std::uint8_t{ 0 });
			context.Requeue(a_geometry, std::uint8_t(Api::kRequeueReevaluate | Api::kRequeueSlots));
		} else {
			gate->second.outgoingGeometries.insert(geometry);
			++gateStats.taggedOutgoing;
			// A hold-out it had goes (its gate's flip holds it out again).
			context.SetEntryVeto(a_geometry, 0);
		}
	}

	void Coordinator::TagGateUnder(const RE::BSGeometry* a_geometry, const Api::Mark& a_mark)
	{
		if (GateContext().TrackedStateOf(a_geometry).tracked)
			TagGate(a_geometry, a_mark.token, a_mark.side);
	}

	void Coordinator::UntagGate(const RE::BSGeometry* a_geometry)
	{
		const auto tag = gateTags.find(a_geometry);
		if (tag == gateTags.end())
			return;
		if (const auto gate = lodGates.find(tag->second.token); gate != lodGates.end())
			(tag->second.side == kGateIncoming ? gate->second.incomingGeometries : gate->second.outgoingGeometries).erase(const_cast<RE::BSGeometry*>(a_geometry));
		gateTags.erase(tag);
	}

	void Coordinator::ResetGateTags()
	{
		for (auto& [token, gate] : lodGates) {
			gate.incomingGeometries.clear();
			gate.outgoingGeometries.clear();
			gate.pending = 0;
		}
	}

	void Coordinator::GateRootDetached(const void* a_key, std::uint64_t a_token)
	{
		// A root detached leaves its gate (its mark erased, its hide ungated: UnmarkDetached; its geometries leave with their own erasure:
		// the forgot hook untags them). An open gate left without an incoming root never flips (its outgoing would be hidden over nothing):
		// the engine side's forced release retires it.
		if (const auto gate = lodGates.find(a_token); gate != lodGates.end()) {
			std::erase(gate->second.incoming, a_key);
			std::erase(gate->second.outgoing, a_key);
		}
		++gateStats.rootsDetached;
	}

	bool Coordinator::GateGeometryReady(const RE::BSGeometry* a_geometry) const
	{
		const Api::TrackedState entry = TrackedStateOf(a_geometry);
		// Classified since it was tagged (TagGate took its verdict again), with its leaf's records.
		if (entry.leafWaiting || !entry.classified)
			return false;
		// No record: ineligible for a recorded verdict (a hide still counts here when it is not the gate's: HiddenForWalk), or not written yet.
		if (entry.slot == kNoObjectSlot && entry.layerSlot == kNoObjectSlot)
			return entry.reason != static_cast<std::uint8_t>(Ineligible::None);
		// Each slot of it whole at its last evaluation by the commit (its Observe's record).
		for (const std::uint32_t slot : { entry.slot, entry.layerSlot })
			if (slot != kNoObjectSlot)
				if (const Api::ObservedState observed = ObservedSlotOf(slot); observed.geometry != a_geometry || !observed.ready)
					return false;
		return true;
	}

	void Coordinator::UpdateGates(bool a_live)
	{
		const auto context = GateContext();
		for (auto& [token, gate] : lodGates) {
			if (gate.state != LodGate::State::Open)
				continue;
			// Roots the mirror lacked a record of: walked again (a capture since; a subtree tracked since is tagged on the way).
			if (gate.unmirrored) {
				std::uint32_t unmirrored = 0;
				for (const void* root : gate.incoming)
					unmirrored += TagGateRoot(token, root, kGateIncoming) ? 1u : 0u;
				gate.unmirrored = unmirrored;
			}
			gate.pending = 0;
			for (auto* geometry : gate.incomingGeometries)
				if (!GateGeometryReady(geometry))
					++gate.pending;
			if (!a_live || gate.pending || gate.unmirrored || gate.incoming.empty())
				continue;
			// Ready, but an incoming record not published yet: its structure changed since the last publication's revocation (a record
			// written or rewritten since: RestructuredSincePublication). The publication this commit makes takes their claims back whole
			// (RevokeUndrawnClaims: the shapes were made without the change) and claims them only at the next one: a gate flipped on one would
			// be released with its incoming unclaimed (every incoming geometry of a gate flipped in the pass that wrote its records: <- GATE
			// HOLE). The flip waits for the commit after that publication (the change log's notes queue the slot there), so the flip's
			// publication claims every incoming geometry.
			std::uint32_t unpublishedRecords = 0;
			for (auto* geometry : gate.incomingGeometries)
				if (const auto entry = context.TrackedStateOf(geometry);
					entry.tracked && (context.RestructuredSincePublication(entry.slot) || context.RestructuredSincePublication(entry.layerSlot)))
					++unpublishedRecords;
			if (unpublishedRecords) {
				gate.pending = unpublishedRecords;
				++gateStats.structureDeferred;
				continue;
			}
			// The flip: the incoming slots' observed phases and the outgoing slots' none, in this commit (the vetoes the settle drops or sets
			// apply in it); the publication carries it.
			GateFlip flip;
			flip.token = token;
			flip.requestNs = gate.stampNs;
			flip.requestFrame = gate.stampFrame;
			gate.flipNs = GateNowNs();
			gate.flipFrame = SceneCapture::Frame();
			flip.flipNs = gate.flipNs;
			flip.flipFrame = gate.flipFrame;
			for (auto* geometry : gate.incomingGeometries) {
				const auto entry = context.TrackedStateOf(geometry);
				if (!entry.tracked)
					continue;
				// Its Observe dropped: each slot the commit last evaluated for it takes the phases recorded then.
				context.SetEntryVeto(geometry, 0);
				// The claims are by base geometry (a layer is claimed with its base).
				if (entry.slot != kNoObjectSlot)
					if (const auto observed = context.ObservedSlot(entry.slot); observed.geometry == geometry && observed.phases && !tables.IsLayer(entry.slot))
						flip.incoming.emplace_back(geometry, observed.phases);
			}
			for (auto* geometry : gate.outgoingGeometries) {
				const auto entry = context.TrackedStateOf(geometry);
				if (!entry.tracked)
					continue;
				// Held out of the set until its detach (the engine side detaches it at a LOD drain after the flip's release): its slots none.
				if (entry.veto != Api::kVetoHold)
					++gateStats.heldOut;
				context.SetEntryVeto(geometry, Api::kVetoHold);
				flip.outgoing.push_back(geometry);
			}
			gate.state = LodGate::State::Flipped;
			++gateStats.flipped;
			gateStats.requestToFlipMs.push_back(GateMs(gate.stampNs, gate.flipNs));
			gateStats.requestToFlipFrames.push_back(gate.flipFrame >= gate.stampFrame ? gate.flipFrame - gate.stampFrame : 0u);
			// Published by this commit's publication, as the gates' component's delta (released by the frame installing it: GateAdoption).
			context.PublishDelta(std::make_shared<const GateFlip>(std::move(flip)));
		}
	}

	void Coordinator::TakeGateOutcomes()
	{
		// Before the tracker's stack is drained (CollectEvents): every outcome taken here was posted after its gate's events were pushed.
		if (!LodGates::Enabled() && lodGates.empty())
			return;
		LodGates::DrainOutcomes([this](const LodGates::Outcome& a_outcome) { gateOutcomesHeld.push_back(GateOutcome{ a_outcome.token, a_outcome.forced, a_outcome.kept }); });
	}

	void Coordinator::ApplyGateOutcomes()
	{
		// Only by a pass that applies a batch (the channel's AfterBatch keys): a load screen's pass carried its events for the mirror (its gate
		// events among them), and the outcomes wait for the pass that applies them.
		if (gateOutcomesHeld.empty())
			return;
		for (const auto& outcome : gateOutcomesHeld) {
			// A kept notice (in order with the retirements: its gate's flip outcome may come before it or after it).
			if (outcome.kept[0]) {
				ApplyGateKept(outcome.token, outcome.kept);
				continue;
			}
			if (!lodGates.contains(outcome.token)) {
				++gateStats.unknownOutcomes;
				continue;
			}
			RetireGate(outcome.token, outcome.forced);
		}
		gateOutcomesHeld.clear();
	}

	void Coordinator::RetireGate(std::uint64_t a_token, bool a_forced)
	{
		const auto it = lodGates.find(a_token);
		if (it == lodGates.end())
			return;
		auto& gate = it->second;
		const bool open = gate.state == LodGate::State::Open;
		// Its roots unmarked, the incoming ungated (flipped: the engine's show came as the root's hidden event, applied before this outcome).
		// A root listed is marked this gate's (a later gate's mark, or a detach, took it off the list).
		const std::uint32_t component = GateAdoption::component.index;
		const Api::CoordinatorComponentContext context{ tables, ComponentState(component), *this, component };
		for (const void* root : gate.incoming)
			context.UnmarkRoot(root, a_token);
		for (const void* root : gate.outgoing)
			context.UnmarkRoot(root, a_token);
		for (auto* geometry : gate.incomingGeometries) {
			if (!context.TrackedStateOf(geometry).tracked)
				continue;
			gateTags.erase(geometry);
			if (open) {
				// Forced before its flip: the block is the engine's as it shows it (or detaches it), so it is withheld no more, its verdict is
				// taken again without the gate and its slots committed with their own phases.
				context.SetEntryVeto(geometry, 0);
				context.Requeue(geometry, std::uint8_t(Api::kRequeueReevaluateAlways | Api::kRequeueSlots));
			}
		}
		// The outgoing: untagged; held out still when the gate flipped (their detach erases them).
		for (auto* geometry : gate.outgoingGeometries)
			if (context.TrackedStateOf(geometry).tracked)
				gateTags.erase(geometry);
		if (a_forced) {
			++gateStats.retiredForced;
			gateStats.forcedOpen += open ? 1u : 0u;
		} else {
			++gateStats.retiredFlipped;
		}
		lodGates.erase(it);
		context.SetCommitLive(!lodGates.empty());
	}

	void Coordinator::ApplyGateKept(std::uint64_t a_token, const std::array<const void*, 2>& a_roots)
	{
		// The engine reused a held outgoing block: it stays attached as its node's current block, drawn by the engine while DCLF holds it
		// out, and no detach will ever erase its entries. Its gate may still be open (reattached before the flip: the flip must not hold it
		// out), flipped, or retired already (the flip's outcome came first: the hold-out stays on entries with no gate).
		++gateStats.keptNotices;
		const auto gate = lodGates.find(a_token);
		const std::uint32_t component = GateAdoption::component.index;
		const Api::CoordinatorComponentContext context{ tables, ComponentState(component), *this, component };
		for (const void* root : a_roots) {
			if (!root)
				continue;
			if (const auto mark = context.MarkOf(root); mark.component.index == component && mark.token == a_token && mark.side == kGateOutgoing) {
				context.UnmarkRoot(root, a_token);
				if (gate != lodGates.end())
					std::erase(gate->second.outgoing, root);
			}
			// One walk of the mirror per notice (the gate's own tag walk, TagGateRoot): a root the mirror lost was detached, its entries erased.
			auto& entries = gateEntries;
			entries.clear();
			bool lacking = false;
			context.SubtreeEntries(root, entries, lacking);
			for (const void* key : entries) {
				const auto* geometry = static_cast<const RE::BSGeometry*>(key);
				const auto tag = gateTags.find(geometry);
				// This gate's outgoing tag, or a hold-out its retirement left (no gate tags it); a later gate's tag is that gate's.
				const bool ours = tag != gateTags.end() && tag->second.token == a_token && tag->second.side == kGateOutgoing;
				const bool heldOut = context.TrackedStateOf(geometry).veto == Api::kVetoHold;
				if (!ours && !(tag == gateTags.end() && heldOut))
					continue;
				UntagGate(geometry);
				if (!heldOut) {
					++gateStats.keptUntagged;
					continue;
				}
				// Held out by the flip (0 applied to its slots): no more, committed again with its own phases.
				context.SetEntryVeto(geometry, 0);
				++gateStats.keptCleared;
				context.Requeue(geometry, Api::kRequeueSlots);
			}
		}
	}

}
