// Forward: DCLF's blended objects and refraction in the forward epoch. Its switches and the forward and refraction toggles, the object
// classes' contributions, the engine's two forward points, the epoch's passes and its report lines.

#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"

namespace DCLF::Features::Forward
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: right before the projected UV's (Features/Landscape's CS_DCLF_MTLAND, then Integrations/TerrainBlending's
			// CS_DCLF_TB_TERRAIN, before CS_DCLF_FORWARD; Features/Effects' CS_DCLF_EFFECTS after it; Features/Particles' and
			// Features/Billboards' after CS_DCLF_REFRACTION).
			const auto forwardSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_FORWARD", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: blended Lighting geometry drawn by DCLF's forward epoch (T7d: a GPU depth sort, back to front, before the engine's blended draws); 0 it stays native (live toggle's seed)",
				.before = "CS_DCLF_PROJECTED_UV" });
			const auto refractionSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_REFRACTION", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: refraction (kRefraction, kTempRefraction Lighting geometry: heat haze, invisibility) drawn by DCLF into the refraction normals in the forward epoch (T7d; needs CS_DCLF_FORWARD); 0 it stays native (live toggle's seed)",
				.before = "CS_DCLF_PROJECTED_UV" });
			// Blended Lighting geometry (alpha-blended, or a material alpha below one), drawn by the forward epoch: a GPU depth sort of the
			// visible set, back to front, before the engine's own blended draws (on by default: CS_DCLF_FORWARD=0 seeds it off). Where Core's
			// was: before the shadows', after the projected's (Features/Landscape's "terrain" and Integrations/TerrainBlending's "terrain
			// under TB" after it); Features/Effects' "effects" after it.
			const auto forward = a_context.RegisterToggle({ .name = "forward", .label = "Blended objects: the forward epoch",
				.tooltip = "T7d: DCLF draws blended Lighting geometry itself, sorted back to front on the GPU, just before the engine's own blended draws (effects, particles). On by default (validated w174/w175).",
				.seed = forwardSwitch, .entersClassification = true, .before = "shadows" });
			// Refraction: Lighting geometry with kRefraction or kTempRefraction, whose one main pass is a Utility RENDER_NORMAL pass into the
			// refraction normals (kREFRACTION_NORMALS) that ISRefraction warps the frame by; drawn in the forward epoch (needs forward; on by
			// default: CS_DCLF_REFRACTION=0 seeds it off). After the effects' (Features/Particles', Features/Billboards', Features/SwitchNodes',
			// Features/Lod's and Features/TreeLod's toggles after it).
			const auto refraction = a_context.RegisterToggle({ .name = "refraction", .label = "Refraction: heat haze, invisibility",
				.tooltip = "T7d: DCLF draws refraction geometry's normals into the refraction normals the engine's refraction effect warps the frame by, in the forward epoch. On by default (validated w174/w175).",
				.seed = refractionSwitch, .entersClassification = true, .after = "effects", .needs = { "forward" } });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Forward, .toggle = forward });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Refraction, .toggle = refraction });
			// The main pass's blended draw, where the forward epoch runs, and Main::Draw's refraction point.
			InstallHooks(a_context);
			// The forward epoch's passes, and its report lines (before Features/Effects', which install after it).
			RegisterPasses(a_context);
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "forward", &Install };
	}
}
