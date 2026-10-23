// Tree wind: the trees DCLF owns animated on the GPU. Its switches and the trees toggle, technique 12's contributions, the trees' table
// family and coordinator component, the seeds (a frame-value pool), the scene resources with their commit hook and frame binding (t124),
// the pass in the depth epoch's next-frame state, the tree manager's engine side, the report line and the parity.

#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::TreeWind
{
	namespace
	{
		Api::SwitchId listAllSwitch;  // install

		// The trees' switches and live toggle, where Core's were listed (after the skin partitions' behind Features/Actors', which registers
		// before this feature, and the primary exclusion's): the toggle.
		Api::ToggleId RegisterSettings(Api::InstallContext& a_context)
		{
			const auto trees = a_context.RegisterSwitch({ .name = "CS_DCLF_TREES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: trees stay native (live toggle's seed)", .after = "CS_DCLF_SKIN_PARTITIONS" });
			listAllSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_TREE_LIST_ALL", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never,
				.description = "1 (T7f; off by default): the tree manager's list loop given DCLF's verdict for every BSTreeNode DCLF lists; off: the stood-in trees alone (T7b5's fix)",
				.after = "CS_DCLF_PRIMARY_EXCLUDE" });
			// The TreeAnim technique, whose vertex animation reads TreeParams and WindTimers per object (extras rows): an object class
			// (its technique contribution's toggle: RegisterObjects).
			return a_context.RegisterToggle({ .name = "trees", .label = "Trees", .seed = trees, .entersClassification = true, .after = "skinned" });
		}

		void Install(Api::InstallContext& a_context)
		{
			const Api::ToggleId trees = RegisterSettings(a_context);
			RegisterObjects(a_context, trees);
			RegisterTables(a_context);
			RegisterSeeds(a_context);
			RegisterScene(a_context);
			RegisterPass(a_context);
			RegisterAnimation(a_context);
			RegisterReport(a_context);
			// CS_DCLF_PERSISTENT_PARITY: the wind rows read back against the engine's formulas and clocks.
			if (Diagnostics::Observe().scene)
				SubscribeParity();
		}
	}

	Api::Feature Feature()
	{
		return { "tree-wind", &Install };
	}

	bool ListAll()
	{
		return SwitchEnabled(listAllSwitch);
	}
}
