// The shadow-view entries (see the header): the features' contributions and Core's own, what the shadow engine reads of them, and the
// shadow cameras' hooks.

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Engine/Views/FocusViews.h"
#include "Features/DrawcallLimitFix/Engine/Views/LightSelection.h"
#include "Features/DrawcallLimitFix/Engine/Views/LightViews.h"
#include "Features/DrawcallLimitFix/Engine/Views/SunViews.h"
#include "Features/Skylighting.h"

namespace DCLF::Extensions
{
	static_assert(kShadowEngineModes == Draws::kShadowModeCount && kOcclusionMaps == kOcclusionViews);
	static_assert(static_cast<std::size_t>(Api::OcclusionMap::Sky) == kOcclusionSky &&
				  static_cast<std::size_t>(Api::OcclusionMap::Precipitation) == kOcclusionPrecipitation);

	namespace
	{
		constexpr auto kSun = static_cast<std::size_t>(Api::ShadowSource::Sun);
		constexpr auto kSpot = static_cast<std::size_t>(Api::ShadowSource::Spot);
		constexpr auto kPoint = static_cast<std::size_t>(Api::ShadowSource::Point);
		constexpr auto kFocus = static_cast<std::size_t>(Api::ShadowSource::Focus);

		// Core's own entries, until their features take them: the occlusion maps (Features/OcclusionMaps), the sun (Features/Shadows/Sun),
		// the spot, point and focus sources (Features/Shadows/Local). A feature registering one of them fails the install.
		constexpr bool kCoreMaps[kOcclusionMaps] = { true, true };
		constexpr bool kCoreSources[kShadowSources] = { true, true, true, true };

		// The sky map's target: Skylighting's own occlusion texture, which it swaps in for the precipitation occlusion target.
		void ImportSkyTarget(Api::OcclusionTargetImport& a_import)
		{
			if (auto* sky = globals::features::skylighting.texOcclusion) {
				a_import.supplied = true;
				a_import.texture = sky->resource.get();
				a_import.view = sky->dsv.get();
			}
		}
		bool SkyReferenceDue() { return Policy::RenderOcclusionReference(kOcclusionSky); }
		bool PrecipitationReferenceDue() { return Policy::RenderOcclusionReference(kOcclusionPrecipitation); }

		/** @brief A kept spot or point light's views, from its cameras (LightViews: the Lights board). */
		void LightKept(const Api::ShadowLightKept& a_light) { LightViews::Get().Update(a_light.light); }

		/**
		 * @brief Core's own maps: Skylighting's sky map and the precipitation mask, both available while Skylighting is loaded: its
		 * RenderOcclusion drives both maps (its hook of the engine's precipitation mask, then its own map).
		 */
		void FillCoreMaps(ShadowViewSet& a_set)
		{
			auto& sky = a_set.maps[kOcclusionSky];
			if (kCoreMaps[kOcclusionSky]) {
				sky.registered = true;
				sky.coreToggle = &ToggleSet::skyOcclusion;
				sky.available = &globals::features::skylighting.loaded;
				sky.phase = kSetOccluderSky;
				sky.viewBit = Draws::kViewSkyOccluder;
				sky.rule = Api::OccluderRule::Skylighting;
				sky.target = Api::OcclusionTarget::ImportedTexture;
				sky.list = "sky occlusion";
				sky.report = "Skylighting";
				sky.targetName = "DCLF Skylighting occlusion map";
				sky.importTarget = &ImportSkyTarget;
				sky.referenceDue = &SkyReferenceDue;
			}
			auto& precipitation = a_set.maps[kOcclusionPrecipitation];
			if (kCoreMaps[kOcclusionPrecipitation]) {
				precipitation.registered = true;
				precipitation.coreToggle = &ToggleSet::precipitationOcclusion;
				precipitation.available = &globals::features::skylighting.loaded;
				precipitation.phase = kSetOccluderPrecipitation;
				precipitation.viewBit = Draws::kViewPrecipOccluder;
				precipitation.rule = Api::OccluderRule::Precipitation;
				precipitation.target = Api::OcclusionTarget::EngineMask;
				precipitation.list = "precipitation";
				precipitation.report = "precipitation";
				precipitation.targetName = "DCLF precipitation occlusion mask";
				precipitation.referenceDue = &PrecipitationReferenceDue;
			}
		}

		/** @brief Core's own sources: the sun's and the focus views' cameras from Core's hooks, the kept local lights' from LightViews. */
		void FillCoreSources(ShadowViewSet& a_set)
		{
			if (kCoreSources[kSun])
				a_set.sources[kSun].registered = true;
			if (kCoreSources[kSpot])
				a_set.sources[kSpot] = { true, &LightKept };
			if (kCoreSources[kPoint])
				a_set.sources[kPoint] = { true, &LightKept };
			if (kCoreSources[kFocus])
				a_set.sources[kFocus].registered = true;
		}

		/** @brief A feature's map into its entry (Registry refused incomplete ones and a second for a map). */
		void FillRegistered(OcclusionMapEntry& a_entry, const Api::OcclusionMapContribution& a_map)
		{
			a_entry.registered = true;
			a_entry.toggle = a_map.toggle;
			a_entry.available = a_map.available;
			a_entry.phase = a_map.bits.phase;
			a_entry.viewBit = a_map.bits.viewBit;
			a_entry.rule = a_map.rule;
			a_entry.target = a_map.target;
			a_entry.list = a_map.labels.list;
			a_entry.report = a_map.labels.report;
			a_entry.targetName = a_map.labels.target;
			a_entry.importTarget = a_map.importTarget;
			a_entry.referenceDue = a_map.referenceDue;
		}

		/** @brief Each mode's phase, the readiness order, the views' sources by kind and the import order. */
		void DeriveShadowOrders(ShadowViewSet& a_set)
		{
			for (std::uint32_t m = 0; m < Draws::kFirstOcclusionMode; ++m)
				a_set.phaseOfMode[m] = SetPhaseOfMode(m);
			a_set.readinessOrder[a_set.readinessCount++] = kSetCaster;
			a_set.readinessOrder[a_set.readinessCount++] = kSetCasterPoint;
			for (std::uint32_t v = 0; v < kOcclusionMaps; ++v) {
				const auto& map = a_set.maps[v];
				a_set.phaseOfMode[Draws::OcclusionModeOf(v)] = map.phase;
				if (!map.registered)
					continue;
				a_set.occluderPhases |= map.phase;
				a_set.occluderViewBits |= map.viewBit;
				a_set.readinessOrder[a_set.readinessCount++] = map.phase;
			}
			// By ShadowViews::Kind (directional, frustum, parabolic, other) and focus: a focus view is the focus source's whatever its host;
			// a directional view the sun's, a frustum view a spot light's, a parabolic view a point light's; another kind's none.
			using S = Api::ShadowSource;
			a_set.sourceOf = { { { S::Sun, S::Focus }, { S::Spot, S::Focus }, { S::Point, S::Focus }, { S::Count, S::Focus } } };
			a_set.importOrder = { Api::OcclusionMap::Precipitation, Api::OcclusionMap::Sky };
		}
	}

	void ResolveShadowViews(std::span<const Api::OcclusionMapContribution, kOcclusionMaps> a_maps,
		std::span<const Api::ShadowSourceContribution, kShadowSources> a_sources)
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the shadow views were resolved twice");
		ShadowViewSet set;
		FillCoreMaps(set);
		FillCoreSources(set);
		for (std::size_t v = 0; v < a_maps.size(); ++v) {
			if (a_maps[v].map >= Api::OcclusionMap::Count)
				continue;
			if (kCoreMaps[v])
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: occlusion map {} was registered by a feature, but Core still fills it", v));
			FillRegistered(set.maps[v], a_maps[v]);
		}
		for (std::size_t s = 0; s < a_sources.size(); ++s) {
			if (a_sources[s].source >= Api::ShadowSource::Count)
				continue;
			if (kCoreSources[s])
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: shadow source {} was registered by a feature, but Core still fills it", s));
			set.sources[s] = { true, a_sources[s].lightKept };
		}
		DeriveShadowOrders(set);
		Detail::resolvedShadowViews = std::move(set);
	}

	void InstallCameraHooks(std::span<const Api::Feature> a_features)
	{
		static bool installed = false;
		if (std::exchange(installed, true))
			stl::report_and_fail("Drawcall Limit Fix: the shadow cameras' hooks were installed twice");
		// Core's own, in the install log's order: the sun's UpdateCamera, the light selection (Core's, whatever the sources), the focus
		// cameras. Then the features' camera hooks, in the list's order.
		if (kCoreSources[kSun])
			SunViews::Get().Install();
		LightSelection::Get().Install();
		if (kCoreSources[kFocus])
			FocusViews::Get().Install();
		for (const auto& feature : a_features)
			if (feature.cameraHooks)
				feature.cameraHooks();
	}
}
