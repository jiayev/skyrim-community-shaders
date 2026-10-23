// Billboards: the effect meshes under a billboard node turned on the GPU, and the particle systems under one drawn by the particle path.
// Their switches and the billboards toggle, the billboard parent's contributions, the parity's early hooks and the report lines.

#include "Features/DrawcallLimitFix/Features/Billboards/Billboards.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Features::Billboards
{
	namespace
	{
		Api::ToggleId billboardsToggle;  // install

		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: the billboards after the particles (Features/Particles registers first, after the refraction), the
			// particles under billboards after the opaque effects.
			const auto billboards = a_context.RegisterSwitch({ .name = "CS_DCLF_BILLBOARDS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: effect-shader meshes under an NiBillboardNode (fire cards, spray jets) drawn by DCLF, turned to each view's camera in the vertex stage as NiBillboardNode::OnVisible turns the node (needs CS_DCLF_EFFECTS); 0 they stay native (live toggle's seed)",
				.after = "CS_DCLF_REFRACTION" });
			const auto particles = a_context.RegisterSwitch({ .name = "CS_DCLF_BILLBOARD_PARTICLES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on (T7f item 7): particle systems under NiBillboardNodes (the magic hands' fire cores and embers) drawn by DCLF's particle path like any other (their world and particles are their last update's; needs CS_DCLF_BILLBOARDS and CS_DCLF_PARTICLES); 0 they stay the engine's (billboard)",
				.after = "CS_DCLF_OPAQUE_EFFECTS" });
			// BSEffectShaderProperty meshes under one NiBillboardNode, turned to the drawing camera by the vertex stage from the node's base
			// transform (Core/Gpu/PlacementRows/BillboardRow.h; needs effects; on by default: CS_DCLF_BILLBOARDS=0 seeds it off): an object class.
			billboardsToggle = a_context.RegisterToggle({ .name = "billboards", .label = "Billboards: fire cards, spray jets",
				.tooltip = "DCLF draws effect-shader meshes under a billboard node, turning each to the drawing camera on the GPU as the engine's cull turns the node. On by default.",
				.seed = billboards, .entersClassification = true, .after = "refraction", .needs = { "effects" } });
			// The leaves under a billboard node DCLF takes while the toggle is on (Coordinator::ClassifyFrame, BillboardModeOf): an effect
			// shape under exactly one (Extensions::BillboardTurnedLeaf), turned by the vertex stage; with CS_DCLF_BILLBOARD_PARTICLES (read once, here) a particle system
			// under any, whose world and particles are its last update's (0x140d3e650 runs each child's UpdateDownwardPass, after which
			// ParticleSnapshots snapshots it).
			a_context.RegisterParent({ .parent = Api::EngineParent::Billboard, .leaves = Api::ParentLeaves::TurnedEffectShapes, .toggle = billboardsToggle });
			if (SwitchValue(particles) != "0")
				a_context.RegisterParent({ .parent = Api::EngineParent::Billboard, .leaves = Api::ParentLeaves::ParticleSystems, .toggle = billboardsToggle });
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		// CS_DCLF_SET_PARITY's observer of the engine's turns goes in early: the primary cull takes NiBillboardNode's OnVisible slot at
		// its install, the observer's thunk among the OnVisibles it stands for.
		return { "billboards", &Install, &InstallTurns };
	}

	Api::ToggleId Toggle()
	{
		return billboardsToggle;
	}
}
