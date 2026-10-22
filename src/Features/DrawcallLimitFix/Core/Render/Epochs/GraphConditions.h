#pragma once

// The frame facts the graph's passes are added by (PassContext::Interior, PassContext::PhaseTwo): what a frame needs decides which
// passes exist, not a pass that runs and finds nothing to do (each pass costs its dispatch and its barriers whether it has work or not).

#include <string>
#include <string_view>

namespace org
{
	class PersistentGraphHost;
}

namespace DCLF::Draws
{
	struct Resources;
	struct PassFrame;

	struct GraphConditions
	{
		bool interior = false;         // the current cell is an interior: no grass and no tree LOD (their passes are left out)
		bool twoPassOcclusion = true;  // the cull mode tests occlusion (cullMode 2): the HZB and the depth epoch's phase 2 block
		std::string cuts;              // diagnostics (dclf:cut=): comma-separated pass key prefixes left out ("*": every pass)

		bool operator==(const GraphConditions&) const = default;
	};

	/**
	 * @brief Render thread, at the frame's build point before it builds (IndirectDraws::BuildPoint): this frame's conditions. A change
	 * from the ones the graph was built for requests its rebuild, which that build point makes, so a frame's passes are its conditions'.
	 */
	void RefreshGraphConditions(org::PersistentGraphHost& a_host);

	/** @brief Any thread (diagnostics, dclf:cut=): the pass key prefixes the next build point leaves out; empty: none. */
	void SetPassCuts(std::string a_cuts);

	/** @brief Whether the graph's conditions leave the pass a_key out (its cuts). */
	bool PassCut(std::string_view a_key);

	/**
	 * @brief Any thread, recording (diagnostics: the cut "~latched-record", which "*" does not imply): the latched-copies passes record no
	 * copy. Unsafe with draws (their constants and counters are not reset): timing an empty-epoch run only.
	 */
	bool LatchedRecordCut();

	/** @brief The conditions the graph is built for (render thread: the passes' makes and preparations). */
	const GraphConditions& CurrentGraphConditions();

	/**
	 * @brief Whether a phase 1 cull tests occlusion against the HZB: the pyramid exists, the frame's cull mode tests occlusion and the
	 * graph was built with the HZB pass (else the pyramid holds an old frame's depth).
	 */
	bool OcclusionTested(const Resources& a_resources, const PassFrame& a_frame);
}
