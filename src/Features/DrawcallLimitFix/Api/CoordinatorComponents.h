#pragma once

// The coordinator component contract (Api::InstallContext::RegisterCoordinatorComponent): a feature's state kept by the scene
// coordinator beside its residents, and the hooks the coordinator calls where a slot's residency changes - what a feature lists for
// the scene's members (its table family's rows, the engine nodes it owns, the seeds it asks the frame for) follows the members - and at
// its walk's phases, its entries' erasure and the writes of the entry class it follows.

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
	 * @brief A class of tracked entries Core's walk classifies but whose records wait on a component: the component that follows it
	 * (CoordinatorComponent::follows) supplies what their writes need, and its objects are DCLF's only while that component takes it.
	 */
	enum class EntryClass : std::uint8_t
	{
		None,
		// A dynamic shape under a BSFaceGenNiNode (an NPC's head, mouth, hair, brows): its positions are its head's, published by the
		// engine's face morphing, not its buffers'. Its record waits for its second vertex stream (Features/Faces).
		FaceShape,
		Count,
	};

	/**
	 * @brief A coordinator component.
	 *
	 * Every hook runs on the scene lane, inside a pass, with the coordinator's live tables (CoordinatorComponentContext::tables), in
	 * registration order at its point, at exactly the place the coordinator's own residency work runs there. Residency changes are events
	 * (joins, leaves, answers), so a hook is called per changed slot, never per object per frame.
	 *
	 * - make: the component's state, once, before its first hook; held by the coordinator for the session.
	 * - resident (optional): a_slot is resident: it joined (or a resident's patch changed: MarkResidentSlot), or the render thread
	 *   answered the fade node its entry asked for (ApplyFadeNodeAnswers). Lists what the slot needs; called again for a slot already
	 *   listed. Right after the slot's resident position is grown, before its patch is stored.
	 * - left (optional): a_slot's residency ended (DropResidentSlot, after its resident position is cleared and before the primary cull is
	 *   told; EndAllResidency, per resident, before its accumulated half is reset). Unlists what it listed.
	 * - recount (optional): the residents kept alive (KeepResidentsAlive), after the coordinator counted the slots touched since the last
	 *   one (or, rebuilt, every resident), before the used sets: the component recounts the same slots (ResidentRecount).
	 * - reset (optional): the slot tables reset (ResetSlotTables), before the tables are cleared: the component's references are handed
	 *   back (through the retirement chain, with everything else the tables named).
	 * - allLeft (optional): every residency ends (EndAllResidency), before left is called for each resident: what the component keeps of
	 *   the members goes whole.
	 * - accumulated (optional): the accumulate phase wrote a_slot's accumulated half from its patch (a join, or a resident's patch that
	 *   changed: ApplyAccumulatePatch), after the write: the patch's values the component lists by (AccumulatedPatch).
	 * - rewritten (optional): the walk rewrote a_slot's scene half (its write, or a layer's), or refreshed its skin partitions in place
	 *   (SlotRewrite), where the coordinator writes them. Called per written slot (the walk writes the slots its events changed).
	 * - joinsDone (optional): the accumulate phase's joins done (the residents kept alive, the slots checked), in its statistics: the
	 *   component brings what it derives from the members up to date, writing the live tables (noting their changes).
	 *
	 * The walk's phases (each walk: the delta walk's and the walk parity's dense walk):
	 * - walkBegun (optional): the walk begins (BeginWalk), its frame tables cleared and its serial moved on: the component takes the
	 *   events it follows since the last walk.
	 * - scheduling (optional): the delta walk schedules its entries (WalkScheduling): a full evaluation, where nothing kept is trusted, or,
	 *   after the pending entries, the component's events' writes (ScheduleWrite) and its in-place updates, counted.
	 * - walkEnded (optional): the walk's end (after the candidates; the dense walk's after its sweep): what it retired this walk.
	 * - denseBegun, denseEnded (optional): the walk parity's dense walk is about to run, and has run (DenseReference): what the component
	 *   keeps beside the tables that the dense walk writes is saved, then restored.
	 *
	 * The entries (every entry, whatever its class):
	 * - forgot (optional): a tracked entry is erased (EraseTracked, after its slot is released): what the component keeps of it goes.
	 * - forgotAll (optional): every tracked entry is gone (the rescan, after the tracked set is cleared).
	 * - recycled (optional): a range the component retired (RetireRange) is free: no publication names it any more (RecycleRetired).
	 *
	 * The class it follows (follows: one component's; None: no entries' writes), on the merge's writes of the class's entries, each a
	 * changed slot (the walk writes the slots its events changed):
	 * - takes: whether the class's objects are DCLF's (Core's eligibility and walk read it as data: Extensions::EntryClassTaken). The hooks
	 *   below are called whatever it says.
	 * - listed (optional): the walk writes an entry of the class (every write, before its verdict's record): the component lists it.
	 * - resolve (optional): a record of a taken entry is being written, before its slot is taken: the entry's second vertex stream into
	 *   EntryWrite; false: the record waits (no record this walk; the component's next event writes it again).
	 * - streamed (optional): the record resolve gave a stream to took a_slot (where the walk writes its scene half): the component points
	 *   the slot at the stream.
	 */
	struct CoordinatorComponent
	{
		std::string_view name;
		std::shared_ptr<void> (*make)() = nullptr;
		void (*resident)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot) = nullptr;
		void (*left)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot) = nullptr;
		void (*recount)(const CoordinatorComponentContext& a_context, const ResidentRecount& a_recount) = nullptr;
		void (*reset)(const CoordinatorComponentContext& a_context) = nullptr;
		void (*allLeft)(const CoordinatorComponentContext& a_context) = nullptr;
		void (*accumulated)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot, const AccumulatedPatch& a_patch) = nullptr;
		void (*rewritten)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot, const SlotRewrite& a_rewrite) = nullptr;
		void (*joinsDone)(const CoordinatorComponentContext& a_context) = nullptr;
		void (*walkBegun)(const CoordinatorComponentContext& a_context) = nullptr;
		void (*scheduling)(const CoordinatorComponentContext& a_context, WalkScheduling& a_scheduling) = nullptr;
		void (*walkEnded)(const CoordinatorComponentContext& a_context) = nullptr;
		void (*denseBegun)(const CoordinatorComponentContext& a_context) = nullptr;
		void (*denseEnded)(const CoordinatorComponentContext& a_context) = nullptr;
		void (*forgot)(const CoordinatorComponentContext& a_context, const void* a_entry) = nullptr;
		void (*forgotAll)(const CoordinatorComponentContext& a_context) = nullptr;
		void (*recycled)(const CoordinatorComponentContext& a_context, std::uint32_t a_first, std::uint32_t a_count) = nullptr;
		EntryClass follows = EntryClass::None;
		bool takes = false;
		void (*listed)(const CoordinatorComponentContext& a_context, const EntryWrite& a_write) = nullptr;
		bool (*resolve)(const CoordinatorComponentContext& a_context, EntryWrite& a_write) = nullptr;
		void (*streamed)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot, EntryWrite& a_write) = nullptr;
	};
}
