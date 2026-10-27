#pragma once

// The CPU-driven pipeline's pre-work (Direct.h): the GPU work a direct site still records before its draws, where no ORG epoch runs.
// At each site (Sites.cpp) the render thread prepares it after the site's commits, from the shapes they wrote, and DXVK's CS thread
// records it first in the site's callback, into DXVK's own command buffer, outside any render pass.
//
// What is pre-work is the schedule's (Schedule/Frame.cpp) passes with CPU-mode work (Api::PassDef::direct), in its order; every other
// pass of the site's epochs is the CPU's in this mode or a draw the sites record:
// - shadow (shadow-maps.after): shadows.latched-copies, shadows.index-pool. Not the build (the CPU's cull), the views' draws (the
//   sites'), the cull counters (the CPU's counters).
// - main.depth (depth.committed): core.z.latched-copies (the hosted views' committed lists with it), texture-transforms.apply,
//   core.z.index-pool (when the depth commit gave out ranges), tree-lod.cull (phase 1, no HZB), grass.cull (phase 1, no HZB), grass.args,
//   tree-wind.next-frame (on the graphics queue here, still writing the next frame's buffer by parity), grass.args-colour (it zeroes the
//   counts the next frame's cull appends with), and the hosted views' reflection/ and fp.depth/tree-lod.faces-cull. Not the member and
//   fade portals' queries, batches and evaluations, the builds, fades.state, the HZB, phase 2, the forward sort, the visibility
//   events, their readback and fades.events (the CPU's: Cull.cpp, the CPU fade stage), nor the tree LOD and grass counts' readbacks
//   (report counters of the GPU cull's phases).
// - main.colour (opaque.after): core.latched-copies. Not the build, the sorts, the forward sort.
// - forward (blended.draw) and fp.colour (fp.colour-render): none (draws only; their shapes' latched copies are main.colour's and
//   main.depth's).
//
// The latched copies stay: the commits still latch the frame constants, the frame buffers (t16 and up), the views' blocks and the
// families' rows and list headers through them, which the direct draws read as the GPU path's do. Their count words are the GPU
// cull's and are copied with them, unread.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"

#include <rhi.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace org::services
{
	class NamedVersions;
}

namespace DCLF::Draws
{
	struct Resources;
	struct RevisionShapes;
	struct ShadowResources;
}

namespace DCLF::Direct
{
	/**
	 * @brief What a site's pre-work is prepared from: render thread, at the site's trigger, after its commits ran (they write the latch
	 * blocks, the latched lists, the index pool's dispatch and the families' rows, as in the GPU path; only the submission is the site's).
	 */
	struct PreWorkInputs
	{
		// The set the frame adopted (IndirectDraws::Impl::Adopted): the versions the commits wrote into. Held by the caller until the
		// site's callback ran.
		const org::services::NamedVersions* adopted = nullptr;
		std::shared_ptr<Draws::Resources> main;          // the Depth, Colour, Forward and FirstPerson sites'
		std::shared_ptr<Draws::ShadowResources> shadow;  // the Shadow site's
		// The shapes the commits wrote, as the GPU path's recording would be selected for them: Depth, main[kDepthShape] (the depth
		// commit's), views[v] of each hosted view that committed in it this frame (none for the others), depthWithoutIndexPool when the
		// depth commit gave out no index pool range (UpdateIndexPool returned 0); Colour and Forward, main[kColourShape]; Shadow, shadow
		// (the shadow commit's variant); FirstPerson, views[FirstPerson]. Not held past the call: the prepared work holds what it reads.
		const Draws::RevisionShapes* shapes = nullptr;
		// The frame slot whose latch regions the commits wrote (the host's CurrentFrameSlot at the commits).
		std::uint32_t frameSlot = 0;
	};

	/**
	 * @brief A site's prepared pre-work: immutable once prepared, read on DXVK's CS thread. Each record is one pass's commands (or a part
	 * of them), in the schedule's order; passEnds says where each pass's records end.
	 */
	struct PreWork
	{
		Site site = Site::Depth;
		std::vector<std::function<void(rhi::CommandList&)>> records;
		std::vector<std::uint32_t> passEnds;
		// The heaps the site's shape names (the frame's resource and sampler heaps), which the passes' descriptors index: RecordPreWork
		// binds them first.
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		// What the records' addresses and descriptors are of (the main, shadow and hosted views' resources: the frame constants, the
		// latch blocks, the views' buffers), held with the work: DXVK's CS thread records it after the render thread may have replaced
		// them (a graph build makes new main resources), and the site's run keeps the work until its frame slot retires.
		std::vector<std::shared_ptr<const void>> holds;
	};

	/**
	 * @brief Render thread, at a_site's trigger after its commits: the site's pre-work from a_inputs (the site's epochs' passes with
	 * CPU-mode work: PreWork.h's list). Null when it has none (no adopted set or shapes, or nothing to record this frame). Allocates
	 * the closures; nothing it holds is the render thread's after it returns.
	 */
	std::shared_ptr<const PreWork> PreparePreWork(Site a_site, const PreWorkInputs& a_inputs);

	/**
	 * @brief DXVK's CS thread, inside the site's callback, first (before the site's entry barriers and rendering): records a_work onto
	 * a_list, DXVK's command buffer adopted as an rhi::CommandList, outside any render pass. Ordered as ORG would order the passes' uses:
	 * a barrier before it (DXVK's earlier work and the frames before: every write before against the copies and dispatches, and their
	 * writes against the earlier reads), one between consecutive passes (copy and compute writes against the next pass's copies and
	 * dispatches), and one after it (copy and compute writes against the draws' index, vertex, pixel, indirect-argument and compute
	 * reads). Binds the frame's heaps (resourceHeap, samplerHeap), and pipelines, layouts and push constants of its own: the site binds
	 * its layout and push data after it.
	 */
	void RecordPreWork(const PreWork& a_work, rhi::CommandList& a_list);

	// The sites' calls, by name (PreparePreWork's site).
	inline std::shared_ptr<const PreWork> PrepareShadowPreWork(const PreWorkInputs& a_inputs) { return PreparePreWork(Site::Shadow, a_inputs); }
	inline std::shared_ptr<const PreWork> PrepareDepthPreWork(const PreWorkInputs& a_inputs) { return PreparePreWork(Site::Depth, a_inputs); }
	inline std::shared_ptr<const PreWork> PrepareColourPreWork(const PreWorkInputs& a_inputs) { return PreparePreWork(Site::Colour, a_inputs); }
	inline std::shared_ptr<const PreWork> PrepareForwardPreWork(const PreWorkInputs& a_inputs) { return PreparePreWork(Site::Forward, a_inputs); }
	inline std::shared_ptr<const PreWork> PrepareFirstPersonPreWork(const PreWorkInputs& a_inputs) { return PreparePreWork(Site::FirstPerson, a_inputs); }
}
