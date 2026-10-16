#pragma once

// Core's fixed slot per draw family (Api::DrawFamily): what Core reserves for each, whichever feature takes it. The family's catalog
// pipelines are the pipeline lane's (DrawPipelines: its depth and colour pair, CatalogFamilyPipelines, and a view's forward pipeline by
// the slot's key flags; the lane's part of the slot is Core/Gpu/Pipelines/DrawFamilyLaneSlots.h), its main pass points are the main
// extension's and MainOpaquePass's (the stages around its draws at the depth segment's GpuStageSlots), its faces' points the view
// epochs' (ViewEpochRecord.cpp), its part of the revisions' shapes key the trees view's key group (MixViewShapesKey), its ownership
// PassCapture's (IndirectDraws::DecideDrawFamilies publishes the feature's decision), and its pipelines' publication the scene's
// (SceneBuffers::familyPipelines). A new family is the enum value, its slot here and its lane slot, then the feature's registration
// (Extensions::DrawFamilies). A family's GPU layout is its feature's (TreeInstances': Features/TreeLod/TreeInstancesLayout.h), sized for
// Core by its faces' data.
// Part of IndirectDraws' implementation: included by Internal.h after Views/ViewSlots.h.

namespace DCLF::Draws
{
	/**
	 * @brief A family's slot: the forward key flags of a trees view's pipeline for it (ForwardPipelineKey: the lane's key). (Its faces'
	 * cull pass and root are its registration's: faces.cullName, faces.root.)
	 */
	struct FamilySlot
	{
		Api::DrawFamily family;
		std::uint32_t forwardFlags;
	};
	inline constexpr std::array<FamilySlot, Extensions::kDrawFamilies> kFamilySlots = { {
		{ Api::DrawFamily::TreeInstances, DrawFamilyLaneSlotOf(Api::DrawFamily::TreeInstances).forwardFlags },
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
	inline std::shared_ptr<const FamilyPipelines> FamilyPipelinesOf(const SceneBuffers& a_scene, Api::DrawFamily a_family)
	{
		return a_scene.familyPipelines[static_cast<std::size_t>(a_family)].load(std::memory_order_acquire);
	}
}
