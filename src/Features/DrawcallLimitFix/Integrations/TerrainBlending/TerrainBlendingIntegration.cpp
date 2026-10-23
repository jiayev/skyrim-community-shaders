// Terrain Blending: the "holds the terrain" classify input, its switch and toggle, the gates on the terrain techniques, its held groups,
// its mask (t55) and its near blocks' depth twin. The render thread's callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/TerrainBlending/TerrainBlending.h"

#include "ShaderCache.h"

#include "Features/TerrainBlending.h"

#include <cstdint>

namespace DCLF::Integrations::TerrainBlending
{
	namespace
	{
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;

		// Whether Terrain Blending draws its terrain after the opaque pass, into a frame DCLF draws in. Any thread: classification takes it
		// once a pass (and the frame driver once a frame, a change dropping the verdicts); the render thread's callbacks below read it live.
		bool DefersTerrain()
		{
			const auto& terrainBlending = globals::features::terrainBlending;
			return terrainBlending.loaded && terrainBlending.settings.Enabled;
		}

		// Terrain Blending's mask (t55) is its terrain depth, bound by its replay after this epoch, so named here rather than
		// captured. By the colour epoch it holds the Z-prepass's main depth (TerrainBlending::BlendPrepassDepths). Null while it does not
		// hold the terrain: the captured view.
		void* MaskView()
		{
			return DefersTerrain() ? globals::features::terrainBlending.terrainDepth.depthSRV : nullptr;
		}

		// Terrain Blending's terrain depth shares the main depth's description; the Z-prepass draws near terrain blocks into it
		// as that feature's depth hook does.
		void* TerrainDepthTarget()
		{
			const auto& terrainBlending = globals::features::terrainBlending;
			return terrainBlending.loaded && terrainBlending.terrainDepth.texture ? terrainBlending.terrainDepth.texture : nullptr;
		}

		// Terrain Blending's near test with its depth-pass eye, for the terrain group 1 twin buckets (kBucketTerrainPair).
		bool NearTest(float (&a_eyeDistance)[4])
		{
			if (!DefersTerrain())
				return false;
			const auto& terrainEye = globals::features::terrainBlending.eyePosition;
			a_eyeDistance[0] = terrainEye.x;
			a_eyeDistance[1] = terrainEye.y;
			a_eyeDistance[2] = terrainEye.z;
			a_eyeDistance[3] = ::TerrainBlending::kTerrainDepthDistance;
			return true;
		}

		void Install(Api::InstallContext& a_context)
		{
			// Switched on or off, it moves the terrain and its no-blend meshes between their held groups and the native loop.
			const auto defers = a_context.RegisterClassifyInput({ .name = "terrain blending defers", .sample = &DefersTerrain });
			// Where Core's were listed: right after Features/Landscape's CS_DCLF_MTLAND, and its toggle right after the terrain's.
			const auto terrainSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_TB_TERRAIN", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: terrain under Terrain Blending, and the meshes that feature redraws after it, stay native (T7c1; live toggle's seed)",
				.after = "CS_DCLF_MTLAND" });
			const auto terrain = a_context.RegisterToggle({ .name = "terrain under TB", .label = "Terrain under Terrain Blending",
				.tooltip = "With Terrain Blending on, DCLF draws the terrain as that feature does: its near blocks' depth into its terrain depth and, offset, into the main depth, and its colour blended after the opaque and decal draws, then the meshes it redraws after the terrain.",
				.seed = terrainSwitch, .entersClassification = true, .after = "terrain", .needs = { "terrain" } });
			// MTLand and MTLandLODBlend (Features/Landscape's): while Terrain Blending holds the terrain (it redraws every terrain pass blended
			// with its own depth state, which an opaque draw would break), they need the toggle too: DCLF then draws the terrain as that
			// feature does, in its terrain group.
			for (const auto technique : { Technique::MTLand, Technique::MTLandLODBlend })
				a_context.RegisterTechniqueGate({ .technique = static_cast<std::uint32_t>(technique), .whileOn = defers, .needs = terrain });
			// Its groups (Records.h's terrain group bits): its terrain blocks (blend mode 1, LESS_EQUAL with depth writes) and the no-blend
			// meshes after it (EQUAL), as that feature replays them before the blended decals (Deferred::Hooks::Main_RenderWorld_BlendedDecals).
			// Without the toggle the no-blend meshes stay native (DCLF would draw them before the terrain blends over them); a decal or layer
			// has a group of its own.
			a_context.RegisterHeldGroup({ .heldWhile = defers, .slot = kTerrainRange, .name = "terrain", .order = 2,
				.propertyFlags = static_cast<std::uint64_t>(Flag::kMultiTextureLandscape) });
			a_context.RegisterHeldGroup({ .heldWhile = defers, .slot = kNoBlendRange, .name = "terrain-no-blend", .order = 3,
				.propertyFlags = static_cast<std::uint64_t>(Flag::kNoTransparencyMultiSample), .toggle = terrain });
			// Its mask, t55, each colour commit.
			a_context.RegisterFrameTexture({ .name = "terrain blending mask", .textureRegister = kMaskRegister, .view = &MaskView, .capturedWhenNull = true });
			// Its near blocks drawn twice in the depth segment: into its terrain depth and, offset, into the main depth.
			a_context.RegisterNearTwin({ .name = "Terrain Blending's terrain depth", .debugName = "DCLF terrain blending depth",
				.resource = "cs.dclf.terrain-blending-depth", .target = &TerrainDepthTarget, .nearTest = &NearTest });
		}
	}

	Api::Feature Integration()
	{
		return { "terrain blending", &Install };
	}
}
