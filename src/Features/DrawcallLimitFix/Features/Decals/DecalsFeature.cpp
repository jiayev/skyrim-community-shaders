// Decals: the decals and multi-index layers DCLF draws in its decal groups. Its switches and the decals and layers toggles, the object
// classes' contributions, the groups' main ranges, the decal nodes' arrays (their edits' early hooks and node capture), the member
// decals' order (a coordinator component), its parity and their lines.

#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Decals
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: after the skin partitions' (and Features/Actors' and Features/TreeWind's, which register before this
			// feature).
			const auto decalsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_DECALS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: decals stay native (live toggle's seed)", .after = "CS_DCLF_SKIN_PARTITIONS" });
			const auto layersSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LAYERS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: multi-index shapes' main passes stay native (live toggle's seed)", .after = "CS_DCLF_SKIN_PARTITIONS" });
			// Decals (accumulation hints 2 and 3), drawn by the second pass: an object class. After the skinned (and the trees, registered
			// before), the layers after it.
			const auto decals = a_context.RegisterToggle({ .name = "decals", .label = "Decals", .seed = decalsSwitch,
				.entersClassification = true, .after = "skinned" });
			// Multi-index shapes' main passes: the shape, and its layer (the additional property's passes, hint 12) as decal group 3.
			const auto layers = a_context.RegisterToggle({ .name = "multi-index layers", .label = "Multi-index shapes: cave walls with an ice or snow layer",
				.seed = layersSwitch, .entersClassification = true, .after = "skinned", .needs = { "decals" } });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Decal, .toggle = decals });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Layer, .toggle = layers });
			// The groups' ranges in the second pass, the engine's geometry groups 3, 2, 4: opaque decals (their depth first, in a
			// pass of their own), multi-index layers, then - after Terrain Blending's groups (Core's orders 2 and 3) - blended decals.
			a_context.RegisterMainRange({ .slot = kOpaqueRange, .name = "decals-opaque", .order = 0, .depthPrepass = true, .objectClass = Api::ObjectClass::Decal });
			a_context.RegisterMainRange({ .slot = kLayerRange, .name = "decals-layers", .order = 1, .objectClass = Api::ObjectClass::Layer });
			a_context.RegisterMainRange({ .slot = kBlendedRange, .name = "decals-blended", .order = 4, .objectClass = Api::ObjectClass::Decal });
			// The decal nodes' arrays in the mirror (a node capture contribution), which the order's keys read.
			RegisterDecalCapture(a_context);
			const auto order = RegisterOrder(a_context);
			// Its lines, the component's: the walk's candidates among the coordinator's last pass statistics (after the skinned line), the
			// order parity's after the material records' line.
			a_context.RegisterReportLines({ .name = "decals", .point = Api::ReportPoint::SceneCoordinatorLastPass, .component = order, .componentText = &CandidateLines });
			a_context.RegisterReportLines({ .name = "decal order parity", .point = Api::ReportPoint::SceneCoordinatorMaterials, .component = order,
				.componentText = &OrderParityLines });
			// CS_DCLF_PERSISTENT_PARITY: the kept order against one made whole.
			if (Diagnostics::Observe().scene)
				SubscribeOrderParity();
		}
	}

	Api::Feature Feature()
	{
		// Its early hooks: the decal array edits' (after Core's scene events, the hidden nodes' early hooks before them; no other hook
		// shares their sites).
		return { "decals", &Install, &InstallDecalHooks };
	}
}
