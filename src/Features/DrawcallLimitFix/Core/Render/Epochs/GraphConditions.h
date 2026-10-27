#pragma once

// The frame facts the graph's passes are added by (PassContext::Interior, PassContext::PhaseTwo): what a frame needs decides which
// passes exist, not a pass that runs and finds nothing to do (each pass costs its dispatch and its barriers whether it has work or not).

#include <string>
#include <string_view>

namespace org
{
	class PersistentGraphHost;
}

namespace DCLF
{
	struct ToggleSet;
}

namespace DCLF::Draws
{
	struct Resources;
	struct PassFrame;

	struct GraphConditions
	{
		bool interior = false;         // the current cell is an interior: no grass and no tree LOD (their passes are left out)
		bool twoPassOcclusion = true;  // the cull mode tests occlusion (cullMode 2): the HZB and the depth epoch's phase 2 block
		// The CPU-driven pipeline (CpuPipelineFor the cell: Toggles.h; Core/Render/Direct): a DCLF epoch keeps only its passes in the CPU
		// pipeline (Api::PassDef::cpuPipeline: the latched and index pool copies, texture transforms, the object rows, tree LOD's and
		// grass's single-phase culls and arguments, tree wind), as external epochs (org::PersistentGraphHost::SetExternalEpochs:
		// CpuPipelineEpochs), which the revisions record and the sites execute in DXVK's command buffer before their draws
		// (Direct::ExecuteEpoch). The draws (main depth and colour, shadow and occlusion maps, forward, the reflection's faces, first
		// person's depth and colour) are the direct sites'; the builds, the HZB and phase 2, the visibility and fade-event readbacks and
		// fade state are left out (the CPU's).
		bool cpuPipeline = false;
		std::string cuts;              // diagnostics (dclf:cut=): comma-separated pass key prefixes left out ("*": every pass)

		bool operator==(const GraphConditions&) const = default;
	};

	/** @brief Whether the device makes every cell CPU-driven: no device-generated commands for the GPU path's colour (DrawPipelines::DeviceGenerated). */
	bool CpuPipelineForced();

	/** @brief Whether a_set draws a cell of a_interior CPU-driven (CpuPipelineFor: Toggles.h), or the device forces it (CpuPipelineForced). */
	bool CpuPipelineForCell(const ToggleSet& a_set, bool a_interior);

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

	/**
	 * @brief Any thread: whether the graph as built is the CPU pipeline's (GraphConditions::cpuPipeline), which decides the frame's
	 * pipeline: the direct sites draw while it holds, the epochs otherwise (the frame's mode, Direct::FrameMode). Set at the build point.
	 */
	bool GraphBuiltForCpu();

	/** @brief The conditions the graph is built for (render thread: the passes' makes and preparations). */
	const GraphConditions& CurrentGraphConditions();

	/**
	 * @brief Whether a phase 1 cull tests occlusion against the HZB: the pyramid exists, the frame's cull mode tests occlusion and the
	 * graph was built with the HZB pass (else the pyramid holds an old frame's depth).
	 */
	bool OcclusionTested(const Resources& a_resources, const PassFrame& a_frame);
}
