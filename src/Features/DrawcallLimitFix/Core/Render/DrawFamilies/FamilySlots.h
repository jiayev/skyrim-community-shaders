#pragma once

// Core's fixed slot per draw family (Api::DrawFamily): what Core reserves for each, whichever feature takes it. The family's catalog
// pipelines are the pipeline lane's (DrawPipelines: its depth and colour pair, TreeLodPipelinesOf, and a view's forward pipeline by the
// slot's key flags), its main pass points are the main extension's and MainOpaquePass's (the stages around its draws at the depth
// segment's GpuStageSlots), its faces' points the view epochs' (ViewEpochRecord.cpp), its part of the revisions' shapes key the trees
// view's key group (MixViewShapesKey), its ownership PassCapture's (IndirectDraws::DecideDrawFamilies publishes the feature's decision),
// and its pipelines' publication the scene's (SceneBuffers::familyPipelines). A new family is the enum value, its slot here and its
// pipelines on the lane, then the feature's registration (Extensions::DrawFamilies). The TreeInstances family's GPU layout is
// TreeInstancesLayout.h.
// Part of IndirectDraws' implementation: included by Internal.h after Views/ViewSlots.h.

namespace DCLF::Draws
{
	/**
	 * @brief A family's slot: the forward key flags of a trees view's pipeline for it (ForwardPipelineKey: the lane's key), its cull pass
	 * in a trees view's depth epoch (after the view's pass prefix), and the root PassCapture's view withholding owns while a trees view's
	 * faces draw it (ViewLodRootsOwned).
	 */
	struct FamilySlot
	{
		Api::DrawFamily family;
		std::uint32_t forwardFlags;
		const char* facesCull;
		std::uint32_t lodRoot;
	};
	inline constexpr std::array<FamilySlot, Extensions::kDrawFamilies> kFamilySlots = { {
		{ Api::DrawFamily::TreeInstances, ForwardPipelineKey::kForwardTreeLod, "tree-lod-cull", PassCapture::kTreeRoot },
	} };
	static_assert(kFamilySlots[static_cast<std::size_t>(Api::DrawFamily::TreeInstances)].family == Api::DrawFamily::TreeInstances, "kFamilySlots is by family");

	inline const FamilySlot& FamilySlotOf(Api::DrawFamily a_family) { return kFamilySlots[static_cast<std::size_t>(a_family)]; }

	/**
	 * @brief Whether a_family is set up in a_scene: taken, and its scene resources' state made (Api::DrawFamilyContribution::resources:
	 * its tables; none, as without them, leaves out its decision, its draws and a trees view's lists).
	 */
	inline bool FamilySetUp(const SceneBuffers& a_scene, Api::DrawFamily a_family)
	{
		const auto& entry = Extensions::DrawFamilyOf(a_family);
		return entry.taken && Extensions::SceneResourcesStateOf(a_scene, entry.contribution.resources) != nullptr;
	}

	/** @brief a_family's pipelines as the lane published them for the passes (DrawFamilyFrame::Pipelines; null: none yet). Any thread. */
	inline std::shared_ptr<const TreeLodPipelines> FamilyPipelinesOf(const SceneBuffers& a_scene, Api::DrawFamily a_family)
	{
		return a_scene.familyPipelines[static_cast<std::size_t>(a_family)].load(std::memory_order_acquire);
	}
}
