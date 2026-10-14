#pragma once

// The main extension's passes, made by the sources that define them (MainExtension.cpp adds them to the graph).
// Included by the sources of this folder only, after Internal.h.

#include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"

namespace DCLF::Draws
{
	// MainOpaque.cpp: DCLF's draws in the depth segment (a_phaseTwo: phase 2's rescues) or the main colour pass.
	std::shared_ptr<org::RenderPass> MakeMainOpaquePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, bool a_phaseTwo = false);

	// Forward.cpp: the forward epoch's blended draws and refraction normals.
	std::shared_ptr<org::RenderPass> MakeForwardDrawPass(std::shared_ptr<Resources> a_resources);
	std::shared_ptr<org::RenderPass> MakeRefractionDrawPass(std::shared_ptr<Resources> a_resources);

	// Build.cpp: BuildDraws (a_phase 0 follows the segment's shape; 2 is the phase 2 retest), the sort's scatter, tree LOD's cull.
	std::shared_ptr<org::RenderPass> MakeBuildDrawsPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, std::uint32_t a_phase = 0);
	std::shared_ptr<org::RenderPass> MakeSortSequencesPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment);
	std::shared_ptr<org::RenderPass> MakeTreeLodCullPass(std::shared_ptr<Resources> a_resources, std::uint32_t a_phase);

	// Occlusion.cpp: the HZB rebuild and the visibility events.
	std::shared_ptr<org::RenderPass> MakeHzbPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment);
	std::shared_ptr<org::RenderPass> MakeVisibilityEventsPass(std::shared_ptr<Resources> a_resources);

	// SceneState.cpp: next frame's tree wind and fade states.
	std::shared_ptr<org::RenderPass> MakeTreeWindPass(std::shared_ptr<SceneBuffers> a_scene);
	std::shared_ptr<org::RenderPass> MakeFadeStatePass(std::shared_ptr<Resources> a_resources);

	// Readbacks.cpp: tree LOD's counts and an event ring's copy to the host.
	std::shared_ptr<org::RenderPass> MakeTreeLodReadbackPass(std::shared_ptr<SceneBuffers> a_scene);
	std::shared_ptr<org::RenderPass> MakeEventReadbackPass(std::shared_ptr<SceneBuffers> a_scene, Versioned a_events, std::shared_ptr<HostEventRing> a_ring);
}
