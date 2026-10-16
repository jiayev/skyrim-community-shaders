// Landscape: the terrain DCLF draws. Its switch and the terrain toggle, and the MTLand and MTLandLODBlend techniques' contributions.

#include "Features/DrawcallLimitFix/Features/Landscape/Landscape.h"

#include "ShaderCache.h"

#include <cstdint>

namespace DCLF::Features::Landscape
{
	namespace
	{
		using Technique = SIE::ShaderCache::LightingShaderTechniques;

		void Install(Api::InstallContext& a_context)
		{
			// Where Core's was listed: right before the forward epoch's (after Features/Decals' and the other object classes' switches, which
			// follow the skin partitions'); Integrations/TerrainBlending's CS_DCLF_TB_TERRAIN after it.
			const auto terrainSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_MTLAND", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: terrain stays native (live toggle's seed)", .before = "CS_DCLF_FORWARD" });
			// Where Core's was: right after the projected's (Integrations/TerrainBlending's "terrain under TB", which needs it, after it).
			const auto terrain = a_context.RegisterToggle({ .name = "terrain", .label = "Terrain (CS_DCLF_MTLAND)", .seed = terrainSwitch,
				.entersClassification = true, .after = "projected" });
			// MTLand and MTLandLODBlend: eligible while the toggle is on (and, while Terrain Blending holds the terrain, its toggle too:
			// Integrations/TerrainBlending's gate); their objects take the land-blend extras (LandBlendParams, Core's per-object VS rows).
			for (const auto technique : { Technique::MTLand, Technique::MTLandLODBlend })
				a_context.RegisterTechnique({ .technique = static_cast<std::uint32_t>(technique), .toggle = terrain, .extras = Api::ObjectExtras::LandBlend });
		}
	}

	Api::Feature Feature()
	{
		return { "landscape", &Install };
	}
}
