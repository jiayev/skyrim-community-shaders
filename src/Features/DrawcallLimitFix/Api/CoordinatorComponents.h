#pragma once

// The coordinator component contract (Api::InstallContext::RegisterCoordinatorComponent): a feature's state kept by the scene
// coordinator beside its residents, and the hooks the coordinator calls at its points - what a feature lists for the scene's members (its
// table family's rows, the engine nodes it owns, the seeds it asks the frame for) follows the members. The hooks come in sets, one per
// concern of the coordinator's: residency, the accumulate phase, the walk, the walk parity's dense walk, retirement, and the writes of the
// entry class the component follows (Api::EntryClass, Api/ObjectContributors.h).

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"

#include <cstdint>
#include <memory>
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
	 * @brief A coordinator component: its name, its state, and its hook sets (each below, documented once).
	 *
	 * What every hook shares:
	 * - Thread: the scene lane, inside a pass.
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
		 * - forgotAll: every tracked entry is gone (the rescan, after the tracked set is cleared).
		 * - recycled: a range the component retired (CoordinatorComponentContext::RetireRange) is free: no publication names it any more
		 *   (RecycleRetired). The retiring component's alone.
		 */
		struct Retirement
		{
			void (*reset)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*forgot)(const CoordinatorComponentContext& a_context, const void* a_entry) = nullptr;
			void (*forgotAll)(const CoordinatorComponentContext& a_context) = nullptr;
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

		std::string_view name;
		std::shared_ptr<void> (*make)() = nullptr;
		Residency residency;
		Accumulate accumulate;
		Walk walk;
		DenseWalk dense;
		Retirement retirement;
		EntryClassHooks entryClass;
	};
}
