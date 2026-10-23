// Grass: the engine's grass DCLF draws in the engine's place. Its switch and toggle, the grass class's contribution, the capture's sink
// (its early hooks), the tables with their commit hook, the cull, arguments and counts' passes, the draw family, the report lines and
// the parity.

#include "Features/DrawcallLimitFix/Features/Grass/Grass.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Grass
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Right after tree LOD's (which registers before this feature).
			const auto grass = a_context.RegisterSwitch({ .name = "CS_DCLF_GRASS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: grass stays native (Grass Optimizations' or the engine's) (live toggle's seed)", .after = "CS_DCLF_LOD_TREES" });
			// Right after tree LOD's. Its change drops no classification cache: grass is drawn from the mirror, not classified.
			const auto toggle = a_context.RegisterToggle({ .name = "grass", .label = "Grass", .seed = grass, .after = "tree LOD" });
			// The grass shapes, the Grass draw family's while the toggle is on (its decision; the revisions' grass pipelines).
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Grass, .toggle = toggle });
			// Its tables and their uploads, its passes, the family over them, its report lines.
			RegisterTables(a_context);
			RegisterPasses(a_context);
			RegisterFamily(a_context);
			RegisterReport(a_context);
			// CS_DCLF_PERSISTENT_PARITY: the mirror against GrassShared's live set.
			if (Diagnostics::Observe().scene)
				SubscribeParity();
		}
	}

	Api::Feature Feature()
	{
		// The capture's sink at the early hooks: registered before any grass loads, whatever the toggle (a live toggle needs the mirror).
		return { "grass", &Install, &InstallHooks };
	}
}
