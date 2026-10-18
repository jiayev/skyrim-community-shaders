#pragma once

// The LOD gates' coordinator component (LodGates.h has the engine side; dclf-architecture.md, "LOD gates: the scene side"): its state
// (made by its make hook, held by the coordinator), its hooks (LodGateComponent.cpp: the gate events, the root marks' events, the commit's
// settle, the outcomes; LodGateAdoption.cpp: the flips' publication and release, the adoption guard; LodGateReports.cpp: its report lines),
// and what its observers read of it (Diagnostics/LodGateParity.cpp, LodGateObservers.cpp). Registered by the feature's install
// (LodGateChannel.cpp). Reaches the coordinator only through its context (Api::CoordinatorComponentContext). Included by the feature's
// sources only.

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::Features::Lod::Gates
{
	/** @brief The gates' coordinator component (LodGateChannel.cpp's registration, at the feature's install; none while they are disabled). */
	inline Api::CoordinatorComponentId component;
	/** @brief The gates' SceneStack channel (LodGateChannel.cpp's registration): the adoption wakes it for the outcomes the flips posted. */
	inline Api::EngineChannelId channel;

	/** @brief The LOD gates' clock (steady, ns) and a span on it in ms (0 without a start). */
	inline std::uint64_t GateNowNs()
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}
	inline double GateMs(std::uint64_t a_from, std::uint64_t a_to)
	{
		return a_from && a_to > a_from ? static_cast<double>(a_to - a_from) / 1.0e6 : 0.0;
	}

	/**
	 * @brief A LOD gate flipped by a commit (the settle), published as the gates' component's delta and released by the frame installing
	 * its publication (Adopt).
	 */
	struct GateFlip
	{
		std::uint64_t token = 0;
		std::vector<std::pair<const RE::BSGeometry*, std::uint8_t>> incoming;  // the incoming base geometries claimed, with the flip's phases
		std::vector<const RE::BSGeometry*> outgoing;                          // the outgoing base geometries dropped
		std::uint64_t requestNs = 0, flipNs = 0;                              // its event's push, the flip (steady clock)
		std::uint32_t requestFrame = 0, flipFrame = 0;                        // the render thread's frames then (FrameClock::Frame)
	};

	/**
	 * @brief The LOD gates, coordinator side. A gate tags the geometries under its roots (its gate event); the commit withholds its incoming
	 * slots (their Observe veto) until all are whole members or ineligible and mirrored, then flips it (the component's settle): observed
	 * phases applied, outgoing blocks held out (their Hold veto) until their detach, the flip published. It retires with its outcome (the
	 * channel's AfterBatch keys).
	 */
	inline constexpr std::uint8_t kGateIncoming = 1, kGateOutgoing = 2;
	struct LodGate
	{
		enum class State : std::uint8_t
		{
			Open,     // the incoming withheld, the outgoing members
			Flipped,  // the incoming applied, the outgoing held out (Api::kVetoHold), awaiting the outcome
		};
		State state = State::Open;
		std::vector<const void*> incoming, outgoing;  // the roots (keys), less those detached since
		ankerl::unordered_dense::set<RE::BSGeometry*> incomingGeometries, outgoingGeometries;  // the tracked ones tagged
		std::uint32_t pending = 0;     // incoming geometries neither whole members nor ineligible
		std::uint32_t unmirrored = 0;  // incoming roots or nodes under them without a mirror record
		std::uint64_t stampNs = 0;     // its event's push, steady clock
		std::uint32_t stampFrame = 0;
		std::uint64_t flipNs = 0;
		std::uint32_t flipFrame = 0;
	};
	/** @brief An Api::TrackedState slot's none (Core's kNoObjectSlot). */
	inline constexpr std::uint32_t kNoSlot = ~0u;
	/** @brief A tracked geometry's tag: its gate (token) and side (kGateIncoming, kGateOutgoing). */
	struct GateTag
	{
		std::uint64_t token = 0;
		std::uint8_t side = 0;
	};
	struct GateOutcome
	{
		std::uint64_t token = 0;
		bool forced = false;
		std::array<const void*, 2> kept{};  // a held block the engine reused (LodGates::Outcome::kept), by its roots
	};
	struct GateStats
	{
		std::uint64_t opened = 0, flipped = 0, retiredFlipped = 0, retiredForced = 0, forcedOpen = 0, unknownOutcomes = 0, superseded = 0;
		std::uint64_t rootsDetached = 0, taggedIncoming = 0, taggedOutgoing = 0, heldOut = 0, emptyIncoming = 0;
		// Kept notices (ApplyGateKept): taken, geometries released from hold-out, outgoing tags dropped before a flip.
		std::uint64_t keptNotices = 0, keptCleared = 0, keptUntagged = 0;
		// Flips deferred for an incoming record the next publication claims first.
		std::uint64_t structureDeferred = 0;
		std::vector<double> requestToFlipMs;
		std::vector<std::uint32_t> requestToFlipFrames;
	};

	/** @brief The gates' observers: the flipped incoming geometries the flip's own publication does not claim (the coordinator's). */
	struct ObserverState
	{
		std::uint64_t publishHoles = 0;
		std::string firstPublishHole;
		// CS_DCLF_PERSISTENT_PARITY: the gates' state against what it is derived from (LodGateParity.cpp), since the last report.
		struct Parity
		{
			std::uint64_t checks = 0, gates = 0, sweeps = 0, entries = 0, differ = 0, unmarked = 0;
			std::string first;  // its invariant's tag first (roots, tags, withheld, ready, stuck, early, flip, marks, veto)
			std::uint32_t pass = 0;  // the pass (PassStamp) that ran the sweep: once a pass
			// Kept across reports: the newest flip checked (each by the commit that made it; tokens are unique), and the open
			// gates' incoming roots the last sweep found shown in the mirror.
			std::uint64_t lastFlip = 0;
			std::vector<std::pair<std::uint64_t, const void*>> shown;
			// And the outgoing-tagged entries the last sweep found under no gate root.
			std::vector<std::pair<std::uint64_t, const void*>> unmarkedOutgoing;
		} parity;
	};

	/** @brief The component's state (MakeState; the scene lane): the gates, the tracked geometries' tags, the outcomes held, the counters. */
	struct GateState
	{
		ankerl::unordered_dense::map<std::uint64_t, LodGate> lodGates;
		// The gates' roots are the coordinator's marks (the component's, token and side kGateIncoming/kGateOutgoing; the incoming own their
		// hide). The tracked geometries' tags (erased with the entry: the component's forgot, forgotAll). The incoming of an open gate are
		// observed and the flipped gates' outgoing held (the coordinator's vetoes, observed slots and requeued slots).
		ankerl::unordered_dense::map<const RE::BSGeometry*, GateTag> gateTags;
		std::vector<const void*> gateEntries;       // a root's tracked entries (SubtreeEntries), reused by each walk
		std::vector<GateOutcome> gateOutcomesHeld;  // taken, not applied yet
		GateStats gateStats;
		ObserverState observed;
	};

	/**
	 * @brief Whether a gated incoming geometry is ready (a_entry its tracked state; a_observedOf: a slot's observed record): ineligible by
	 * a verdict of the gate's time, or every slot of it whole. The settle's, the report's and the parity's.
	 */
	template <class ObservedOf>
	bool GeometryReady(const void* a_geometry, const Api::TrackedState& a_entry, ObservedOf&& a_observedOf)
	{
		// Classified since it was tagged (its tag took its verdict again), with its leaf's records.
		if (a_entry.leafWaiting || !a_entry.classified)
			return false;
		// No record: ineligible for a recorded verdict (a hide still counts here when it is not the gate's: HiddenForWalk), or not written
		// yet (Ineligible::None, 0).
		if (a_entry.slot == kNoSlot && a_entry.layerSlot == kNoSlot)
			return a_entry.reason != 0;
		// Each slot of it whole at its last evaluation by the commit (its Observe's record).
		for (const std::uint32_t slot : { a_entry.slot, a_entry.layerSlot })
			if (slot != kNoSlot)
				if (const Api::ObservedState observed = a_observedOf(slot); observed.geometry != a_geometry || !observed.ready)
					return false;
		return true;
	}

	// The observers' reads (the scene lane; they move with the gates).
	/** @brief The component's state as the coordinator holds it (SceneView::componentState with component), null before it is made. */
	inline GateState* StateOf(void* a_componentState) { return static_cast<GateState*>(a_componentState); }
	/** @brief Whether a root mark is the gates' (their component's). */
	inline bool IsGateMark(const Api::Mark& a_mark) { return a_mark.component.index == component.index; }
	/** @brief The flips the commits made since the last publication: the component's deltas pending (SceneView::componentDeltas), each a GateFlip. */
	inline auto Flips(const std::vector<std::shared_ptr<const void>>& a_deltas)
	{
		return a_deltas | std::views::transform([](const std::shared_ptr<const void>& a_delta) -> const GateFlip& { return *static_cast<const GateFlip*>(a_delta.get()); });
	}

	// LodGateChannel.cpp.
	/**
	 * @brief The feature's install: the engine side's hooks (LodGates::Install, a_switch: CS_DCLF_LOD_GATES), then, while they are
	 * installed, the gates' channel, show veto, hidden site, adoption guard, component and report lines.
	 */
	void RegisterGates(Api::InstallContext& a_context, Api::SwitchId a_switch);

	// LodGateComponent.cpp: the component's hooks (Api::CoordinatorComponent).
	/** @brief make: the component's state (GateState). */
	std::shared_ptr<void> MakeState();
	/** @brief Events::stacked: a gate event (in stack order, after its incoming blocks' attaches) opens its gate (a_event's pinned incoming, named outgoing). */
	void ApplyGateEvent(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, const Api::Stacked& a_event);
	/** @brief Events::detached: a_key (its mark erased: the coordinator's UnmarkDetached) leaves its gate. */
	void RootDetached(const Api::CoordinatorComponentContext& a_context, const void* a_key, const Api::Mark& a_mark);
	/** @brief Events::rescan: the gates' tags dropped with the tracked set (its adds tag again). */
	void Rescan(const Api::CoordinatorComponentContext& a_context);
	/** @brief Events::underMark: a newly tracked a_geometry under a root of theirs (the prepare's mark walk found it) tagged. */
	void UnderMark(const Api::CoordinatorComponentContext& a_context, const void* a_geometry, const Api::Mark& a_mark);
	/**
	 * @brief Commit::settle: flips the open gates that are ready (roots mirrored, incoming ready, a live scene), their vetoes dropped and
	 * set in this commit, into the component's deltas (PublishDelta: each flip a GateFlip, published by this commit's publication).
	 */
	void Settle(const Api::CoordinatorComponentContext& a_context, bool a_live);
	/** @brief Commit::take (before the pass's events are collected): holds LodGates' outcomes posted so far. */
	void Take(const Api::CoordinatorComponentContext& a_context);
	/**
	 * @brief Events::keys on the gates' channel (AfterBatch: a pass that applies a batch, after it; not a load screen's pass): retires each
	 * held outcome's gate (a forced gate's incoming reclassified).
	 */
	void ApplyOutcomes(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, std::span<const void* const> a_keys);
	/** @brief Retirement::forgot: a tracked entry erased, off its gate's tag (its veto is the coordinator's, erased with it). */
	void Forgot(const Api::CoordinatorComponentContext& a_context, const void* a_entry);
	/** @brief Retirement::forgotAll: the tracked set cleared (the rescan), every tag with it. */
	void ForgotAll(const Api::CoordinatorComponentContext& a_context);

	// LodGateAdoption.cpp: the publication and the adoption.
	/** @brief Commit::published: the GateFlipsPublished observers, against the publication's claims. */
	void Published(const Api::CoordinatorComponentContext& a_context);
	/**
	 * @brief Adoption::adopt (render thread, the frame's start, after the coverage decision - InstallClaims, or none yet - before the
	 * culls): releases the flips the installed publications carried (LodGates::Flip: incoming shown, outgoing hidden, in the first
	 * post-swap frame), then wakes the coordinator. Under set or persistent parity, checks the claims against each
	 * flip (<- GATE HOLE, <- GATE COVERAGE).
	 */
	void Adopt(const Api::AdoptionContext& a_context);

	/** @brief Render thread: the releases since the last report (ReleaseLines takes them), and their observers' checks. */
	struct ReleaseStats
	{
		std::uint64_t released = 0, refused = 0, beforeDrawing = 0;  // beforeDrawing: in a frame before DCLF's first covered one (no claims)
		std::vector<double> flipToReleaseMs, requestToReleaseMs;
		std::vector<std::uint32_t> flipToReleaseFrames;
		bool any = false;
		// The observers at the release (LodGateObservers.cpp): the flips checked against the installed claims.
		struct Observed
		{
			std::uint64_t checked = 0, holes = 0, refusedHoles = 0, coverage = 0;
			std::string firstHole, firstCoverage;
		} observed;
	};
	ReleaseStats& Releases();

	// LodGateReports.cpp: the component's report lines.
	/**
	 * @brief ReportLines::componentText at SceneCoordinatorGates (logged as one line): opened, flipped, retired; the open ones' age and
	 * what they wait for; the observers' holes and the parity. Empty while nothing happened.
	 */
	void GateLines(const Api::CoordinatorComponentContext& a_context, std::string& a_text);
	/** @brief ReportLines::lines at GateReleases (render thread, once a report window): the releases' line, then the engine side's. */
	void ReleaseLines(std::uint32_t a_frame);

	// Diagnostics/LodGateObservers.cpp, Diagnostics/LodGateParity.cpp.
	/** @brief CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY (Diagnostics::Observe().claimsOrScene): the flips against the claims. */
	void SubscribeGateObservers();
	/** @brief CS_DCLF_PERSISTENT_PARITY (Diagnostics::Observe().scene): the gates' state after each commit. */
	void SubscribeGateParity();
}
