// LOD: object and terrain LOD under TES::lodLandRoot, DCLF's while their toggles are on. Their switches and toggles, the two object
// classes' contributions, and object LOD's segment events (their writers' hooks at the early hooks, their engine channel and its
// component, report line and parity: LodSegmentEvents.h).

#include "Features/DrawcallLimitFix/Features/Lod/Lod.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodSegmentEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Lod
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: the switches before the LOD gates' (Features/TreeLod's follow them there), the toggles after the
			// refraction's, behind Features/Particles', Features/Billboards' and Features/SwitchNodes' (which register before this feature).
			const auto objectsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LOD_OBJECTS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: object LOD stays native and untracked (live toggle's seed)", .before = "CS_DCLF_LOD_GATES" });
			const auto terrainSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LOD_TERRAIN", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: terrain LOD stays native (live toggle's seed)", .before = "CS_DCLF_LOD_GATES" });
			const auto objects = a_context.RegisterToggle({ .name = "object LOD", .label = "Object LOD: distant objects (CS_DCLF_LOD_OBJECTS)", .seed = objectsSwitch,
				.entersClassification = true, .after = "refraction" });
			const auto terrain = a_context.RegisterToggle({ .name = "terrain LOD", .label = "Terrain LOD: distant land (CS_DCLF_LOD_TERRAIN)", .seed = terrainSwitch,
				.entersClassification = true, .after = "refraction" });
			// Object LOD (the sub-index shapes' eligibility, the LOD object techniques and flags) and terrain LOD (the LOD land techniques and
			// flags), each an object class; either lists the LOD root as a category node, and both (with the swaps gated) let a view's faces
			// draw the LOD land and objects roots in the engine's place.
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::LodObjects, .toggle = objects });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::LodTerrain, .toggle = terrain });
			// Object LOD's segment events: the channel its writers' hooks feed, its component (the shapes' drawn ranges kept up to date)
			// and its report line; CS_DCLF_PERSISTENT_PARITY: the kept ranges against the live ones.
			RegisterSegmentChannel(a_context);
			if (Diagnostics::Observe().scene)
				SubscribeSegmentParity();
		}
	}

	Api::Feature Feature()
	{
		// The early hooks: the segment writers, the channel's source (where Core's scene events installed them: after the billboards'
		// early hooks, before tree LOD's and the hidden nodes').
		return { "lod", &Install, &InstallSegmentHooks };
	}
}
