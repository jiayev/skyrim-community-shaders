// A view's tree LOD per face and the LOD it draws in the engine's place (IndirectDraws.h, "The view engine"): what the view features read of
// Core's tree LOD path and the LOD gates, under the LOD classes' contributions (Api::ObjectClass::TreeLod, LodObjects, LodTerrain, read as
// data; dclf-lod.md, "Water reflections"). A trees view's tree LOD pipeline, ownership and row
// (ResolveViewTreeLod, DecideViewTreeLod, ViewTreeRow), the cube camera's LOD roots its faces draw (ViewLodRootsOwned) and a LodTechniques
// view's phase readiness (LodPhaseReady). Render thread, but for LodPhaseReady (the scene lane's commit).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LodGates.h"

namespace DCLF
{
	bool IndirectDraws::ResolveViewTreeLod(Api::EngineView a_view, bool a_on, const PipelineCatalog* a_catalog)
	{
		auto& state = impl->View(a_view);
		bool waited = false;
		state.treePipeline = {};
		if (a_on && Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::TreeLod)) {
			// Its program is built of the DistantTree shader DecideTreeLod posts.
			auto& drawPipelines = DrawPipelines::Get();
			const ForwardPipelineKey treeKey{ 0, 0, ForwardPipelineKey::kForwardTreeLod };
			drawPipelines.RequestForward(treeKey);
			state.treePipeline = ForwardPipelineOf(a_catalog, treeKey, state.targets);
			const auto* entry = a_catalog ? a_catalog->FindForward(treeKey) : nullptr;
			waited = !state.treePipeline.valid() && impl->treeLodOwned && !(entry && entry->failed);
		}
		state.treeReady = state.treePipeline.valid();
		return waited;
	}

	void IndirectDraws::DecideViewTreeLod(Api::EngineView a_view)
	{
		// The faces' tree LOD: DCLF's while the main view's is (DecideTreeLod), the last depth commit uploaded the tables the faces draw
		// from, and the faces are drawn.
		auto& state = impl->View(a_view);
		const auto* scene = impl->scene.get();
		state.treeOwned = ViewDrawable(a_view) && impl->ViewCovered(a_view) && state.treeReady && impl->treeLodOwned && scene && scene->treeLodRow.shapeSlots &&
		                  scene->treeLodPipelines.load(std::memory_order_acquire);
		PassCapture::Get().SetViewTreeLodOwned(a_view, state.treeOwned);
	}

	TreeLod::DrawRow IndirectDraws::Impl::ViewTreeRow(ViewKind a_kind, const ViewFrame& a_shape, std::vector<std::shared_ptr<const void>>& a_owners)
	{
		// Tree LOD's row as the last depth commit uploaded it, its texture bound again for this execution, when the revision's faces have
		// tree LOD (their lists hold the scene's shape slots: one unit group) and the faces' tree LOD is DCLF's.
		if (!scene)
			return {};
		const auto& state = View(a_kind);
		const auto treeLod = scene->treeLodPipelines.load(std::memory_order_acquire);
		TreeLod::DrawRow treeRow = scene->treeLodRow;
		const bool trees = a_shape.tree.valid() && state.treePipeline.valid() && treeLod && scene->treeLodCull;
		if (!(trees && state.treeOwned && treeRow.shapeSlots && TreeLodTextureBinding(treeRow, a_owners)))
			treeRow.shapeSlots = 0;
		return treeRow;
	}

	std::uint32_t IndirectDraws::ViewLodRootsOwned(Api::EngineView a_view, bool a_plain)
	{
		// The roots whose members DCLF's faces draw, in a plain update of a frame whose faces are DCLF's (DecideCoverage):
		// - LOD trees while the faces' tree LOD is DCLF's (DecideViewTreeLod): drawn from DCLF's mirror, no registration needed.
		// - LOD land and objects while the engine's LOD swaps are gated (LodGates) and both are DCLF's (their classes): a
		//   swap's incoming blocks are shown only by the frame whose claims hold them, so the faces never need the engine's
		//   registrations of them. Ungated, the LOD chunks and objects the engine shows or attaches in motion take DCLF a few frames
		//   to bind (ClassifyResidue) and their roots stay, the members withheld (PassCapture).
		auto& capture = PassCapture::Get();
		capture.WatchReflectionResidue(0);
		if (!a_plain || !ViewDrawable(a_view) || !impl->ViewCovered(a_view) || !Policy::WithholdFromEngine())
			return 0;
		const auto toggles = ActiveToggles();
		const bool lodOwned = LodGates::Enabled() && Extensions::ObjectClassOn(toggles, Api::ObjectClass::LodObjects) &&
		                      Extensions::ObjectClassOn(toggles, Api::ObjectClass::LodTerrain);
		const std::uint32_t owned = (impl->View(a_view).treeOwned ? PassCapture::kTreeRoot : 0u) | (lodOwned ? PassCapture::kLodRoots : 0u);
		// The updates that keep the roots (Policy::KeepReflectionRoots): what the engine registers into the faces and DCLF does not
		// withhold is watched, by root - the LOD roots' (what skipping them loses: any is a defect once they are owned) and the tree root's.
		if (Policy::KeepReflectionRoots(impl->reflectionResidueUpdates)) {
			capture.WatchReflectionResidue(PassCapture::kLodRoots | owned);
			return 0;
		}
		return owned;
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
