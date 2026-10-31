// The shadow-view entries (see the header): the features' contributions, what the shadow engine reads of them, and the shadow cameras'
// hooks.

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"

namespace DCLF::Extensions
{
	static_assert(kShadowEngineModes == Draws::kShadowModeCount && kOcclusionMaps == kOcclusionViews);
	static_assert(static_cast<std::size_t>(Api::OcclusionMap::Sky) == kOcclusionSky &&
				  static_cast<std::size_t>(Api::OcclusionMap::Precipitation) == kOcclusionPrecipitation);

	namespace
	{
		/** @brief A feature's map into its entry (Registry refused incomplete ones and a second for a map). */
		void FillRegistered(OcclusionMapEntry& a_entry, const Api::OcclusionMapContribution& a_map)
		{
			a_entry.registered = true;
			a_entry.toggle = a_map.toggle;
			a_entry.available = a_map.available;
			a_entry.phase = a_map.bits.phase;
			a_entry.viewBit = a_map.bits.viewBit;
			a_entry.technique = a_map.technique;
			a_entry.minRadius = a_map.minRadius;
			a_entry.floorHeight = a_map.floorHeight;
			a_entry.target = a_map.target;
			a_entry.list = a_map.labels.list;
			a_entry.report = a_map.labels.report;
			a_entry.targetName = a_map.labels.target;
			a_entry.importTarget = a_map.importTarget;
			a_entry.referenceDue = a_map.referenceDue;
			a_entry.camera = a_map.camera;
			a_entry.ownsAccumulator = a_map.ownsAccumulator;
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
		std::span<const Api::ShadowSourceContribution, kShadowSources> a_sources, const Api::ShadowExclusionContribution& a_exclusion)
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the shadow views were resolved twice");
		ShadowViewSet set;
		for (std::size_t v = 0; v < a_maps.size(); ++v)
			if (a_maps[v].map < Api::OcclusionMap::Count)
				FillRegistered(set.maps[v], a_maps[v]);
		for (std::size_t s = 0; s < a_sources.size(); ++s) {
			if (a_sources[s].source < Api::ShadowSource::Count)
				set.sources[s] = { true, a_sources[s].camera, a_sources[s].lightKept, a_sources[s].fullFrustumWindowEnd, a_sources[s].frameStart,
					a_sources[s].frameEnd };
		}
		if (!a_exclusion.name.empty()) {
			set.exclusion = a_exclusion.build;
			set.exclusionPublish = a_exclusion.publish;
			set.exclusionFrameStart = a_exclusion.frameStart;
			set.listExclusion = a_exclusion.listExclusion;
		}
		DeriveShadowOrders(set);
		Detail::resolvedShadowViews = std::move(set);
	}

	namespace
	{
		// Main thread, written by the sources' hooks (ShadowSourceHooks::CullsInstalled), read by the engine side's installs after them.
		std::array<bool, kShadowSources> sourceCullsInstalled{};
	}

	bool SourceCullsInstalled(Api::ShadowSource a_source)
	{
		return sourceCullsInstalled[static_cast<std::size_t>(a_source)];
	}

	void ShadowSourcesFrameStart(std::uint32_t a_frame)
	{
		for (const auto& source : ShadowViewsSet().sources)
			if (source.frameStart)
				source.frameStart(a_frame);
	}

	void ShadowSourcesFrameEnd()
	{
		for (const auto& source : ShadowViewsSet().sources)
			if (source.frameEnd)
				source.frameEnd();
	}

	void InstallSourceHooks(std::span<const Api::Feature> a_features)
	{
		static bool installed = false;
		if (std::exchange(installed, true))
			stl::report_and_fail("Drawcall Limit Fix: the shadow sources' hooks were installed twice");
		// By source, in ShadowSource order (the install log's): the sun's accumulation and UpdateCamera (Features/Shadows/Sun), the light
		// selection at the Spot's (Features/LocalLights: it hands the Spot and Point sources their kept lights), the point culls
		// (Features/Shadows/Local), then the focus cameras (Features/Shadows/Local).
		for (std::size_t s = 0; s < kShadowSources; ++s) {
			Api::ShadowSourceHooks hooks{ static_cast<Api::ShadowSource>(s) };
			for (const auto& feature : a_features)
				if (feature.sourceHooks)
					feature.sourceHooks(hooks);
		}
	}
}

namespace DCLF::Api
{
	void ShadowSourceHooks::CullsInstalled()
	{
		if (source < ShadowSource::Count)
			Extensions::sourceCullsInstalled[static_cast<std::size_t>(source)] = true;
	}
}
