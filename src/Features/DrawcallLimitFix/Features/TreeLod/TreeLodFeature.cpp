// Tree LOD: the tree LOD instances DCLF draws in the engine's place. Its switches and toggle, the tree LOD class's contribution, the
// channel's source (its early hooks) and the audit.

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Features::TreeLod
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: before the LOD gates', after Features/Lod's (which registers before this feature); the audit's after
			// the texture paths' trace.
			const auto trees = a_context.RegisterSwitch({ .name = "CS_DCLF_LOD_TREES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: tree LOD stays native (live toggle's seed)", .before = "CS_DCLF_LOD_GATES" });
			const auto audit = a_context.RegisterSwitch({ .name = "CS_DCLF_TREE_LOD_AUDIT", .kind = Api::SwitchKind::Diagnostic, .reduced = Api::SwitchReduced::Never,
				.description = "1: tree LOD instances the engine leaves shown over a loaded, visible full tree (and hidden ones without one), every 60 frames",
				.after = "CS_DCLF_TRACE_TEXTURE_PATHS" });
			// Where Core's was: after the refraction's, behind Features/Particles', Features/Billboards', Features/SwitchNodes' and
			// Features/Lod's (which register before this feature). Its change drops no classification cache: tree LOD is drawn from the
			// mirror, not classified.
			const auto toggle = a_context.RegisterToggle({ .name = "tree LOD", .label = "Tree LOD: distant trees (CS_DCLF_LOD_TREES)", .seed = trees, .after = "refraction" });
			// The tree LOD instances, Core's tree LOD path's while the toggle is on (DecideTreeLod; the revisions' trees pipelines; a trees
			// view's tree LOD per face).
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::TreeLod, .toggle = toggle });
			// Main::Draw's early hook, DCLF running or not.
			if (SwitchEnabled(audit))
				SubscribeAudit(audit);
		}
	}

	Api::Feature Feature()
	{
		// The channel's source at the early hooks: after Core's scene events, before Core's engine side and observers.
		return { "tree-lod", &Install, &InstallHooks };
	}
}
