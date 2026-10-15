// Reflection: the water reflection's cube map faces drawn by DCLF as a view of the one scene. Its switch and the reflections toggle, the
// view's contribution and its work after tree LOD's decision and at the depth commit, its engine hooks (the feature's early hooks), its
// report lines and the faces' camera parity.

// ReflectionView.h first (Internal.h): volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionView.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"

#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <string>

namespace DCLF::Features::Reflection
{
	namespace
	{
		// The view's lines (its faces, their pipelines and epochs, the engine's registrations of its members, the LOD roots and the
		// parity), taking its counters.
		void Lines(std::string& a_text)
		{
			a_text += Report();
		}

		/**
		 * @brief The view's contribution: the LOD techniques of the main set; keys for a LOD Lighting technique's slot only, front faces culled
		 * unless two-sided (dclf-lod.md, "Water reflections"); six faces with tree LOD; its last shape kept while off; its imported target's
		 * area; withheld in renderers of its own (the cube camera's accumulators, ReflectionFaces); drawn into the engine's cube with DCLF's
		 * own depth; covered before its first coverage and on a failed graph.
		 */
		Api::ViewContribution Contribution(Api::ToggleId a_toggle)
		{
			return { .view = kView,
				.toggle = a_toggle,
				.phase = kSetReflection,
				.membership = Api::ViewMembership::LodTechniques,
				.keys = { .lodTechniquesOnly = true },
				.shape = { .faces = kFaces,
					.trees = true,
					.viewBits = kViewReflection,
					.name = "reflection",
					.prefix = "cs.dclf.reflection",
					.blocks = "face-blocks",
					.target = "cube",
					.labels = { "reflection bucket map", "reflection bucket tables", "reflection culling latches", "reflection face per-frame data",
						"reflection tree rows" } },
				.life = Api::ViewShapeLife::KeepLast,
				.area = Api::ViewArea::Target,
				.withholding = Api::ViewWithholding::OwnRenderers,
				.targetImport = Api::ViewTargetImport::ColourWithOwnDepth,
				.coveredInitially = true,
				.coveredOnFailedGraph = true,
				.covered = &Covered };
		}

		/**
		 * @brief The water reflection's hooks (the update, its LOD add-roots, the face render, its orientation and draws), first of the early
		 * hooks, before Core's observers and engine side: DCLF's view of each face, the observers' capture of it, and the cube camera's LOD
		 * roots DCLF draws in its place, while DCLF runs (ShellState::Running).
		 */
		void InstallHooks()
		{
			ReflectionFaces::Install();
			ReflectionFaces::SetFaceOriented([](const RE::NiCamera& a_camera, std::uint32_t a_face) {
				RenderThreadBudget::Hook budget(RenderThreadBudget::Site::ReflectionFace);
				if (ShellState::Running())
					FaceOriented(a_camera, a_face);
			});
			ReflectionFaces::SetAfterFaceDraws([] {
				RenderThreadBudget::Hook budget(RenderThreadBudget::Site::ReflectionFace);
				if (ShellState::Running())
					FaceDrawn();
			});
			ReflectionFaces::SetRootsOwned([](bool a_plain) {
				RenderThreadBudget::Hook budget(RenderThreadBudget::Site::ReflectionFace);
				return ShellState::Running() ? IndirectDraws::Get().ViewLodRootsOwned(kView, a_plain) : 0u;
			});
		}

		void Install(Api::InstallContext& a_context)
		{
			// Where Core's was listed: after the precipitation mask's.
			const auto reflectionsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_REFLECTIONS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: the water reflection's faces stay native (live toggle's seed)", .after = "CS_DCLF_PRECIPITATION" });
			// The water reflection's cube map faces' LOD drawn by DCLF from its own tables (dclf-lod.md, "Water reflections"). Where Core's was
			// listed, after the precipitation occlusion's, opening its section; the first feature toggle registered, so its bit in the packed
			// word is where Core's was.
			const auto reflections = a_context.RegisterToggle({ .name = "reflections", .label = "Draw the water reflection's distant land, objects and trees (CS_DCLF_REFLECTIONS)",
				.tooltip = "DCLF draws the LOD the water's cube map reflects from its own tables on the GPU; the engine draws the sky, and any LOD DCLF cannot draw yet.",
				.section = "Reflections", .seed = reflectionsSwitch, .after = "precipitation occlusion" });
			// The view (Core's view machinery: its epochs, revisions, set decisions and withholding read it as data).
			a_context.RegisterView(Contribution(reflections));
			// The faces' targets, the LOD slots' forward pipelines and tree LOD's, and the view's resources, after tree LOD's decision; the
			// frame's faces' epoch right after the depth commit.
			a_context.RegisterViewWork({ .view = kView, .point = Api::ViewPoint::FrameStartAfterTreeLod, .run = &Prepare });
			a_context.RegisterViewWork({ .view = kView, .point = Api::ViewPoint::DepthCommitted, .run = &Execute });
			// After the visibility lines and before the forward epoch's, where Core printed the view's.
			a_context.RegisterReportLines({ .name = "reflection", .point = Api::ReportPoint::AsyncWorkViews, .text = &Lines });
			// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: the faces' cameras against the engine's.
			if (Diagnostics::Observe().claimsOrScene)
				SubscribeFaceCameraParity();
		}
	}

	Api::Feature Feature()
	{
		return { "reflection", &Install, &InstallHooks };
	}
}
