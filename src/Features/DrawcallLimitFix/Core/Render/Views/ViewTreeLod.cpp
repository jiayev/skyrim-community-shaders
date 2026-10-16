// A view's tree LOD per face (IndirectDraws.h, "The view engine"): what the view features read of Core's tree LOD path (the TreeInstances
// draw family: Api/DrawFamilies.h, its slot and entry), under the family's class (read as data; dclf-lod.md, "Water reflections"). A trees
// view's tree LOD pipeline, ownership and row (ResolveViewTreeLod, DecideViewTreeLod, ViewTreeRow) and a LodTechniques view's phase
// readiness (LodPhaseReady); the LOD roots its faces draw are ViewRoots.cpp's. Render thread, but for LodPhaseReady (the scene lane's commit).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF
{
	bool IndirectDraws::ResolveViewTreeLod(Api::EngineView a_view, bool a_on, const PipelineCatalog* a_catalog)
	{
		auto& state = impl->View(a_view);
		bool waited = false;
		state.treePipeline = {};
		if (a_on && Extensions::DrawFamilyOn(ActiveToggles(), Api::DrawFamily::TreeInstances)) {
			// Its program is built of the family's program source its decision posts (DrawFamilyFrame::Pipelines: the DistantTree shader).
			auto& drawPipelines = DrawPipelines::Get();
			const ForwardPipelineKey treeKey{ 0, 0, FamilySlotOf(Api::DrawFamily::TreeInstances).forwardFlags };
			drawPipelines.RequestForward(treeKey);
			state.treePipeline = ForwardPipelineOf(a_catalog, treeKey, state.targets);
			const auto* entry = a_catalog ? a_catalog->FindForward(treeKey) : nullptr;
			waited = !state.treePipeline.valid() && impl->familyOwned[static_cast<std::size_t>(Api::DrawFamily::TreeInstances)] && !(entry && entry->failed);
		}
		state.treeReady = state.treePipeline.valid();
		return waited;
	}

	void IndirectDraws::DecideViewTreeLod(Api::EngineView a_view)
	{
		// The faces' tree LOD: DCLF's while the main view's is (DecideDrawFamilies), the last depth commit uploaded the tables the faces draw
		// from (the family's faces.row, asked without owners: its row names shape slots, its pipelines are in), and the faces are drawn.
		auto& state = impl->View(a_view);
		const auto* scene = impl->scene.get();
		const auto& trees = Extensions::DrawFamilyOf(Api::DrawFamily::TreeInstances).contribution.faces;
		const auto rowDraws = [&] {
			TreeInstances::DrawRow row{};
			Api::DrawFamilyFaceRow query{ *scene, true, row, nullptr };
			return trees.row && trees.row(query);
		};
		state.treeOwned = ViewDrawable(a_view) && impl->ViewCovered(a_view) && state.treeReady && impl->familyOwned[static_cast<std::size_t>(Api::DrawFamily::TreeInstances)] &&
		                  scene && rowDraws();
		PassCapture::Get().SetViewTreeLodOwned(a_view, state.treeOwned);
	}

	TreeInstances::DrawRow IndirectDraws::Impl::ViewTreeRow(ViewKind a_kind, const ViewFrame& a_shape, std::vector<std::shared_ptr<const void>>& a_owners)
	{
		// The faces' draw family's row (Api::DrawFamilyContribution::faces.row: tree LOD's as the last depth commit uploaded it, its texture
		// bound again for this execution), drawn when the revision's faces have it (their shape's pipeline), the view its pipeline, and
		// the faces' tree LOD is DCLF's.
		if (!scene)
			return {};
		const auto& state = View(a_kind);
		TreeInstances::DrawRow treeRow{};
		if (const auto& trees = Extensions::DrawFamilyOf(Api::DrawFamily::TreeInstances).contribution.faces; trees.row) {
			Api::DrawFamilyFaceRow row{ *scene, a_shape.tree.valid() && state.treePipeline.valid() && state.treeOwned, treeRow, &a_owners };
			trees.row(row);
		}
		return treeRow;
	}

	bool IndirectDraws::Impl::LodPhaseReady(std::uint8_t a_phase, const SceneTables& a_tables, std::uint32_t a_slot) const
	{
		// The view whose set phase a_phase is (the reflection's): its slot's forward pipeline in its state's slot pipelines.
		const auto& views = Extensions::Views();
		for (std::size_t v = 0; v < views.views.size(); ++v) {
			if (views.views[v].phase != a_phase)
				continue;
			const auto& slotPipelines = View(static_cast<ViewKind>(v)).slotPipelines;
			const std::uint32_t p = a_slot < a_tables.objects.size() ? a_tables.objects[a_slot].pipelineIndex : ~0u;
			return p < slotPipelines.size() && slotPipelines[p].valid();
		}
		return false;
	}
}
