// OcclusionMaps: Skylighting's sky occlusion map and the precipitation occlusion mask drawn by Core's shadow engine. Their switches and
// toggles, each map's contribution (its bits, occluder rule, cull size test, target, labels, camera, accumulator and reference parity),
// their report lines and parity.

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/Skylighting.h"

namespace DCLF::Features::OcclusionMaps
{
	namespace
	{
		constexpr auto kSky = static_cast<std::uint32_t>(Api::OcclusionMap::Sky);
		constexpr auto kPrecipitation = static_cast<std::uint32_t>(Api::OcclusionMap::Precipitation);

		// The sky map's target: Skylighting's own occlusion texture, which it swaps in for the precipitation occlusion target.
		void ImportSkyTarget(Api::OcclusionTargetImport& a_import)
		{
			if (auto* sky = globals::features::skylighting.texOcclusion) {
				a_import.supplied = true;
				a_import.texture = sky->resource.get();
				a_import.view = sky->dsv.get();
			}
		}
		bool SkyReferenceDue() { return ReferenceDue(kSky); }
		/**
		 * @brief Skylighting::OcclusionTechnique's probe grid test: an occluder whose bound top is below the grid's bottom less the margin.
		 * The shadow epoch latches it at the shadow maps, before this frame's grid update (State::UpdateSharedData at EarlyPrepasses): the
		 * lower of the last update's bottom and the one this frame's camera takes (the main camera as the frame's culls placed it), so a
		 * camera that moved a grid cell down since draws no fewer occluders than the grid it reads (a cell up: a few more). -FLT_MAX
		 * after a reset (ResetSkylighting): no floor, as Skylighting's own test.
		 */
		float SkyFloorHeight()
		{
			const auto& skylighting = globals::features::skylighting;
			float bottom = skylighting.probeGridBottomZ;
			if (const auto* camera = RE::Main::WorldRootCamera(); camera && bottom > -FLT_MAX)
				bottom = std::min(bottom, skylighting.ProbeGridBottomZ(camera->world.translate.z));
			return bottom - Skylighting::OCCLUSION_BELOW_GRID_MARGIN;
		}
		bool PrecipitationReferenceDue() { return ReferenceDue(kPrecipitation); }

		/**
		 * @brief The maps' contributions: Skylighting's sky map and the precipitation mask, both available while Skylighting is loaded (its
		 * RenderOcclusion drives both maps: its hook of the engine's precipitation mask, then its own map), both drawn by the precipitation
		 * accumulator from its camera.
		 */
		void RegisterMaps(Api::InstallContext& a_context, Api::ToggleId a_sky, Api::ToggleId a_precipitation)
		{
			a_context.RegisterOcclusionMap({ .map = Api::OcclusionMap::Sky,
				.toggle = a_sky,
				.available = &globals::features::skylighting.loaded,
				.bits = { kSetOccluderSky, kViewSkyOccluder },
				.technique = &SkylightingTechnique,
				.minRadius = true,
				.floorHeight = &SkyFloorHeight,
				.target = Api::OcclusionTarget::ImportedTexture,
				.labels = { "sky occlusion", "Skylighting", "DCLF Skylighting occlusion map" },
				.importTarget = &ImportSkyTarget,
				.referenceDue = &SkyReferenceDue,
				.camera = &PrecipitationCamera,
				.ownsAccumulator = &SkyOwnsAccumulator });
			a_context.RegisterOcclusionMap({ .map = Api::OcclusionMap::Precipitation,
				.toggle = a_precipitation,
				.available = &globals::features::skylighting.loaded,
				.bits = { kSetOccluderPrecipitation, kViewPrecipOccluder },
				.technique = &PrecipitationTechnique,
				// Skylighting's size test, which the mask's cull has always taken too.
				.minRadius = true,
				.target = Api::OcclusionTarget::EngineMask,
				.labels = { "precipitation", "precipitation", "DCLF precipitation occlusion mask" },
				.referenceDue = &PrecipitationReferenceDue,
				.camera = &PrecipitationCamera,
				.ownsAccumulator = &PrecipitationOwnsAccumulator });
		}

		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: after the primary exclusion's and the features' after it (Features/TreeWind's), before the opaque
			// effects' and the other features' before them (Features/Reflection's, Features/FirstPerson's, which register later).
			const auto skySwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_SKYLIGHT", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: Skylighting's occlusion map stays native (live toggle's seed)", .before = "CS_DCLF_OPAQUE_EFFECTS" });
			const auto precipitationSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_PRECIPITATION", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: the precipitation occlusion mask stays native (live toggle's seed)", .before = "CS_DCLF_OPAQUE_EFFECTS" });
			// Where Core's were listed, after the primary exclusion's, last in the shadow views' section; the first feature toggles
			// registered, so their bits in the packed word are where Core's were. They need the shadows (the maps draw on the shadow commit).
			const auto sky = a_context.RegisterToggle({ .name = "sky occlusion", .label = "Draw Skylighting's occlusion map",
				.tooltip = "With Skylighting loaded, DCLF draws its sky occlusion height map from its own tables on the GPU, and the engine no longer culls or registers the scene for it.",
				.seed = skySwitch, .entersClassification = true, .after = "exclude primary entries", .needs = { "shadows" } });
			const auto precipitation = a_context.RegisterToggle({ .name = "precipitation occlusion", .label = "Draw the precipitation occlusion mask",
				.tooltip = "With Skylighting loaded, DCLF draws the precipitation occlusion mask (where rain and snow stop) from its own tables on the GPU, and the engine no longer culls or registers the scene for it.",
				.seed = precipitationSwitch, .entersClassification = true, .after = "exclude primary entries", .needs = { "shadows" } });
			// The maps (Core's shadow engine: the shadow epoch, the set decisions and the reports read them as data; the classification
			// calls their rules per walked entry).
			RegisterMaps(a_context, sky, precipitation);
			// Their report lines, and CS_DCLF_SKYLIGHT_PARITY's switches and compare.
			RegisterReport(a_context);
			RegisterParity(a_context);
			if (Diagnostics::Observe().claimsOrScene)
				SubscribeOcclusionViewCheck();
		}
	}

	Api::Feature Feature()
	{
		return { "occlusion maps", &Install };
	}
}
