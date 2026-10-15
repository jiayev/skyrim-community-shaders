// CS_DCLF_PERSISTENT_PARITY: the LOD gates' state (Scene/Coordinator/Gates.cpp) against what it is derived from, at each commit while gates
// live (SetCommitted: after UpdateGates, before the publication takes the commit's flips): the root map and the mirror's gated roots, the
// tags, the withheld slots, the open gates' readiness taken again, the commit's flips. Every 60th pass a sweep of the tracked set: the tags
// and the hold-outs, the marks a walk of the mirror gives now, and the open gates' incoming roots hidden. Gate roots are keys: nothing
// engine is dereferenced. Reported with the gates' line (GATE PARITY).

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;
		using Parity = GateObserverState::Parity;

		/** @brief The commit checked: its store and decision inputs (SetCommitted's), and the counts. */
		struct Commit
		{
			const Scene::Coordinator& store;
			bool live;
			std::uint32_t drawn;
			Parity& parity;
		};

		/** @brief A difference: counted, the first described (a_describe: its text, the invariant's tag first). */
		template <class Describe>
		void Differ(Parity& a_parity, Describe&& a_describe)
		{
			if (a_parity.differ++ == 0)
				a_parity.first = a_describe();
		}

		const char* SideName(std::uint8_t a_side)
		{
			return a_side == V::kGateIncoming ? "incoming" : a_side == V::kGateOutgoing ? "outgoing" : "no side";
		}

		/**
		 * @brief A slot whole now, as GateWithhold records it: its decision taken again (DecideSlot), its layer partner's main phase paired
		 * as the commit pairs them, not waiting, every phase it takes part in.
		 */
		bool SlotWholeNow(const Commit& a_commit, std::uint32_t a_slot)
		{
			const auto& tables = V::tables(a_commit.store);
			const std::size_t objects = tables.objects.size();
			// The commit evaluates no slot past the tables.
			if (a_slot >= objects)
				return false;
			const auto decision = V::DecideSlot(a_commit.store, a_slot, a_commit.live, a_commit.drawn);
			std::uint8_t phases = decision.phases;
			const std::uint32_t partner = tables.IsLayer(a_slot) ? tables.layerBase[a_slot] : a_slot < tables.layerOf.size() ? tables.layerOf[a_slot] : kNoObjectSlot;
			if (partner != kNoObjectSlot && partner < objects && !(phases & kSetMain) != !(V::DecideSlot(a_commit.store, partner, a_commit.live, a_commit.drawn).phases & kSetMain))
				phases = 0;
			return !decision.wait && phases == (a_commit.live ? V::SetParticipation(a_commit.store, a_slot, a_commit.drawn) : std::uint8_t{ 0 });
		}

		/**
		 * @brief GateGeometryReady with each slot's readiness decided now instead of as the commit recorded it. A slot not evaluated under
		 * its gate yet (no record of the geometry: TagGate cleared it) is not ready either way: the gate waits for that evaluation.
		 */
		bool ReadyNow(const Commit& a_commit, const RE::BSGeometry* a_geometry, const V::Tracked& a_entry)
		{
			if (a_entry.leafWaiting || a_entry.candidateFrame == 0)
				return false;
			if (a_entry.slot == kNoObjectSlot && a_entry.layerSlot == kNoObjectSlot)
				return a_entry.candidateReason != Ineligible::None;
			const auto& gateSlots = V::gateSlots(a_commit.store);
			for (const std::uint32_t slot : { a_entry.slot, a_entry.layerSlot })
				if (slot != kNoObjectSlot && (slot >= gateSlots.size() || gateSlots[slot].geometry != a_geometry || !SlotWholeNow(a_commit, slot)))
					return false;
			return true;
		}

		/**
		 * @brief G3: a withheld geometry's slots out of the commit's decision and its base geometry out of the next claims. a_open: an open
		 * gate's incoming, whose slots are withheld from their first evaluation under the gate (one tagged since, by UpdateGates' walk, is
		 * the next commit's); else held out, whose slots the flip applied 0 to and every evaluation since withholds.
		 */
		void CheckWithheld(const Commit& a_commit, const RE::BSGeometry* a_geometry, const V::Tracked& a_entry, bool a_open)
		{
			const auto& setPhases = V::setPhasesNext(a_commit.store);
			const auto& gateSlots = V::gateSlots(a_commit.store);
			bool base = false;
			for (const std::uint32_t slot : { a_entry.slot, a_entry.layerSlot }) {
				if (slot == kNoObjectSlot || (a_open && (slot >= gateSlots.size() || gateSlots[slot].geometry != a_geometry)))
					continue;
				base = base || slot == a_entry.slot;
				if (slot < setPhases.size() && setPhases[slot])
					Differ(a_commit.parity, [&] {
						return fmt::format("withheld: geometry {} ({} of gate {}) slot {} holds {:#x} in the commit", static_cast<const void*>(a_geometry),
							a_open ? "incoming" : "held out", a_entry.gate, slot, setPhases[slot]);
					});
			}
			if (const auto* building = V::setBuilding(a_commit.store); base && building)
				if (const auto it = building->phases.find(a_geometry); it != building->phases.end())
					Differ(a_commit.parity, [&] {
						return fmt::format("withheld: geometry {} ({} of gate {}) claimed {:#x} by the next publication", static_cast<const void*>(a_geometry),
							a_open ? "incoming" : "held out", a_entry.gate, it->second);
					});
		}

		/** @brief G1: the gates' root lists, the root map and the mirror's gated roots agree. */
		void CheckRoots(const Commit& a_commit)
		{
			const auto& lodGates = V::lodGates(a_commit.store);
			const auto& gateOfRoot = V::gateOfRoot(a_commit.store);
			const auto& mirror = V::mirror(a_commit.store);
			for (const auto& [token, gate] : lodGates)
				for (const std::uint8_t side : { V::kGateIncoming, V::kGateOutgoing })
					for (const void* root : side == V::kGateIncoming ? gate.incoming : gate.outgoing)
						if (const auto at = gateOfRoot.find(root); at == gateOfRoot.end() || at->second.token != token || at->second.side != side)
							Differ(a_commit.parity, [&] {
								return at == gateOfRoot.end() ? fmt::format("roots: gate {} lists {} root {}, the root map has none", token, SideName(side), root) :
								                                fmt::format("roots: gate {} lists {} root {}, the root map names gate {} ({})", token, SideName(side), root,
																	at->second.token, SideName(at->second.side));
							});
			std::size_t incoming = 0;
			for (const auto& [root, owner] : gateOfRoot) {
				const auto gate = lodGates.find(owner.token);
				const auto* roots = gate == lodGates.end()          ? nullptr :
				                    owner.side == V::kGateIncoming ? &gate->second.incoming :
				                    owner.side == V::kGateOutgoing ? &gate->second.outgoing :
				                                                     nullptr;
				if (!roots || std::ranges::find(*roots, root) == roots->end())
					Differ(a_commit.parity, [&] {
						return fmt::format("roots: root {} mapped to gate {} ({}), {}", root, owner.token, SideName(owner.side), gate == lodGates.end() ? "no such gate" : "not listed by it");
					});
				incoming += owner.side == V::kGateIncoming ? 1u : 0u;
				if (mirror.Gated(root) != (owner.side == V::kGateIncoming))
					Differ(a_commit.parity, [&] { return fmt::format("roots: root {} of gate {} ({}) gated {} in the mirror", root, owner.token, SideName(owner.side), mirror.Gated(root)); });
			}
			if (mirror.GatedCount() != incoming)
				Differ(a_commit.parity, [&] { return fmt::format("roots: the mirror gates {} roots, the root map has {} incoming", mirror.GatedCount(), incoming); });
		}

		/** @brief G2 (a gate's sets): each tagged geometry tracked with the gate's token and side. G3 for its held-out outgoing. */
		void CheckTags(const Commit& a_commit, std::uint64_t a_token, const V::LodGate& a_gate)
		{
			const auto& tracked = V::tracked(a_commit.store);
			for (const std::uint8_t side : { V::kGateIncoming, V::kGateOutgoing })
				for (auto* geometry : side == V::kGateIncoming ? a_gate.incomingGeometries : a_gate.outgoingGeometries) {
					const auto it = tracked.find(geometry);
					if (it == tracked.end() || it->second.gate != a_token || it->second.gateSide != side) {
						Differ(a_commit.parity, [&] {
							return it == tracked.end() ? fmt::format("tags: gate {} {} geometry {} not tracked", a_token, SideName(side), static_cast<const void*>(geometry)) :
							                             fmt::format("tags: gate {} {} geometry {} tagged gate {} ({})", a_token, SideName(side), static_cast<const void*>(geometry),
															 it->second.gate, SideName(it->second.gateSide));
						});
						continue;
					}
					if (it->second.gateHeldOut)
						CheckWithheld(a_commit, geometry, it->second, false);
				}
		}

		/**
		 * @brief G3 and G4 for an open gate: its incoming withheld, each incoming geometry's readiness as recorded against a fresh decision,
		 * the not-ready count against pending, and a gate UpdateGates should have flipped.
		 */
		void CheckOpen(const Commit& a_commit, std::uint64_t a_token, const V::LodGate& a_gate, bool a_retagged)
		{
			const auto& store = a_commit.store;
			const auto& tracked = V::tracked(store);
			std::uint32_t recorded = 0, fresh = 0;
			const RE::BSGeometry* differs = nullptr;
			bool differsRecorded = false;
			for (auto* geometry : a_gate.incomingGeometries) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end()) {
					++recorded;
					++fresh;
					continue;
				}
				CheckWithheld(a_commit, geometry, it->second, true);
				const bool wasReady = V::GateGeometryReady(store, geometry, it->second);
				const bool readyNow = ReadyNow(a_commit, geometry, it->second);
				recorded += wasReady ? 0u : 1u;
				fresh += readyNow ? 0u : 1u;
				if (wasReady != readyNow && !differs) {
					differs = geometry;
					differsRecorded = wasReady;
				}
			}
			// Nothing pending, every root mirrored, roots left and the scene built: UpdateGates flips such a gate, or defers it with its
			// unpublished records as pending.
			const bool flippable = a_commit.live && !a_gate.unmirrored && !a_gate.incoming.empty();
			if (flippable && !a_gate.pending)
				Differ(a_commit.parity, [&] { return fmt::format("stuck: gate {} open with nothing pending ({} incoming geometries, {} roots)", a_token, a_gate.incomingGeometries.size(), a_gate.incoming.size()); });
			// The count: not for a gate the structure check deferred (every geometry ready by the commit's records; pending then counts the
			// records not published yet), nor after a tag in UpdateGates' walks (an unmirrored gate's walk may take a geometry of a gate
			// counted before it; each such tag of a geometry with slots queues them, and gateQueued is empty after the commit's start
			// otherwise). A geometry with neither slots nor a verdict queues nothing: taking it needs a root nested under another gate's,
			// which the marks (G6) flag.
			const bool counted = !(flippable && !recorded) && !a_retagged;
			if (differs || (counted && fresh != a_gate.pending))
				Differ(a_commit.parity, [&] {
					auto text = fmt::format("ready: gate {}: {} incoming geometries not ready by a fresh decision, {} by the commit's records, {} pending{}", a_token, fresh, recorded,
						a_gate.pending, counted ? "" : " (not compared)");
					if (differs)
						text += fmt::format("; geometry {} {} as recorded, {} now", static_cast<const void*>(differs), differsRecorded ? "ready" : "not ready", differsRecorded ? "not ready" : "ready");
					return text;
				});
		}

		/**
		 * @brief G5 and G4's early flips, for the flips this commit made: incoming the base geometries the commit holds with phases (the flip
		 * applied them), each ready by a fresh decision; outgoing the gate's tracked outgoing. Not compared after a tag in UpdateGates' walks
		 * (CheckOpen): a later gate's walk may take a flipped gate's geometry after its flip.
		 */
		void CheckFlips(const Commit& a_commit, bool a_retagged)
		{
			const auto& store = a_commit.store;
			const auto& lodGates = V::lodGates(store);
			const auto& tracked = V::tracked(store);
			const auto& tables = V::tables(store);
			const auto& setPhases = V::setPhasesNext(store);
			const auto& flips = V::gateFlipsMade(store);
			auto& p = a_commit.parity;
			// This commit's: those after the newest an earlier commit checked (the publication empties the list).
			std::size_t from = 0;
			if (p.lastFlip)
				for (std::size_t i = flips.size(); i > 0; --i)
					if (flips[i - 1].token == p.lastFlip) {
						from = i;
						break;
					}
			if (!flips.empty())
				p.lastFlip = flips.back().token;
			for (std::size_t f = from; f < flips.size(); ++f) {
				const auto& flip = flips[f];
				const auto found = lodGates.find(flip.token);
				if (found == lodGates.end() || found->second.state != V::LodGate::State::Flipped) {
					Differ(p, [&] { return fmt::format("flip: gate {} flipped by this commit is {}", flip.token, found == lodGates.end() ? "gone" : "open"); });
					continue;
				}
				const auto& gate = found->second;
				ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint8_t> held;
				for (auto* geometry : gate.incomingGeometries) {
					const auto it = tracked.find(geometry);
					if (it == tracked.end())
						continue;
					if (!ReadyNow(a_commit, geometry, it->second))
						Differ(p, [&] {
							return fmt::format("early: gate {} flipped with incoming geometry {} not ready by a fresh decision (slots {}, {})", flip.token, static_cast<const void*>(geometry),
								it->second.slot, it->second.layerSlot);
						});
					if (const std::uint32_t slot = it->second.slot; slot != kNoObjectSlot && slot < setPhases.size() && !tables.IsLayer(slot) && setPhases[slot])
						held.emplace(geometry, setPhases[slot]);
				}
				if (a_retagged)
					continue;
				if (flip.incoming.size() != held.size())
					Differ(p, [&] { return fmt::format("flip: gate {}: {} incoming geometries flipped, the commit holds {} with phases", flip.token, flip.incoming.size(), held.size()); });
				for (const auto& [geometry, phases] : flip.incoming)
					if (const auto it = held.find(geometry); it == held.end() || it->second != phases)
						Differ(p, [&] {
							return fmt::format("flip: gate {}: incoming geometry {} flipped with {:#x}, the commit holds {:#x}", flip.token, static_cast<const void*>(geometry), phases,
								it == held.end() ? 0u : it->second);
						});
				std::size_t outgoing = 0;
				for (auto* geometry : gate.outgoingGeometries)
					outgoing += tracked.contains(geometry) ? 1u : 0u;
				if (flip.outgoing.size() != outgoing)
					Differ(p, [&] { return fmt::format("flip: gate {}: {} outgoing geometries flipped, {} tracked in its outgoing", flip.token, flip.outgoing.size(), outgoing); });
				for (const auto* geometry : flip.outgoing)
					if (auto* key = const_cast<RE::BSGeometry*>(geometry); !gate.outgoingGeometries.contains(key) || !tracked.contains(key))
						Differ(p, [&] { return fmt::format("flip: gate {}: outgoing geometry {} flipped, not a tracked outgoing of it", flip.token, static_cast<const void*>(geometry)); });
			}
		}

		/**
		 * @brief The gate root TagGateAbove gives an entry now: the nearest key of the root map on its mirror chain, up to its category node.
		 * a_resolved false: the chain meets a key the mirror holds no record of (TagGateAbove stops there; the gate's own walk does not
		 * reach past it either), or runs past any depth a scene has.
		 */
		const V::GateRoot* GateAbove(const Scene::Coordinator& a_store, const RE::BSGeometry* a_geometry, const V::Tracked& a_entry, bool& a_resolved)
		{
			const auto& mirror = V::mirror(a_store);
			const auto& gateOfRoot = V::gateOfRoot(a_store);
			const auto* record = mirror.Node(a_geometry);
			if (!record) {
				a_resolved = false;
				return nullptr;
			}
			std::uint32_t depth = 0;
			for (const void* key = record->parent; key;) {
				if (const auto at = gateOfRoot.find(key); at != gateOfRoot.end())
					return &at->second;
				if (key == a_entry.categoryNode)
					return nullptr;
				const auto* node = mirror.Node(key);
				if (!node || ++depth > 256) {
					a_resolved = false;
					return nullptr;
				}
				key = node->parent;
			}
			return nullptr;
		}

		/**
		 * @brief G7: every open gate's incoming root hidden in the mirror (the engine side hides the block before its attach and vetoes its
		 * shows until the flip). A root the mirror has no record of holds its gate as unmirrored. A shown root can be a forced release's
		 * show whose outcome is not applied yet (FlushAll shows the blocks, then posts the outcome, which may miss the pass that took the
		 * show), or a flip-hidden block reattached before its hide's event: flagged only when the same gate still shows it at the next sweep.
		 */
		void CheckVetoes(const Commit& a_commit)
		{
			const auto& mirror = V::mirror(a_commit.store);
			auto& p = a_commit.parity;
			std::vector<std::pair<std::uint64_t, const void*>> shown;
			for (const auto& [token, gate] : V::lodGates(a_commit.store)) {
				if (gate.state != V::LodGate::State::Open)
					continue;
				for (const void* root : gate.incoming) {
					const auto* record = mirror.Node(root);
					if (!record || (record->flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kHidden)))
						continue;
					shown.emplace_back(token, root);
					if (std::ranges::find(p.shown, shown.back()) != p.shown.end())
						Differ(p, [&] { return fmt::format("veto: gate {} open with incoming root {} shown in the mirror at two sweeps", token, root); });
				}
			}
			p.shown = std::move(shown);
		}

		/**
		 * @brief The sweep: G2 over the tracked set (each tagged entry in its gate's set, the held-out count), G3 for the held-out entries
		 * no gate's set covered, G6 (each entry's tag against TagGateAbove's rule now), then G7.
		 */
		void Sweep(const Commit& a_commit)
		{
			const auto& store = a_commit.store;
			const auto& lodGates = V::lodGates(store);
			auto& p = a_commit.parity;
			++p.sweeps;
			std::uint32_t heldOut = 0;
			std::vector<std::pair<std::uint64_t, const void*>> unmarkedOutgoing;
			for (const auto& [geometry, entry] : V::tracked(store)) {
				++p.entries;
				bool covered = false;
				if (entry.gate) {
					const auto gate = lodGates.find(entry.gate);
					const auto* set = gate == lodGates.end()                ? nullptr :
					                  entry.gateSide == V::kGateIncoming ? &gate->second.incomingGeometries :
					                  entry.gateSide == V::kGateOutgoing ? &gate->second.outgoingGeometries :
					                                                       nullptr;
					covered = set && set->contains(geometry);
					if (!covered)
						Differ(p, [&] {
							return fmt::format("tags: geometry {} tagged gate {} ({}), {}", static_cast<const void*>(geometry), entry.gate, SideName(entry.gateSide),
								gate == lodGates.end() ? "no such gate" : "not in its set");
						});
				}
				if (entry.gateHeldOut) {
					++heldOut;
					// A gate's held-out outgoing was checked with its gate (CheckTags).
					if (!covered)
						CheckWithheld(a_commit, geometry, entry, false);
					// Held out with no gate: a retired flip's outgoing awaiting its detach, under no gate root any more.
					if (!entry.gate)
						continue;
				}
				bool resolved = true;
				const auto* above = GateAbove(store, geometry, entry, resolved);
				if (!resolved)
					continue;
				const std::uint64_t token = above ? above->token : 0;
				const std::uint8_t side = above ? above->side : 0;
				if (!above && entry.gate && entry.gateSide == V::kGateOutgoing) {
					// An outgoing tag under no gate root: the root detached (or a held block reattached) ahead of its entries' erasure or
					// the kept notice, which untag them. Flagged only when still so at the next sweep.
					++p.unmarked;
					unmarkedOutgoing.emplace_back(entry.gate, geometry);
					if (std::ranges::find(p.unmarkedOutgoing, unmarkedOutgoing.back()) != p.unmarkedOutgoing.end())
						Differ(p, [&] {
							const auto gate = lodGates.find(entry.gate);
							return fmt::format("marks: geometry {} tagged gate {} (outgoing) under no gate root at two sweeps; gate {}, {} outgoing roots left, {}held out",
								static_cast<const void*>(geometry), entry.gate,
								gate == lodGates.end() ? "retired" : gate->second.state == V::LodGate::State::Open ? "open" : "flipped",
								gate == lodGates.end() ? 0 : gate->second.outgoing.size(), entry.gateHeldOut ? "" : "not ");
						});
					continue;
				}
				if (token != entry.gate || side != entry.gateSide)
					Differ(p, [&] {
						return fmt::format("marks: geometry {} tagged gate {} ({}), its mirror chain's nearest root is gate {} ({})", static_cast<const void*>(geometry), entry.gate,
							SideName(entry.gateSide), token, SideName(side));
					});
			}
			p.unmarkedOutgoing = std::move(unmarkedOutgoing);
			if (heldOut != V::gateHeldOutCount(store))
				Differ(p, [&] { return fmt::format("tags: {} entries held out, {} counted", heldOut, V::gateHeldOutCount(store)); });
			CheckVetoes(a_commit);
		}

		void OnCommitted(const SetCommitted& a_event)
		{
			if (!a_event.gatesLive)
				return;
			const auto& store = a_event.store;
			Commit commit{ store, a_event.live, a_event.drawn, SceneDiagnostics().gates.parity };
			auto& p = commit.parity;
			++p.checks;
			CheckRoots(commit);
			const bool retagged = !V::gateQueued(store).empty();
			for (const auto& [token, gate] : V::lodGates(store)) {
				++p.gates;
				CheckTags(commit, token, gate);
				if (gate.state == V::LodGate::State::Open)
					CheckOpen(commit, token, gate, retagged);
			}
			CheckFlips(commit, retagged);
			if (ParityDue(V::passSerial(store), 37) && std::exchange(p.pass, store.PassStamp()) != store.PassStamp())
				Sweep(commit);
		}
	}

	void InstallGateParity()
	{
		Subscribe<SetCommitted>(OnCommitted);
	}
}
