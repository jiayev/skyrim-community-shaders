#pragma once

// The coordinator component contract (Api::InstallContext::RegisterCoordinatorComponent): a feature's state kept by the scene
// coordinator beside its residents, and the hooks the coordinator calls at its points - what a feature lists for the scene's members (its
// table family's rows, the engine nodes it owns, the seeds it asks the frame for) follows the members. The hooks come in sets, one per
// concern of the coordinator's: residency, the accumulate phase, the walk, the walk parity's dense walk, retirement, the writes of the
// entry class the component follows (Api::EntryClass, Api/ObjectContributors.h), the engine channels' events (Api/EngineEvents.h), the
// commit and publication, and the frame's adoption.

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/CoordinatorComponents.h), which the features inside DCLF include for them.
	class CoordinatorComponentContext;
	class ResidentRecount;
	class AccumulatedPatch;
	class SlotRewrite;
	class WalkScheduling;
	class EntryWrite;

	/** @brief A registered component (RegisterCoordinatorComponent's result): FrameView::TakeComponentSeeds takes its seeds by it. */
	struct CoordinatorComponentId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief A mark on a scene root (CoordinatorComponentContext::MarkRoot): the component that marked it, its token and its side, which
	 * the coordinator reads as data for the entries under the root (CoordinatorComponent::Events::underMark) and erases with the root's
	 * detach (detached). No component: no mark.
	 */
	struct Mark
	{
		CoordinatorComponentId component;
		std::uint64_t token = 0;
		std::uint8_t side = 0;
		explicit operator bool() const { return static_cast<bool>(component); }
	};

	/**
	 * @brief An entry's veto (CoordinatorComponentContext::SetEntryVeto; one byte, so Hold and Observe exclude each other): the commit
	 * withholds the entry's slots. Hold: applied as no phases. Observe: withheld, and what the commit would have applied is recorded
	 * (CoordinatorComponentContext::ObservedSlot). 0: none.
	 */
	inline constexpr std::uint8_t kVetoHold = 1, kVetoObserve = 2;

	/**
	 * @brief What CoordinatorComponentContext::Requeue does with an entry: its slots committed again at the next commit (Slots); its
	 * verdict taken again when it was classified (Reevaluate), or whatever it was (ReevaluateAlways).
	 */
	inline constexpr std::uint8_t kRequeueSlots = 1, kRequeueReevaluate = 2, kRequeueReevaluateAlways = 4;

	/**
	 * @brief The mirrored node kind bits a component may test (CoordinatorComponentContext::MirroredFadeNode's a_kind, the mirror's capture
	 * kind as is; Core checks they match it). Tree: a BSTreeNode. Captured: a node capture contribution's class (Api/EngineEvents.h,
	 * NodeCaptureContribution), its list in MirroredNode::captured.
	 */
	inline constexpr std::uint32_t kMirroredKindTree = 1u << 8;
	inline constexpr std::uint32_t kMirroredKindCaptured = 1u << 9;

	/**
	 * @brief A node as the mirror holds it (CoordinatorComponentContext::MirroredNodeOf; known false: no record), viewed in place: valid
	 * until the hook returns. Keys are never dereferenced.
	 */
	struct MirroredNode
	{
		bool known = false;
		const void* parent = nullptr;  // null: a root
		const void* rtti = nullptr;    // its RTTI exactly
		std::uint32_t kind = 0;        // its capture kind as is (kMirroredKind*)
		std::span<const void* const> children;  // by engine slot (stable; null slots kept): what OnVisible visits, in order
		std::span<const void* const> captured;  // kMirroredKindCaptured: its contribution's list, in its order (else empty)
	};

	/** @brief A tracked entry's state as the coordinator keeps it (CoordinatorComponentContext::TrackedStateOf; tracked false: none). */
	struct TrackedState
	{
		bool tracked = false;
		bool classified = false;   // a pass took its verdict (and it has not been asked again)
		bool leafWaiting = false;  // written while the mirror held no record of it
		std::uint32_t slot = ~0u, layerSlot = ~0u;  // its object slots (~0u: none)
		std::uint8_t reason = 0;     // its candidate verdict (Ineligible)
		std::uint8_t veto = 0;       // kVetoHold, kVetoObserve or 0
		std::uint8_t waitCause = 0;  // its slot's: what the commit's wait waits on (0: not waiting)
		std::uint8_t waitWhy = 0;    // its slot's first reason of its last wait
		bool resident = false;       // its slot resident
	};

	/** @brief What the commit recorded of an observed slot (CoordinatorComponentContext::ObservedSlot): its entry, phases, readiness. */
	struct ObservedState
	{
		const void* geometry = nullptr;  // the entry the commit last evaluated the slot for (null: none since the Observe)
		std::uint8_t phases = 0;
		bool ready = false;  // all its phases, not waiting
	};

	/**
	 * @brief A root of the primary cull's admitted entries (CoordinatorComponent::Events::admitted): its node (a key, never dereferenced),
	 * and whether DCLF stands in for it whole (no engine-drawn part: its state the GPU's alone).
	 */
	struct StandInRoot
	{
		const void* node = nullptr;
		bool standIn = false;
	};

	/**
	 * @brief What the adoption hook is handed (render thread, the frame's start, after the frame's coverage decision: claims
	 * installed, or none yet): the frame, the claims it installed (the installed publication's set snapshot; null before DCLF's first
	 * covered frame, FrameView::NotDrawingYet), and the component's deltas (CoordinatorComponentContext::PublishDelta) of the
	 * publications installed since the last call, oldest first.
	 */
	struct AdoptionContext
	{
		std::uint32_t frame = 0;
		const void* claims = nullptr;
		std::span<const std::shared_ptr<const void>> deltas;
	};

	/**
	 * @brief A coordinator component: its name, its state, and its hook sets (each below, documented once).
	 *
	 * What every hook shares:
	 * - Thread: the scene lane, inside a pass (Adoption's: the render thread).
	 * - Context: CoordinatorComponentContext (Core/Extensions/CoordinatorComponents.h): the coordinator's live tables, the component's
	 *   state, and the few coordinator services a residency change or a walk's phase needs, never the coordinator itself.
	 * - Order: every registered component's hook in registration order, at exactly the place the coordinator's own work runs there.
	 * - Cost: one call per event - a changed slot (a join, a leave, an answer, a write), an erased entry, a freed range or a phase -
	 *   never per object per frame. The loops over every object each frame call no hook: they read what the hooks wrote, as data.
	 * - Every hook is optional (null: not called).
	 *
	 * - name: the component's (the install's failures).
	 * - make: the component's state, once, before its first hook; held by the coordinator for the session.
	 */
	struct CoordinatorComponent
	{
		/**
		 * @brief A slot's record, not its residency (Walk.cpp, ApplyEvents.cpp, Slots.cpp): what the component lists for what a slot's record
		 * names (the fade root its fade node is: Features/Fades) follows the record's writes and its slot's release, whether or not the
		 * slot joins. Called in the walk parity's dense walk too: a component whose listing the dense walk must not touch returns there
		 * (CoordinatorComponentContext::DenseWalk), as the fade roots do.
		 *
		 * - written: the walk wrote a_slot's record or a layer's (before its identity is set and before Walk::rewritten), or the render
		 *   thread answered the fade node a_slot's entry asked for (ApplyFadeNodeAnswers, before Residency::resident): list what the
		 *   record names. Called again for a slot already listed.
		 * - released: a_slot is freed (a slot's or a layer's release, the walk's sweep of unseen slots, the scan's untracked hit): after its
		 *   residency ended where it was resident (Residency::left), right before the slot's object is reset: unlist what it listed.
		 */
		struct Slots
		{
			void (*written)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot) = nullptr;
			void (*released)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot) = nullptr;
		};

		/**
		 * @brief Residency (Residents.cpp; ApplyEvents.cpp's answers): a slot's residency begins, changes and ends, and the residents are
		 * recounted. What the component lists for the members follows them.
		 *
		 * - resident: a_slot is resident: it joined (or a resident's patch changed: MarkResidentSlot), or the render thread answered the
		 *   fade node its entry asked for (ApplyFadeNodeAnswers). Lists what the slot needs; called again for a slot already listed. Right
		 *   after the slot's resident position is grown, before its patch is stored.
		 * - left: a_slot's residency ended (DropResidentSlot, after its resident position is cleared and before the primary cull is told;
		 *   EndAllResidency, per resident, after allLeft and before its accumulated half is reset). Unlists what it listed.
		 * - allLeft: every residency ends (EndAllResidency), before left is called for each resident: what the component keeps of the
		 *   members goes whole.
		 * - recount: the residents kept alive (KeepResidentsAlive), after the coordinator counted the slots touched since the last one
		 *   (or, rebuilt, every resident), before the used sets: the component recounts the same slots (ResidentRecount).
		 */
		struct Residency
		{
			void (*resident)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot) = nullptr;
			void (*left)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot) = nullptr;
			void (*allLeft)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*recount)(const CoordinatorComponentContext& a_context, const ResidentRecount& a_recount) = nullptr;
		};

		/**
		 * @brief The accumulate phase (AccumulatePhase.cpp): the members' accumulated halves written from their patches, then the joins
		 * done.
		 *
		 * - patched: the phase wrote a_slot's accumulated half from its patch (a join, or a resident's patch that changed:
		 *   ApplyAccumulatePatch), after the write: the patch's values the component lists by (AccumulatedPatch).
		 * - joinsDone: the phase's joins done (the residents kept alive, the slots checked), in its statistics: the component brings what
		 *   it derives from the members up to date, writing the live tables (noting their changes).
		 */
		struct Accumulate
		{
			void (*patched)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot, const AccumulatedPatch& a_patch) = nullptr;
			void (*joinsDone)(const CoordinatorComponentContext& a_context) = nullptr;
		};

		/**
		 * @brief The walk (Walk.cpp), each one: the delta walk's and the walk parity's dense walk.
		 *
		 * - begun: the walk begins (BeginWalk), its frame tables cleared and its serial moved on: the component takes the events it
		 *   follows since the last walk.
		 * - scheduling: the delta walk schedules its entries (WalkScheduling): a full evaluation, where nothing kept is trusted, or, after
		 *   the pending entries, the component's events' writes (ScheduleWrite) and its in-place updates, counted.
		 * - rewritten: the walk rewrote a_slot's scene half (its write, or a layer's), or refreshed its skin partitions in place
		 *   (SlotRewrite), where the coordinator writes them. Per written slot (the walk writes the slots its events changed).
		 * - ended: the walk's end (after the candidates; the dense walk's after its sweep): what it retired this walk.
		 */
		struct Walk
		{
			void (*begun)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*scheduling)(const CoordinatorComponentContext& a_context, WalkScheduling& a_scheduling) = nullptr;
			void (*rewritten)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot, const SlotRewrite& a_rewrite) = nullptr;
			void (*ended)(const CoordinatorComponentContext& a_context) = nullptr;
		};

		/**
		 * @brief The walk parity's dense walk (Walk.cpp, DenseReference), which restores the tables after it, not what the components keep
		 * beside them: begun, it is about to run (save it); ended, it has run (restore it). Between them the walk's hooks run for it too.
		 */
		struct DenseWalk
		{
			void (*begun)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*ended)(const CoordinatorComponentContext& a_context) = nullptr;
		};

		/**
		 * @brief Retirement (Slots.cpp, ApplyEvents.cpp, SceneSet.cpp): Core lets go of what the component's holdings name - the slot
		 * tables, a tracked entry (every entry, whatever its class), a range the component retired.
		 *
		 * - reset: the slot tables reset (ResetSlotTables), before the tables are cleared: the component's references are handed back
		 *   (through the retirement chain, with everything else the tables named).
		 * - forgot: a tracked entry is erased (EraseTracked, after its slot is released): what the component keeps of it goes.
		 * - recycled: a range the component retired (CoordinatorComponentContext::RetireRange) is free: no publication names it any more
		 *   (RecycleRetired). The retiring component's alone.
		 */
		struct Retirement
		{
			void (*reset)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*forgot)(const CoordinatorComponentContext& a_context, const void* a_entry) = nullptr;
			void (*recycled)(const CoordinatorComponentContext& a_context, std::uint32_t a_first, std::uint32_t a_count) = nullptr;
		};

		/**
		 * @brief The entry class the component follows (Walk.cpp, Coordinator::MergeWrite): the merge's writes of the class's entries, each a
		 * changed slot (the walk writes the slots its events changed). Only the following component's hooks are called.
		 *
		 * - follows: the class (one component's; None: no entries' writes).
		 * - takes: whether the class's objects are DCLF's (Core's eligibility and walk read it as data: Extensions::EntryClassTaken). The
		 *   hooks below are called whatever it says.
		 * - listed: the walk writes an entry of the class (every write, before its verdict's record): the component lists it.
		 * - resolve: a record of a taken entry is being written, before its slot is taken: the entry's second vertex stream into
		 *   EntryWrite; false (or none): the record waits (no record this walk; the component's next event writes it again).
		 * - streamed: the record resolve gave a stream to took a_slot (where the walk writes its scene half): the component points the slot
		 *   at the stream.
		 */
		struct EntryClassHooks
		{
			EntryClass follows = EntryClass::None;
			bool takes = false;
			void (*listed)(const CoordinatorComponentContext& a_context, const EntryWrite& a_write) = nullptr;
			bool (*resolve)(const CoordinatorComponentContext& a_context, EntryWrite& a_write) = nullptr;
			void (*streamed)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot, EntryWrite& a_write) = nullptr;
		};

		/**
		 * @brief The engine channels' events (ApplyEvents.cpp, ApplyBatch's structural events; Api::EngineChannel).
		 *
		 * - keys: a pass applies a_channel's keys at the channel's point (ChannelApply), the channels at a point in registration order:
		 *   the keys its queue held at the pass's drain, oldest first (possibly none: called every pass). The keys are never dereferenced: an entry the coordinator tracks is the only one known alive.
		 * - stacked: a pass applies a SceneStack channel's event (Api::Stacked) in stack order, among the attaches and detaches.
		 * - detached: a root the component marked (or a node under a detached root) is detached: its mark is erased (a_mark, as it was)
		 *   before the call. The marking component's alone.
		 * - underMark: a newly tracked entry (a_geometry) is under a root marked a_mark (the nearest on its chain). The marking
		 *   component's alone; per added entry, never per frame.
		 * - switched: a pass applies a switch event (ApplyEvents' switch loop, per applied event, right after it is noted applied and
		 *   before the entries under it are scheduled; a full evaluation's too): a_switch, the switch node (a key, never dereferenced). A
		 *   tree root's LOD switch restepping its root (Features/Fades).
		 * - admitted: the primary cull's admitted roots, whole, latest wins (FrameView::SetStandInRoots, render thread), at the pass's
		 *   coordinator inputs after the table families' sent epochs (TableFamilies::TakeSent): which listed roots it owns and which it
		 *   stands in for (StandInRoot). The pass forces a publication. Per change of the set, never per frame.
		 */
		struct Events
		{
			void (*keys)(const CoordinatorComponentContext& a_context, EngineChannelId a_channel, std::span<const void* const> a_keys) = nullptr;
			void (*stacked)(const CoordinatorComponentContext& a_context, EngineChannelId a_channel, const Stacked& a_event) = nullptr;
			void (*detached)(const CoordinatorComponentContext& a_context, const void* a_key, const Mark& a_mark) = nullptr;
			void (*underMark)(const CoordinatorComponentContext& a_context, const void* a_geometry, const Mark& a_mark) = nullptr;
			void (*switched)(const CoordinatorComponentContext& a_context, const void* a_switch) = nullptr;
			void (*admitted)(const CoordinatorComponentContext& a_context, std::span<const StandInRoot> a_roots) = nullptr;
		};

		/**
		 * @brief The commit (SceneSet.cpp) and the publication.
		 *
		 * - settle: the commit, after its slots are decided and before the set's change is noted and the claims are snapshotted: vetoes
		 *   changed here apply in the same commit (an Observe dropped applies the observed phases where the observed entry matches; Hold
		 *   applies none). Called only while the commit runs live (any component live, CoordinatorComponentContext::SetCommitLive, or any
		 *   entry vetoed); a_live: whether the scene is built (not a load screen: its members are decided).
		 * - take: the pass, before its events are collected: what the component's own queues hold, taken (applied after the batch, by an
		 *   AfterBatch channel's keys hook).
		 * - published: the publication's deltas are taken (its PublishDelta payloads among them).
		 */
		struct Commit
		{
			void (*settle)(const CoordinatorComponentContext& a_context, bool a_live) = nullptr;
			void (*take)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*published)(const CoordinatorComponentContext& a_context) = nullptr;
		};

		/**
		 * @brief The adoption (render thread, the frame's start: AdoptionContext), every frame. Called with no coordinator context: what it
		 * keeps is the component's own render-thread state.
		 */
		struct Adoption
		{
			void (*adopt)(const AdoptionContext& a_context) = nullptr;
		};

		std::string_view name;
		std::shared_ptr<void> (*make)() = nullptr;
		Slots slots;
		Residency residency;
		Accumulate accumulate;
		Walk walk;
		DenseWalk dense;
		Retirement retirement;
		EntryClassHooks entryClass;
		Events events;
		Commit commit;
		Adoption adoption;
	};
}
