// FirstPerson: the player's first-person skeleton drawn by DCLF as a view of the one scene. Its switches and the first person toggle, the
// view's contribution and its work at the frame's start, its engine hooks, its report lines and the skeleton's parity.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <string>

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		// The view's lines (its epochs, its forward part, the engine's registrations of its members), taking its counters.
		void Lines(std::string& a_text)
		{
			a_text += Report();
		}

		/**
		 * @brief The view's contribution: a category root (the first-person root, filed under itself) with an exclusive phase; keys for
		 * every slot but a forward one (its forward part draws those), with the first-person targets (its key bit) and back faces culled; one face with a forward part and a
		 * depth pass; dropped while off; the main colour viewport; withheld in the main accumulators (and the first-person one); drawn
		 * into kPOST_ZPREPASS_COPY; not covered before its first coverage nor on a failed graph (the engine draws it).
		 */
		Api::ViewContribution Contribution(Api::ToggleId a_toggle, bool a_forwardDecals)
		{
			return { .view = Api::EngineView::FirstPerson,
				.toggle = a_toggle,
				.forwardDecals = a_forwardDecals,
				.phase = kSetFirstPerson,
				.membership = Api::ViewMembership::CategoryRoot,
				.keys = { .forwardRasterToForwardPart = true, .keyFlags = kForwardKeyFirstPerson, .cullBackFaces = true },
				.shape = { .faces = 1,
					.forwardPart = true,
					.depthPass = true,
					.viewBits = kViewFirstPerson,
					.name = "first-person",
					.prefix = "cs.dclf.first-person",
					.blocks = "block",
					.target = "depth-copy",
					.labels = { "first-person bucket map", "first-person bucket table", "first-person culling latch", "first-person per-frame data",
						"first-person tree rows" },
					.depthRange = 0.1f,
					// A handful of objects, always before the first-person camera: drawn untested. The frustum test dropped the weapon in
					// some places and orientations (its skinned bound against the first-person camera's frustum).
					.frustumCull = false },
				.life = Api::ViewShapeLife::DropWhenOff,
				.area = Api::ViewArea::MainColourViewport,
				.withholding = Api::ViewWithholding::InMainRenderers,
				.targetImport = Api::ViewTargetImport::DepthTarget,
				.coveredInitially = false,
				.coveredOnFailedGraph = false,
				.covered = &Covered,
				.captureRoot = &CaptureRoot,
				.accumulators = &Accumulators };
		}

		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: after the reflections' (Features/Reflection's, registered earlier before the same Core switch).
			const auto firstPersonSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_FIRST_PERSON", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: the player's first-person arms, weapons and magic hands drawn by DCLF as a view of the one scene (T7e: kSetFirstPerson members, the view bit kViewFirstPerson at the GPU cull, its depth in the Z-prepass epoch and a colour epoch at Main::RenderFirstPersonView); unset or 0 they stay native: a few dozen draws, cheaper as the engine's own than as two epochs of GPU-driven work (live toggle's seed)",
				.before = "CS_DCLF_OPAQUE_EFFECTS" });
			const auto decalsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_FIRST_PERSON_DECALS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on (T7f item 7): a decal on a first-person entry (an effect's skinned blood decal on the arms, a weapon's BloodLighting) drawn by the first-person forward part as a forward object with its decal group's depth bias, test and blend (needs CS_DCLF_FIRST_PERSON and CS_DCLF_DECALS); 0 it stays the engine's",
				.before = "CS_DCLF_OPAQUE_EFFECTS" });
			// The player's first-person skeleton (arms, weapons, shield, magic hands) as a view of the one scene: its geometry tracked under the
			// first-person root, members of kSetFirstPerson drawn by the first-person epochs (off by default: CS_DCLF_FIRST_PERSON=1 seeds it
			// on; the engine draws its few dozen draws for less than the two epochs' builds, barriers and indirect draws cost). After the reflections (Features/Reflection's, registered earlier after the same Core toggle), last; it needs the skinned and
			// the actors (the player's skeleton).
			const auto firstPerson = a_context.RegisterToggle({ .name = "first person", .label = "Draw the first-person arms and weapons (CS_DCLF_FIRST_PERSON)",
				.tooltip = "T7e: DCLF draws the first-person skeleton's geometry (arms, weapons, shield, magic hands) as a view of its scene, with the first-person camera, in the engine's depth and first-person passes. Off by default: the engine draws the skeleton's few draws for less than DCLF's two epochs cost (hand-back sweep: 0.15 ms a frame).",
				.section = "First person", .seed = firstPersonSwitch, .entersClassification = true, .offByDefault = true, .after = "exclude primary entries", .needs = { "skinned", "actors" } });
			// The view (Core's view machinery: its epochs, revisions, frame requests, classification and set decisions read it as data), and
			// with CS_DCLF_FIRST_PERSON_DECALS (read once, here) a decal on a first-person entry as a forward object of its forward part.
			a_context.RegisterView(Contribution(firstPerson, SwitchValue(decalsSwitch) != "0"));
			// The view's latch, targets, pipelines and resources, before the coverage that decides whether its members are withheld.
			a_context.RegisterViewWork({ .view = Api::EngineView::FirstPerson, .point = Api::ViewPoint::FrameStartBeforeCoverage, .run = &BeginFrame });
			// The depth epoch's commit, inside the depth commit (the depth epoch hosts its passes: Schedule/Frame.cpp's fp.depth), after the
			// reflection's faces' (view order).
			a_context.RegisterViewWork({ .view = Api::EngineView::FirstPerson, .point = Api::ViewPoint::DepthCommitted, .run = &ExecuteDepth });
			// The skeleton, hidden for every view the scene serves, in the frame's cull-hidden list (whatever the toggle, as Core listed it).
			a_context.RegisterCullHiddenSource({ .bit = Api::CullHiddenBit::Hidden, .nodes = &CullHiddenSkeleton });
			// After Features/Particles' and Features/Billboards' lines, where Core printed the view's.
			a_context.RegisterReportLines({ .name = "first person", .point = Api::ReportPoint::AsyncWork, .text = &Lines });
			// CS_DCLF_SET_PARITY: the skeleton's slots and claimed phases.
			if (Diagnostics::Observe().claims)
				SubscribeSkeletonParity();
			// The view's scene diagnostics (Core announces them at its root view's points), and their lines after the set's, where Core
			// printed them.
			SubscribeRootViewChecks();
			a_context.RegisterReportLines({ .name = "first person filing", .point = Api::ReportPoint::SceneCoordinatorSet, .lines = &SetLines });
			// RenderDepth's first-person draw (the frame's culled bits and depth range) and Main::RenderFirstPersonView's accumulator render (the colour epoch).
			InstallHooks(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "first person", &Install };
	}
}
