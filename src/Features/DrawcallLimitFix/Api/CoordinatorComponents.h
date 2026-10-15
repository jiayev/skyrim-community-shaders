#pragma once

// The coordinator component contract (Api::InstallContext::RegisterCoordinatorComponent): a feature's state kept by the scene
// coordinator beside its residents, and the hooks the coordinator calls where a slot's residency changes - what a feature lists for
// the scene's members (its table family's rows, the engine nodes it owns, the seeds it asks the frame for) follows the members.

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

	/** @brief A registered component (RegisterCoordinatorComponent's result): FrameView::TakeComponentSeeds takes its seeds by it. */
	struct CoordinatorComponentId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
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
	};
}
