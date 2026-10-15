// OcclusionMaps: Skylighting's sky occlusion map and the precipitation occlusion mask drawn by Core's shadow engine. Their switches and
// toggles, and each map's contribution (its bits, occluder rule, target, labels and reference parity).

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
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
		bool SkyReferenceDue() { return Policy::RenderOcclusionReference(kSky); }
		bool PrecipitationReferenceDue() { return Policy::RenderOcclusionReference(kPrecipitation); }

		/**
		 * @brief The maps' contributions: Skylighting's sky map and the precipitation mask, both available while Skylighting is loaded (its
		 * RenderOcclusion drives both maps: its hook of the engine's precipitation mask, then its own map).
		 */
		void RegisterMaps(Api::InstallContext& a_context, Api::ToggleId a_sky, Api::ToggleId a_precipitation)
		{
			a_context.RegisterOcclusionMap({ .map = Api::OcclusionMap::Sky,
				.toggle = a_sky,
				.available = &globals::features::skylighting.loaded,
				.bits = { kSetOccluderSky, kViewSkyOccluder },
				.rule = Api::OccluderRule::Skylighting,
				.target = Api::OcclusionTarget::ImportedTexture,
				.labels = { "sky occlusion", "Skylighting", "DCLF Skylighting occlusion map" },
				.importTarget = &ImportSkyTarget,
				.referenceDue = &SkyReferenceDue });
			a_context.RegisterOcclusionMap({ .map = Api::OcclusionMap::Precipitation,
				.toggle = a_precipitation,
				.available = &globals::features::skylighting.loaded,
				.bits = { kSetOccluderPrecipitation, kViewPrecipOccluder },
				.rule = Api::OccluderRule::Precipitation,
				.target = Api::OcclusionTarget::EngineMask,
				.labels = { "precipitation", "precipitation", "DCLF precipitation occlusion mask" },
				.referenceDue = &PrecipitationReferenceDue });
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
			const auto sky = a_context.RegisterToggle({ .name = "sky occlusion", .label = "Draw Skylighting's occlusion map (CS_DCLF_SKYLIGHT)",
				.tooltip = "With Skylighting loaded, DCLF draws its sky occlusion height map from its own tables on the GPU, and the engine no longer culls or registers the scene for it.",
				.seed = skySwitch, .entersClassification = true, .after = "exclude primary entries", .needs = { "shadows" } });
			const auto precipitation = a_context.RegisterToggle({ .name = "precipitation occlusion", .label = "Draw the precipitation occlusion mask (CS_DCLF_PRECIPITATION)",
				.tooltip = "With Skylighting loaded, DCLF draws the precipitation occlusion mask (where rain and snow stop) from its own tables on the GPU, and the engine no longer culls or registers the scene for it.",
				.seed = precipitationSwitch, .entersClassification = true, .after = "exclude primary entries", .needs = { "shadows" } });
			// The maps (Core's shadow engine: the occlusion epoch, the set decisions, the classification and the reports read them as data).
			RegisterMaps(a_context, sky, precipitation);
		}
	}

	Api::Feature Feature()
	{
		return { "occlusion maps", &Install };
	}
}
