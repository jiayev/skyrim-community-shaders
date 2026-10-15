#pragma once

// Core's fixed slot per draw family (Api::DrawFamily): what Core reserves for each, whichever feature takes it. The family's catalog
// pipelines are the pipeline lane's (DrawPipelines: its depth and colour pair, TreeLodPipelinesOf, and a view's forward pipeline by the
// slot's key flags), its main pass points are the main extension's and MainOpaquePass's (the stages around its draws at the depth
// segment's GpuStageSlots), its faces' points the view epochs' (ViewEpochRecord.cpp), its part of the revisions' shapes key the trees
// view's key group (MixViewShapesKey), and its ownership PassCapture's. A new family is the enum value, its slot here and its pipelines
// on the lane, then the feature's registration (Extensions::DrawFamilies).
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

	// ViewEpochRecord.cpp: a trees view's tree LOD cull (Core's TreeInstances family's faces.makeCull: DrawFamilies/TreeInstances.cpp).
	std::shared_ptr<org::RenderPass> MakeViewTreeCullPass(std::shared_ptr<ViewResources> a_resources);
}
