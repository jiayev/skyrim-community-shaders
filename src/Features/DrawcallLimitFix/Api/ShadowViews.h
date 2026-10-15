#pragma once

// The shadow-view contract (Api::InstallContext::RegisterOcclusionMap, RegisterShadowSource), the view contract's sibling
// (Api/Views.h): the occlusion maps and the shadow sources that Core's one shadow engine draws (one payload, one set of resources,
// one latch block and one key group; the occlusion epoch shares the shadow commit), taken by features as data and event callbacks.
// Casters are keyed by the engine's render mode, not by source: the sources own no set phase or view bit.

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Api/ViewCommon.h"

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Core-defined (Core/Extensions/ShadowViews.h): what a map's and a source's callbacks are handed.
	class OcclusionTargetImport;
	class ShadowLightKept;

	/**
	 * @brief An occlusion map: a view of the precipitation accumulator (render mode 0x1C) the engine would fill by culling the scene
	 * lists. The order is Core's slot order (kOcclusionSky, kOcclusionPrecipitation): each map's mode, view slot, depth target and
	 * technique column, and the order its epoch, readiness and reports run in.
	 */
	enum class OcclusionMap : std::uint8_t
	{
		Sky,
		Precipitation,
		Count,
	};
	/** @brief Which of Core's occluder rules decides a map's technique per object (read as data: Coordinator::OcclusionTechniqueOf). */
	enum class OccluderRule : std::uint8_t
	{
		Skylighting,
		Precipitation,
	};
	/** @brief What a map draws into: the engine's mask (the precipitation occlusion target), or a texture the feature imports. */
	enum class OcclusionTarget : std::uint8_t
	{
		EngineMask,
		ImportedTexture,
	};
	/** @brief A map's names: its view-mask parity list's, its report lines', and its imported target's. Copied at install. */
	struct OcclusionLabels
	{
		std::string_view list, report, target;
	};
	/**
	 * @brief An occlusion map contribution: the map is DCLF's while it is available and its toggle is on. Read where the shadow epochs,
	 * the set decisions and the classification run, as data (no call per object).
	 *
	 * - map: the map, one feature's. toggle: a registered toggle. available: a load-time flag the feature owns, read as data whenever
	 *   the toggle is (required).
	 * - bits: the map's set phase and view bit (the feature's own: Api/ViewCommon.h). rule, target, labels: see their types.
	 * - importTarget (ImportedTexture, required): render thread, once, as the shadow capability sets the maps' modes up
	 *   (IndirectDraws::UpdateShadowCapability), in Core's import order (Precipitation, then Sky): the texture to import as the map's
	 *   depth target.
	 * - referenceDue (optional, parity): render thread, where the engine asks whether this frame renders the map's reference
	 *   (OcclusionReferenceFrame): whether it does.
	 */
	struct OcclusionMapContribution
	{
		OcclusionMap map = OcclusionMap::Count;
		ToggleId toggle;
		const bool* available = nullptr;
		ViewBits bits;
		OccluderRule rule = OccluderRule::Precipitation;
		OcclusionTarget target = OcclusionTarget::EngineMask;
		OcclusionLabels labels;
		void (*importTarget)(OcclusionTargetImport& a_import) = nullptr;
		bool (*referenceDue)() = nullptr;
	};

	/**
	 * @brief A shadow source: the lights whose views the shadow engine draws. Sun and Spot share the clamped mode and its caster phase,
	 * Point is the paraboloid, Focus the focus views (any host's); the order is Core's (ShadowViews::Rebuild's views map to it by kind).
	 */
	enum class ShadowSource : std::uint8_t
	{
		Sun,
		Spot,
		Point,
		Focus,
		Count,
	};
	/**
	 * @brief A shadow source contribution: the source's views are drawn while it is registered (a view of a source none registered stays
	 * the engine's). Its cameras are the feature's to compute, into Core's boards (Core/Render/Shadow/ShadowCameras.h), from its
	 * Feature::cameraHooks.
	 *
	 * - lightKept (Spot and Point, required): render thread, at the end of the light selection, once per kept light of the source's kind,
	 *   in the selection's order.
	 */
	struct ShadowSourceContribution
	{
		ShadowSource source = ShadowSource::Count;
		void (*lightKept)(const ShadowLightKept& a_light) = nullptr;
	};
}
