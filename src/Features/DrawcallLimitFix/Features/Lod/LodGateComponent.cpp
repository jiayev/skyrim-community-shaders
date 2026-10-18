// The LOD gates' scene side (LodGates.h has the engine side; dclf-architecture.md, "LOD gates: the scene side"): the
// component's gates, opened by their events, the commit's withholding and flips, and the outcomes that retire them (drained from the
// engine side: LodGates::DrainOutcomes). Scene lane; the coordinator only through the hooks' context.

#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"

#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Engine/FrameClock.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodGates.h"

#include <algorithm>

namespace DCLF::Features::Lod::Gates
{
	namespace
	{
		using Context = Api::CoordinatorComponentContext;

		/** @brief Off its gate's tag (the component's forgot, a retag, a kept notice); its veto is the caller's. */
		void UntagGate(GateState& a_state, const RE::BSGeometry* a_geometry)
		{
			auto& gateTags = a_state.gateTags;
			const auto tag = gateTags.find(a_geometry);
			if (tag == gateTags.end())
				return;
			if (const auto gate = a_state.lodGates.find(tag->second.token); gate != a_state.lodGates.end())
				(tag->second.side == kGateIncoming ? gate->second.incomingGeometries : gate->second.outgoingGeometries).erase(const_cast<RE::BSGeometry*>(a_geometry));
			gateTags.erase(tag);
		}

		/** @brief Tags a tracked a_geometry on a_token's a_side (its veto: Observe while an open gate's incoming, else none). */
		void TagGate(const Context& a_context, GateState& a_state, const RE::BSGeometry* a_geometry, std::uint64_t a_token, std::uint8_t a_side)
		{
			auto& gateTags = a_state.gateTags;
			auto& gateStats = a_state.gateStats;
			const auto tag = gateTags.find(a_geometry);
			if (tag != gateTags.end() && tag->second.token == a_token && tag->second.side == a_side)
				return;
			const auto gate = a_state.lodGates.find(a_token);
			if (gate == a_state.lodGates.end())
				return;
			if (tag != gateTags.end())
				UntagGate(a_state, a_geometry);
			gateTags[a_geometry] = GateTag{ a_token, a_side };
			auto* geometry = const_cast<RE::BSGeometry*>(a_geometry);
			if (a_side == kGateIncoming) {
				gate->second.incomingGeometries.insert(geometry);
				++gateStats.taggedIncoming;
				// Withheld while its gate is open, its slots' readiness recorded (Observe: ready only by an evaluation under this gate, an earlier
				// gate's record of the slot is not this one's); a hold-out it had goes. Classified again with its root gated (a verdict taken while
				// the gate's hide counted stands no more; AddGeometry's own is pending already), and its slots committed again by the next commit.
				a_context.SetEntryVeto(a_geometry, gate->second.state == LodGate::State::Open ? Api::kVetoObserve : std::uint8_t{ 0 });
				a_context.Requeue(a_geometry, std::uint8_t(Api::kRequeueReevaluate | Api::kRequeueSlots));
			} else {
				gate->second.outgoingGeometries.insert(geometry);
				++gateStats.taggedOutgoing;
				// A hold-out it had goes (its gate's flip holds it out again).
				a_context.SetEntryVeto(a_geometry, 0);
			}
		}

		/** @brief Tags the tracked geometries under a_root on a_token's a_side; true when the mirror lacked a record there. */
		bool TagGateRoot(const Context& a_context, GateState& a_state, std::uint64_t a_token, const void* a_root, std::uint8_t a_side)
		{
			// One walk of the mirror per root (not per pass): the subtree as the tracking walks it (SubtreeEntries), each tracked geometry tagged.
			auto& entries = a_state.gateEntries;
			entries.clear();
			bool lacking = false;
			a_context.SubtreeEntries(a_root, entries, lacking);
			for (const void* geometry : entries)
				TagGate(a_context, a_state, static_cast<const RE::BSGeometry*>(geometry), a_token, a_side);
			return lacking;
		}

		/** @brief Whether a gated incoming geometry is ready (GeometryReady, by the coordinator's records). */
		bool GateGeometryReady(const Context& a_context, const RE::BSGeometry* a_geometry)
		{
			return GeometryReady(a_geometry, a_context.TrackedStateOf(a_geometry), [&](std::uint32_t a_slot) { return a_context.ObservedSlot(a_slot); });
		}

		/** @brief A gate event opens its gate: token, roots (keys), push stamp. */
		void OpenGate(const Context& a_context, GateState& a_state, std::uint64_t a_token, std::span<const void* const> a_incoming, std::span<const void* const> a_outgoing,
			std::uint64_t a_stampNs, std::uint32_t a_stampFrame)
		{
			auto& lodGates = a_state.lodGates;
			auto& gateStats = a_state.gateStats;
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
			// The commit runs live (the gates settle) while any gate stands.
			a_context.SetCommitLive(true);
			auto take = [&](const void* a_root, std::uint8_t a_side) {
				if (const auto displaced = a_context.MarkRoot(a_root, a_token, a_side, a_side == kGateIncoming)) {
					if (displaced.component.index == component.index)
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
				// The tracked geometries under it (its attach applied before this event: tracked by an earlier pass, or by this batch's adds
				// after the mirror's events, which AddGeometry tags); a root or node the mirror lacks a record of holds the gate until it has one.
				if (TagGateRoot(a_context, a_state, a_token, root, kGateIncoming))
					++lodGates.find(a_token)->second.unmirrored;
			}
			for (const void* root : a_outgoing) {
				if (!root)
					continue;
				take(root, kGateOutgoing);
				lodGates.find(a_token)->second.outgoing.push_back(root);
				TagGateRoot(a_context, a_state, a_token, root, kGateOutgoing);
			}
			if (lodGates.find(a_token)->second.incoming.empty())
				++gateStats.emptyIncoming;
		}

		/** @brief Retires a_token's gate at its outcome (a_forced: released without a flip). */
		void RetireGate(const Context& a_context, GateState& a_state, std::uint64_t a_token, bool a_forced)
		{
			auto& lodGates = a_state.lodGates;
			auto& gateStats = a_state.gateStats;
			const auto it = lodGates.find(a_token);
			if (it == lodGates.end())
				return;
			auto& gate = it->second;
			const bool open = gate.state == LodGate::State::Open;
			// Its roots unmarked, the incoming ungated (flipped: the engine's show came as the root's hidden event, applied before this outcome).
			// A root listed is marked this gate's (a later gate's mark, or a detach, took it off the list).
			for (const void* root : gate.incoming)
				a_context.UnmarkRoot(root, a_token);
			for (const void* root : gate.outgoing)
				a_context.UnmarkRoot(root, a_token);
			for (auto* geometry : gate.incomingGeometries) {
				if (!a_context.TrackedStateOf(geometry).tracked)
					continue;
				a_state.gateTags.erase(geometry);
				if (open) {
					// Forced before its flip: the block is the engine's as it shows it (or detaches it), so it is withheld no more, its verdict is
					// taken again without the gate and its slots committed with their own phases.
					a_context.SetEntryVeto(geometry, 0);
					a_context.Requeue(geometry, std::uint8_t(Api::kRequeueReevaluateAlways | Api::kRequeueSlots));
				}
			}
			// The outgoing: untagged; held out still when the gate flipped (their detach erases them).
			for (auto* geometry : gate.outgoingGeometries)
				if (a_context.TrackedStateOf(geometry).tracked)
					a_state.gateTags.erase(geometry);
			if (a_forced) {
				++gateStats.retiredForced;
				gateStats.forcedOpen += open ? 1u : 0u;
			} else {
				++gateStats.retiredFlipped;
			}
			lodGates.erase(it);
			a_context.SetCommitLive(!lodGates.empty());
		}

		/**
		 * @brief The engine reused a_token's held block under a_roots: those roots and their geometries leave the gate's outgoing side and
		 * hold-out (requeued).
		 */
		void ApplyGateKept(const Context& a_context, GateState& a_state, std::uint64_t a_token, const std::array<const void*, 2>& a_roots)
		{
			// The engine reused a held outgoing block: it stays attached as its node's current block, drawn by the engine while DCLF holds it
			// out, and no detach will ever erase its entries. Its gate may still be open (reattached before the flip: the flip must not hold it
			// out), flipped, or retired already (the flip's outcome came first: the hold-out stays on entries with no gate).
			auto& gateTags = a_state.gateTags;
			auto& gateStats = a_state.gateStats;
			++gateStats.keptNotices;
			const auto gate = a_state.lodGates.find(a_token);
			for (const void* root : a_roots) {
				if (!root)
					continue;
				if (const auto mark = a_context.MarkOf(root); mark.component.index == component.index && mark.token == a_token && mark.side == kGateOutgoing) {
					a_context.UnmarkRoot(root, a_token);
					if (gate != a_state.lodGates.end())
						std::erase(gate->second.outgoing, root);
				}
				// One walk of the mirror per notice (the gate's own tag walk, TagGateRoot): a root the mirror lost was detached, its entries erased.
				auto& entries = a_state.gateEntries;
				entries.clear();
				bool lacking = false;
				a_context.SubtreeEntries(root, entries, lacking);
				for (const void* key : entries) {
					const auto* geometry = static_cast<const RE::BSGeometry*>(key);
					const auto tag = gateTags.find(geometry);
					// This gate's outgoing tag, or a hold-out its retirement left (no gate tags it); a later gate's tag is that gate's.
					const bool ours = tag != gateTags.end() && tag->second.token == a_token && tag->second.side == kGateOutgoing;
					const bool heldOut = a_context.TrackedStateOf(geometry).veto == Api::kVetoHold;
					if (!ours && !(tag == gateTags.end() && heldOut))
						continue;
					UntagGate(a_state, geometry);
					if (!heldOut) {
						++gateStats.keptUntagged;
						continue;
					}
					// Held out by the flip (0 applied to its slots): no more, committed again with its own phases.
					a_context.SetEntryVeto(geometry, 0);
					++gateStats.keptCleared;
					a_context.Requeue(geometry, Api::kRequeueSlots);
				}
			}
		}
	}

	std::shared_ptr<void> MakeState() { return std::make_shared<GateState>(); }

	void ApplyGateEvent(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, const Api::Stacked& a_event)
	{
		// Its incoming roots pinned, its outgoing named.
		if (a_channel == channel)
			OpenGate(a_context, a_context.State<GateState>(), a_event.token, a_event.pinned, a_event.named, a_event.stampNs, a_event.stampFrame);
	}

	void RootDetached(const Api::CoordinatorComponentContext& a_context, const void* a_key, const Api::Mark& a_mark)
	{
		// A root detached leaves its gate (its mark erased, its hide ungated: UnmarkDetached; its geometries leave with their own erasure:
		// the forgot hook untags them). An open gate left without an incoming root never flips (its outgoing would be hidden over nothing):
		// the engine side's forced release retires it.
		auto& state = a_context.State<GateState>();
		if (const auto gate = state.lodGates.find(a_mark.token); gate != state.lodGates.end()) {
			std::erase(gate->second.incoming, a_key);
			std::erase(gate->second.outgoing, a_key);
		}
		++state.gateStats.rootsDetached;
	}

	void Rescan(const Api::CoordinatorComponentContext& a_context)
	{
		for (auto& [token, gate] : a_context.State<GateState>().lodGates) {
			gate.incomingGeometries.clear();
			gate.outgoingGeometries.clear();
			gate.pending = 0;
		}
	}

	void UnderMark(const Api::CoordinatorComponentContext& a_context, const void* a_geometry, const Api::Mark& a_mark)
	{
		if (a_context.TrackedStateOf(a_geometry).tracked)
			TagGate(a_context, a_context.State<GateState>(), static_cast<const RE::BSGeometry*>(a_geometry), a_mark.token, a_mark.side);
	}

	void Settle(const Api::CoordinatorComponentContext& a_context, bool a_live)
	{
		auto& state = a_context.State<GateState>();
		auto& gateStats = state.gateStats;
		for (auto& [token, gate] : state.lodGates) {
			if (gate.state != LodGate::State::Open)
				continue;
			// Roots the mirror lacked a record of: walked again (a capture since; a subtree tracked since is tagged on the way).
			if (gate.unmirrored) {
				std::uint32_t unmirrored = 0;
				for (const void* root : gate.incoming)
					unmirrored += TagGateRoot(a_context, state, token, root, kGateIncoming) ? 1u : 0u;
				gate.unmirrored = unmirrored;
			}
			gate.pending = 0;
			for (auto* geometry : gate.incomingGeometries)
				if (!GateGeometryReady(a_context, geometry))
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
				if (const auto entry = a_context.TrackedStateOf(geometry);
					entry.tracked && (a_context.RestructuredSincePublication(entry.slot) || a_context.RestructuredSincePublication(entry.layerSlot)))
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
			gate.flipFrame = FrameClock::Frame();
			flip.flipNs = gate.flipNs;
			flip.flipFrame = gate.flipFrame;
			for (auto* geometry : gate.incomingGeometries) {
				const auto entry = a_context.TrackedStateOf(geometry);
				if (!entry.tracked)
					continue;
				// Its Observe dropped: each slot the commit last evaluated for it takes the phases recorded then.
				a_context.SetEntryVeto(geometry, 0);
				// The claims are by base geometry (a layer is claimed with its base).
				if (entry.slot != kNoSlot)
					if (const auto observed = a_context.ObservedSlot(entry.slot); observed.geometry == geometry && observed.phases && !a_context.LayerSlot(entry.slot))
						flip.incoming.emplace_back(geometry, observed.phases);
			}
			for (auto* geometry : gate.outgoingGeometries) {
				const auto entry = a_context.TrackedStateOf(geometry);
				if (!entry.tracked)
					continue;
				// Held out of the set until its detach (the engine side detaches it at a LOD drain after the flip's release): its slots none.
				if (entry.veto != Api::kVetoHold)
					++gateStats.heldOut;
				a_context.SetEntryVeto(geometry, Api::kVetoHold);
				flip.outgoing.push_back(geometry);
			}
			gate.state = LodGate::State::Flipped;
			++gateStats.flipped;
			gateStats.requestToFlipMs.push_back(GateMs(gate.stampNs, gate.flipNs));
			gateStats.requestToFlipFrames.push_back(gate.flipFrame >= gate.stampFrame ? gate.flipFrame - gate.stampFrame : 0u);
			// Published by this commit's publication, as the gates' component's delta (released by the frame installing it: Adopt).
			a_context.PublishDelta(std::make_shared<const GateFlip>(std::move(flip)));
		}
	}

	void Take(const Api::CoordinatorComponentContext& a_context)
	{
		// Before the tracker's stack is drained (CollectEvents): every outcome taken here was posted after its gate's events were pushed.
		auto& state = a_context.State<GateState>();
		if (!LodGates::Enabled() && state.lodGates.empty())
			return;
		auto& held = state.gateOutcomesHeld;
		LodGates::DrainOutcomes([&held](const LodGates::Outcome& a_outcome) { held.push_back(GateOutcome{ a_outcome.token, a_outcome.forced, a_outcome.kept }); });
	}

	void ApplyOutcomes(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, std::span<const void* const>)
	{
		if (a_channel != channel)
			return;
		// Only by a pass that applies a batch (the channel's AfterBatch keys): a load screen's pass carried its events for the mirror (its gate
		// events among them), and the outcomes wait for the pass that applies them.
		auto& state = a_context.State<GateState>();
		if (state.gateOutcomesHeld.empty())
			return;
		for (const auto& outcome : state.gateOutcomesHeld) {
			// A kept notice (in order with the retirements: its gate's flip outcome may come before it or after it).
			if (outcome.kept[0]) {
				ApplyGateKept(a_context, state, outcome.token, outcome.kept);
				continue;
			}
			if (!state.lodGates.contains(outcome.token)) {
				++state.gateStats.unknownOutcomes;
				continue;
			}
			RetireGate(a_context, state, outcome.token, outcome.forced);
		}
		state.gateOutcomesHeld.clear();
	}

	void Forgot(const Api::CoordinatorComponentContext& a_context, const void* a_entry)
	{
		UntagGate(a_context.State<GateState>(), static_cast<const RE::BSGeometry*>(a_entry));
	}

	void ForgotAll(const Api::CoordinatorComponentContext& a_context)
	{
		a_context.State<GateState>().gateTags.clear();
	}
}
