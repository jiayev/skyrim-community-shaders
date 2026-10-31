// A trees view's draw families per face (IndirectDraws.h, "The view engine"): what the view features read of Core's draw families' faces
// (Api/DrawFamilies.h: each family's slot and entry; tree LOD's), under each family's class (read as data; dclf-architecture.md, "Water
// reflections"). A trees view's families' pipelines, ownership and rows (ResolveViewFaces, DecideViewFaces, ViewFaceRows; none but in a
// trees view) and a LodTechniques view's phase readiness (LodPhaseReady); the roots its faces draw are ViewRoots.cpp's. Render thread,
// but for LodPhaseReady (the scene lane's commit).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF
{
	bool IndirectDraws::ResolveViewFaces(Api::EngineView a_view, bool a_on, const PipelineCatalog* a_catalog)
	{
		if (!TraitsOf(a_view).trees)
			return false;
		auto& state = impl->View(a_view);
		bool waited = false;
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			auto& faces = state.faces[d];
			faces.pipeline = {};
			// A family without faces (grass) has no forward pipeline: its slot's key flags are none (a Lighting key's).
			if (a_on && Extensions::DrawFamilyHasFaces(family) && Extensions::DrawFamilyOn(ActiveToggles(), family)) {
				// Its program is built of the family's program source its decision posts (DrawFamilyFrame::Pipelines: tree LOD's DistantTree
				// shader).
				auto& drawPipelines = DrawPipelines::Get();
				const ForwardPipelineKey key{ 0, 0, FamilySlotOf(family).forwardFlags };
				drawPipelines.RequestForward(key);
				faces.pipeline = ForwardPipelineOf(a_catalog, key, state.targets);
				const auto* entry = a_catalog ? a_catalog->FindForward(key) : nullptr;
				waited = (!faces.pipeline.valid() && impl->familyOwned[d] && !(entry && entry->failed)) || waited;
			}
			faces.ready = faces.pipeline.valid();
		}
		return waited;
	}

	void IndirectDraws::DecideViewFaces(Api::EngineView a_view)
	{
		if (!TraitsOf(a_view).trees)
			return;
		// A family's faces: DCLF's while the main view's family is (DecideDrawFamilies), the last depth commit uploaded the tables the faces
		// draw from (the family's faces.row, asked without owners: tree LOD's row names shape slots, its pipelines are in), and the faces are
		// drawn.
		auto& state = impl->View(a_view);
		const auto* scene = impl->scene.get();
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			const auto& faces = Extensions::DrawFamilyOf(family).contribution.faces;
			auto& own = state.faces[d];
			const auto rowDraws = [&] {
				own.row.assign(faces.rowBytes, std::byte{});
				Api::DrawFamilyFaceRow query{ *scene, true, own.row, nullptr };
				return faces.row && faces.row(query);
			};
			state.facesOwned[d] = ViewDrawable(a_view, ActiveToggles()) && impl->ViewCovered(a_view) && own.ready && impl->familyOwned[d] && scene && rowDraws();
			PassCapture::Get().SetViewFamilyOwned(a_view, family, state.facesOwned[d]);
		}
	}

	ViewFamilyRows IndirectDraws::Impl::ViewFaceRows(ViewKind a_kind, const ViewFrame& a_shape, std::vector<std::shared_ptr<const void>>& a_owners)
	{
		// Each faces' draw family's row (Api::DrawFamilyContribution::faces.row: tree LOD's as the last depth commit uploaded it, its
		// texture bound again for this execution), drawn when the revision's faces have it (their shape's pipeline), the view its pipeline,
		// and the faces' family is DCLF's; zero without the scene.
		ViewFamilyRows rows{};
		auto& state = View(a_kind);
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			if (!Extensions::DrawFamilyHasFaces(family))
				continue;
			const auto& faces = Extensions::DrawFamilyOf(family).contribution.faces;
			auto& own = state.faces[d];
			own.row.assign(faces.rowBytes, std::byte{});
			if (scene && faces.row) {
				Api::DrawFamilyFaceRow row{ *scene, a_shape.faces[d].pipeline.valid() && own.pipeline.valid() && state.facesOwned[d], own.row, &a_owners };
				faces.row(row);
			}
			rows[d] = own.row;
		}
		return rows;
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
