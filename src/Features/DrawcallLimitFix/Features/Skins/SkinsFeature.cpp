// Skins: the skinned shapes DCLF draws. Their switches and toggles, the skinned and skins of several partitions classes' contributions,
// and the skinned report line.

#include "Features/DrawcallLimitFix/Features/Skins/Skins.h"

namespace DCLF::Features::Skins
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: after the culling's, the skins of several partitions' after the skinned's (Features/Actors',
			// Features/TreeWind's and Features/Decals' after it, which register after this feature).
			const auto skinnedSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_SKINNED", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: skinned objects stay native (live toggle's seed)", .after = "CS_DCLF_CULL" });
			const auto partitionsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_SKIN_PARTITIONS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: skins of several partitions stay native (live toggle's seed)", .after = "CS_DCLF_SKINNED" });
			// Where Core's were: the skinned first, opening the object classes (Features/TreeWind's and Features/Decals' after it), the skins of
			// several partitions after the tree LOD's (Features/Actors' after it). Features/FirstPerson's toggle needs the skinned.
			const auto skinned = a_context.RegisterToggle({ .name = "skinned", .label = "Skinned", .section = "Object classes", .seed = skinnedSwitch,
				.entersClassification = true, .before = "projected" });
			const auto partitions = a_context.RegisterToggle({ .name = "skin partitions", .label = "Skins of several partitions: LOD trees, actor bodies",
				.seed = partitionsSwitch, .entersClassification = true, .after = "tree LOD", .needs = { "skinned" } });
			// Single-partition NiSkinInstance shapes, palettes from the engine; skins of several partitions (and BSDismemberSkinInstance), one
			// draw per partition the engine would draw.
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Skinned, .toggle = skinned });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::SkinPartitions, .toggle = partitions });
			// The skinned line, first at the coordinator's last-pass lines (before Features/Decals', which registers after this feature).
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "skins", &Install };
	}
}
