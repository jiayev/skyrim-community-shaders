// The LOD gates' scene side (Engine/Cull/LodGates.h has the engine side; dclf-async-publication.md, "T6b5, the scene side"): the
// coordinator's gates, opened by their events, the commit's withholding and flips, and the outcomes that retire them (drained from the
// engine side: LodGates::DrainOutcomes). Scene lane.

#include "Internal.h"

#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LodGates.h"

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
		// A root another gate still names (its block swapped again before that gate's outcome): this gate's from here. A root that changes
		// side is gated only while incoming (an outgoing block's hide is the engine's own).
		auto take = [&](const void* a_root, std::uint8_t a_side) {
			if (const auto at = gateOfRoot.find(a_root); at != gateOfRoot.end() && at->second.token != a_token) {
				if (const auto other = lodGates.find(at->second.token); other != lodGates.end()) {
					std::erase(other->second.incoming, a_root);
					std::erase(other->second.outgoing, a_root);
				}
				if (at->second.side == kGateIncoming && a_side != kGateIncoming)
					mirror.SetGated(a_root, false);
				++gateStats.superseded;
			}
			gateOfRoot.insert_or_assign(a_root, GateRoot{ a_token, a_side });
		};
		for (const void* root : a_incoming) {
			if (!root)
				continue;
			take(root, kGateIncoming);
			lodGates.find(a_token)->second.incoming.push_back(root);
			// Its own hide the gate's from here (HiddenForWalk): classified, written, bound and committed while the engine keeps it hidden.
			mirror.SetGated(root, true);
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

	bool Coordinator::TagGateRoot(std::uint64_t a_token, const void* a_root, std::uint8_t a_side)
	{
		// One walk of the mirror per root (not per pass): the subtree as the tracking walks it (MirrorSubtree), each tracked geometry tagged.
		std::vector<std::pair<RE::BSGeometry*, Ineligible>> found;
		bool lacking = mirror.Node(a_root) == nullptr;
		MirrorSubtree(a_root, Ineligible::None, found, lacking);
		for (const auto& [geometry, reason] : found)
			if (const auto it = tracked.find(geometry); it != tracked.end())
				TagGate(it->first, it->second, a_token, a_side);
		return lacking;
	}

	void Coordinator::TagGateAbove(RE::BSGeometry* a_geometry, Tracked& a_entry)
	{
		// The mirror's chain up to its category node (the walk's): the nearest gate root on it decides.
		const auto* record = mirror.Node(a_geometry);
		for (const void* key = record ? record->parent : nullptr; key;) {
			if (const auto at = gateOfRoot.find(key); at != gateOfRoot.end()) {
				TagGate(a_geometry, a_entry, at->second.token, at->second.side);
				return;
			}
			if (key == a_entry.categoryNode)
				return;
			const auto* node = mirror.Node(key);
			key = node ? node->parent : nullptr;
		}
	}

	void Coordinator::TagGate(RE::BSGeometry* a_geometry, Tracked& a_entry, std::uint64_t a_token, std::uint8_t a_side)
	{
		if (a_entry.gate == a_token && a_entry.gateSide == a_side)
			return;
		const auto gate = lodGates.find(a_token);
		if (gate == lodGates.end())
			return;
		if (a_entry.gate || a_entry.gateHeldOut)
			UntagGate(a_geometry, a_entry);
		a_entry.gate = a_token;
		a_entry.gateSide = a_side;
		a_entry.gateSeen = true;
		if (a_side == kGateIncoming) {
			gate->second.incomingGeometries.insert(a_geometry);
			++gateStats.taggedIncoming;
			// Classified again with its root gated (a verdict taken while the gate's hide counted stands no more; AddGeometry's own is pending
			// already), and its slots committed again by the next commit (gateQueued: withheld, their readiness recorded by GateWithhold).
			if (a_entry.candidateFrame != 0) {
				a_entry.candidateFrame = 0;
				pendingEvaluation.push_back(a_geometry);
			}
			for (const std::uint32_t slot : { a_entry.slot, a_entry.layerSlot })
				if (slot != kNoObjectSlot) {
					gateQueued.push_back(slot);
					// Ready only by an evaluation under this gate (an earlier gate's record of the slot is not this one's).
					if (slot < gateSlots.size())
						gateSlots[slot] = GateSlot{};
				}
		} else {
			gate->second.outgoingGeometries.insert(a_geometry);
			++gateStats.taggedOutgoing;
		}
	}

	void Coordinator::UntagGate(RE::BSGeometry* a_geometry, Tracked& a_entry)
	{
		if (a_entry.gate)
			if (const auto gate = lodGates.find(a_entry.gate); gate != lodGates.end())
				(a_entry.gateSide == kGateIncoming ? gate->second.incomingGeometries : gate->second.outgoingGeometries).erase(a_geometry);
		if (a_entry.gateHeldOut) {
			a_entry.gateHeldOut = false;
			--gateHeldOutCount;
		}
		a_entry.gate = 0;
		a_entry.gateSide = 0;
	}

	void Coordinator::ResetGateTags()
	{
		for (auto& [token, gate] : lodGates) {
			gate.incomingGeometries.clear();
			gate.outgoingGeometries.clear();
			gate.pending = 0;
		}
		gateHeldOutCount = 0;
	}

	void Coordinator::NoteGateRootsDetached(const void* a_root, std::span<const void* const> a_nodes)
	{
		// A root detached leaves its gate (its geometries leave with their own erasure: EraseTracked untags them). An open gate left without
		// an incoming root never flips (its outgoing would be hidden over nothing): the engine side's forced release retires it.
		auto leave = [&](const void* a_key) {
			const auto at = gateOfRoot.find(a_key);
			if (at == gateOfRoot.end())
				return;
			if (const auto gate = lodGates.find(at->second.token); gate != lodGates.end()) {
				std::erase(gate->second.incoming, a_key);
				std::erase(gate->second.outgoing, a_key);
			}
			if (at->second.side == kGateIncoming)
				mirror.SetGated(a_key, false);
			gateOfRoot.erase(at);
			++gateStats.rootsDetached;
		};
		leave(a_root);
		for (const void* node : a_nodes)
			leave(node);
	}

	bool Coordinator::GateWithholds(std::uint32_t a_slot) const
	{
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		if (!geometry)
			return false;
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(geometry));
		if (it == tracked.end())
			return false;
		if (it->second.gateHeldOut)
			return true;
		if (it->second.gateSide != kGateIncoming)
			return false;
		const auto gate = lodGates.find(it->second.gate);
		return gate != lodGates.end() && gate->second.state == LodGate::State::Open;
	}

	std::uint8_t Coordinator::GateWithhold(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, std::uint32_t a_drawn, bool a_live)
	{
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		const auto it = geometry ? tracked.find(const_cast<RE::BSGeometry*>(geometry)) : tracked.end();
		if (it == tracked.end())
			return a_phases;
		const auto& entry = it->second;
		if (entry.gateHeldOut)
			return 0;
		if (!entry.gate || entry.gateSide != kGateIncoming)
			return a_phases;
		// The slot's phases as the commit decided them, and whether whole (every phase it takes part in, not waiting): what its flip applies
		// and what the gate's readiness counts (UpdateGates).
		if (gateSlots.size() < tables.objects.size())
			gateSlots.resize(tables.objects.size());
		auto& held = gateSlots[a_slot];
		held.geometry = geometry;
		held.phases = a_phases;
		held.ready = !a_wait && a_phases == (a_live ? SetParticipation(a_slot, a_drawn) : std::uint8_t{ 0 });
		const auto gate = lodGates.find(entry.gate);
		return gate != lodGates.end() && gate->second.state == LodGate::State::Open ? std::uint8_t{ 0 } : a_phases;
	}

	bool Coordinator::GateGeometryReady(const RE::BSGeometry* a_geometry, const Tracked& a_entry) const
	{
		// Classified since it was tagged (TagGate took its verdict again), with its leaf's records.
		if (a_entry.leafWaiting || a_entry.candidateFrame == 0)
			return false;
		// No record: ineligible for a recorded verdict (a hide still counts here when it is not the gate's: HiddenForWalk), or not written yet.
		if (a_entry.slot == kNoObjectSlot && a_entry.layerSlot == kNoObjectSlot)
			return a_entry.candidateReason != Ineligible::None;
		// Each slot of it whole at its last evaluation by the commit (GateWithhold).
		for (const std::uint32_t slot : { a_entry.slot, a_entry.layerSlot })
			if (slot != kNoObjectSlot && (slot >= gateSlots.size() || gateSlots[slot].geometry != a_geometry || !gateSlots[slot].ready))
				return false;
		return true;
	}

	void Coordinator::UpdateGates(bool a_live, std::vector<std::pair<std::uint32_t, std::uint8_t>>& a_apply)
	{
		// The slots whose structure changed since the last publication's revocation (revokeCursor's unread kStructureCauses: a record
		// written or rewritten since). The publication this commit makes takes their claims back whole (RevokeUndrawnClaims: the shapes
		// were made without the change) and claims them only at the next one: a gate flipped on one would be released with its incoming
		// unclaimed (every incoming geometry of a gate flipped in the pass that wrote its records: <- GATE HOLE). Made once, when a gate is
		// otherwise ready to flip.
		bool structureTaken = false;
		ankerl::unordered_dense::set<std::uint32_t> structureUnpublished;
		auto unpublished = [&](std::uint32_t a_slot) {
			if (!structureTaken) {
				structureTaken = true;
				if (!IndirectDraws::Get().Failed() && revokeCursor.Continues(tables.changeLog, tablesGeneration))
					for (const auto& change : revokeCursor.Unread(tables.changeLog))
						if (change.causes & kStructureCauses)
							structureUnpublished.insert(change.slot);
			}
			return a_slot != kNoObjectSlot && structureUnpublished.contains(a_slot);
		};
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
				if (const auto entry = tracked.find(geometry); entry == tracked.end() || !GateGeometryReady(geometry, entry->second))
					++gate.pending;
			if (!a_live || gate.pending || gate.unmirrored || gate.incoming.empty())
				continue;
			// Ready, but an incoming record not published yet: the flip waits for the commit after that publication (the change log's
			// notes queue the slot there), so the flip's publication claims every incoming geometry.
			std::uint32_t unpublishedRecords = 0;
			for (auto* geometry : gate.incomingGeometries)
				if (const auto entry = tracked.find(geometry); entry != tracked.end() && (unpublished(entry->second.slot) || unpublished(entry->second.layerSlot)))
					++unpublishedRecords;
			if (unpublishedRecords) {
				gate.pending = unpublishedRecords;
				++gateStats.structureDeferred;
				continue;
			}
			// The flip: the incoming slots' held phases and the outgoing slots' none, in this commit; the publication carries it.
			GateFlip flip;
			flip.token = token;
			flip.requestNs = gate.stampNs;
			flip.requestFrame = gate.stampFrame;
			gate.flipNs = GateNowNs();
			gate.flipFrame = SceneCapture::Frame();
			flip.flipNs = gate.flipNs;
			flip.flipFrame = gate.flipFrame;
			for (auto* geometry : gate.incomingGeometries) {
				const auto found = tracked.find(geometry);
				if (found == tracked.end())
					continue;
				const auto& entry = found->second;
				for (const std::uint32_t slot : { entry.slot, entry.layerSlot })
					if (slot != kNoObjectSlot && slot < gateSlots.size() && gateSlots[slot].geometry == geometry)
						a_apply.emplace_back(slot, gateSlots[slot].phases);
				// The claims are by base geometry (a layer is claimed with its base).
				if (entry.slot != kNoObjectSlot && entry.slot < gateSlots.size() && gateSlots[entry.slot].geometry == geometry && gateSlots[entry.slot].phases &&
					!tables.IsLayer(entry.slot))
					flip.incoming.emplace_back(geometry, gateSlots[entry.slot].phases);
			}
			for (auto* geometry : gate.outgoingGeometries) {
				const auto found = tracked.find(geometry);
				if (found == tracked.end())
					continue;
				auto& entry = found->second;
				// Held out of the set until its detach (the engine side detaches it at a LOD drain after the flip's release).
				if (!entry.gateHeldOut) {
					entry.gateHeldOut = true;
					++gateHeldOutCount;
					++gateStats.heldOut;
				}
				for (const std::uint32_t slot : { entry.slot, entry.layerSlot })
					if (slot != kNoObjectSlot)
						a_apply.emplace_back(slot, std::uint8_t{ 0 });
				flip.outgoing.push_back(geometry);
			}
			gate.state = LodGate::State::Flipped;
			++gateStats.flipped;
			gateStats.requestToFlipMs.push_back(GateMs(gate.stampNs, gate.flipNs));
			gateStats.requestToFlipFrames.push_back(gate.flipFrame >= gate.stampFrame ? gate.flipFrame - gate.stampFrame : 0u);
			gateFlipsMade.push_back(std::move(flip));
		}
	}

	void Coordinator::TakeGateOutcomes()
	{
		// Before the tracker's stack is drained (CollectEvents): every outcome taken here was posted after its gate's events were pushed.
		if (!LodGates::Enabled() && lodGates.empty())
			return;
		LodGates::DrainOutcomes([this](const LodGates::Outcome& a_outcome) { gateOutcomesHeld.push_back(GateOutcome{ a_outcome.token, a_outcome.forced, a_outcome.kept }); });
	}

	void Coordinator::ApplyGateOutcomes(bool a_applied)
	{
		// A load screen's pass carried its events for the mirror (its gate events among them): the outcomes wait for the pass that applies them.
		if (!a_applied || gateOutcomesHeld.empty())
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
		// Its roots ungated (flipped: the engine's show came as the root's hidden event, applied before this outcome), off the root map.
		auto release = [&](const void* a_root, bool a_incoming) {
			if (const auto at = gateOfRoot.find(a_root); at != gateOfRoot.end() && at->second.token == a_token)
				gateOfRoot.erase(at);
			if (a_incoming)
				mirror.SetGated(a_root, false);
		};
		for (const void* root : gate.incoming)
			release(root, true);
		for (const void* root : gate.outgoing)
			release(root, false);
		for (auto* geometry : gate.incomingGeometries) {
			const auto entry = tracked.find(geometry);
			if (entry == tracked.end())
				continue;
			entry->second.gate = 0;
			entry->second.gateSide = 0;
			if (open) {
				// Forced before its flip: the block is the engine's as it shows it (or detaches it), so its verdict is taken again without the
				// gate and its slots committed with their own phases.
				entry->second.candidateFrame = 0;
				pendingEvaluation.push_back(geometry);
				for (const std::uint32_t slot : { entry->second.slot, entry->second.layerSlot })
					if (slot != kNoObjectSlot)
						gateQueued.push_back(slot);
			}
		}
		// The outgoing: untagged; held out still when the gate flipped (their detach erases them).
		for (auto* geometry : gate.outgoingGeometries)
			if (const auto entry = tracked.find(geometry); entry != tracked.end()) {
				entry->second.gate = 0;
				entry->second.gateSide = 0;
			}
		if (a_forced) {
			++gateStats.retiredForced;
			gateStats.forcedOpen += open ? 1u : 0u;
		} else {
			++gateStats.retiredFlipped;
		}
		lodGates.erase(it);
	}

	void Coordinator::ApplyGateKept(std::uint64_t a_token, const std::array<const void*, 2>& a_roots)
	{
		// The engine reused a held outgoing block: it stays attached as its node's current block, drawn by the engine while DCLF holds it
		// out, and no detach will ever erase its entries. Its gate may still be open (reattached before the flip: the flip must not hold it
		// out), flipped, or retired already (the flip's outcome came first: the hold-out stays on entries with no gate).
		++gateStats.keptNotices;
		const auto gate = lodGates.find(a_token);
		std::vector<std::pair<RE::BSGeometry*, Ineligible>> found;
		for (const void* root : a_roots) {
			if (!root)
				continue;
			if (const auto at = gateOfRoot.find(root); at != gateOfRoot.end() && at->second.token == a_token && at->second.side == kGateOutgoing) {
				gateOfRoot.erase(at);
				if (gate != lodGates.end())
					std::erase(gate->second.outgoing, root);
			}
			// One walk of the mirror per notice (the gate's own tag walk, TagGateRoot): a root the mirror lost was detached, its entries erased.
			found.clear();
			bool lacking = false;
			MirrorSubtree(root, Ineligible::None, found, lacking);
			for (const auto& [geometry, reason] : found) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end())
					continue;
				auto& entry = it->second;
				// This gate's outgoing tag, or a hold-out its retirement left (no gate tags it); a later gate's tag is that gate's.
				const bool ours = entry.gate == a_token && entry.gateSide == kGateOutgoing;
				if (!ours && !(entry.gate == 0 && entry.gateHeldOut))
					continue;
				const bool heldOut = entry.gateHeldOut;
				UntagGate(it->first, entry);
				if (!heldOut) {
					++gateStats.keptUntagged;
					continue;
				}
				// Held out by the flip (0 applied to its slots): committed again with its own phases.
				++gateStats.keptCleared;
				for (const std::uint32_t slot : { entry.slot, entry.layerSlot })
					if (slot != kNoObjectSlot)
						gateQueued.push_back(slot);
			}
		}
	}

}
