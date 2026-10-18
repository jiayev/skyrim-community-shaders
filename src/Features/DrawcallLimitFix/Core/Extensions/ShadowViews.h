#pragma once

// The shadow-view contributions' Core side (Api/ShadowViews.h): each occlusion map's and shadow source's entry and the exclusion
// contribution's, resolved once at InstallFeatures (ResolveShadowViews) from the features' registrations, and what the shadow engine
// derives from them for its per-object and per-frame paths (each mode's set phase, the set decision's readiness order, the source of each
// engine view kind, the maps' import order). The shadow capability, the captures, both shadow epochs, the set decisions, the
// classification and the reports read them as data; the contexts the callbacks are handed are defined here (but the exclusion build's,
// Core/Render/Shadow/ShadowExclusionBuild.h).

#include "Features/DrawcallLimitFix/Api/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

struct ID3D11Texture2D;
struct ID3D11DepthStencilView;

namespace RE
{
	class BSShadowLight;
	class NiAVObject;
	class NiCamera;
	class BSShaderAccumulator;
}

namespace DCLF::ShadowCameras
{
	struct Camera;
}

namespace DCLF::Api
{
	/**
	 * @brief What an ImportedTexture map's importTarget is handed: the texture and depth view to import as its depth target, and whether
	 * it supplied them (none: nothing is imported this time).
	 */
	class OcclusionTargetImport
	{
	public:
		bool supplied = false;
		ID3D11Texture2D* texture = nullptr;
		ID3D11DepthStencilView* view = nullptr;
	};

	/**
	 * @brief What a map's technique is handed: the walked object's records as its occluder rule reads them (the mirror's, on the scene
	 * work). The nearest fade node's BSX flags are walked only when asked (the walk's own, through nearestFadeBsx).
	 */
	class OccluderLeaf
	{
	public:
		std::uint64_t flags = 0;    // its Lighting property's (BSShaderProperty::flags)
		bool alphaTesting = false;  // NiAlphaProperty::GetAlphaTesting
		bool referenced = false;    // its node has a reference (user data)
		/** @brief The BSX flags of the nearest fade node above the object, by the mirror's parents (-1: none, or none read). */
		std::int32_t NearestFadeNodeBsx() const { return nearestFadeBsx(*this); }

		// The walk's: what NearestFadeNodeBsx walks (the coordinator and the object's records).
		const void* scene = nullptr;
		const void* records = nullptr;
		std::int32_t (*nearestFadeBsx)(const OccluderLeaf& a_leaf) = nullptr;
	};

	/** @brief What a map's camera is handed: the camera it is drawn from (none: the view is not set up). */
	class OcclusionCamera
	{
	public:
		const RE::NiCamera* camera = nullptr;
	};

	/** @brief What a map's ownsAccumulator is handed: the 0x1C accumulator that finished accumulating. */
	class OcclusionAccumulator
	{
	public:
		const RE::BSShaderAccumulator* accumulator = nullptr;
	};

	/** @brief A light the selection kept (ShadowSourceContribution::lightKept), and the scene frame of the selection. */
	class ShadowLightKept
	{
	public:
		const RE::BSShadowLight* light = nullptr;
		std::uint32_t sceneFrame = 0;
	};

	/**
	 * @brief What a source's camera is handed: a view (its light, its descriptor, the scene frame), and what it answers: DCLF's camera
	 * for the view (null: none this frame) and, a focus view's, its target's node (null: past the targets).
	 */
	class ShadowSourceCamera
	{
	public:
		const RE::BSShadowLight* light = nullptr;
		std::uint32_t descriptor = 0;
		std::uint32_t sceneFrame = 0;
		const ShadowCameras::Camera* camera = nullptr;
		const RE::NiAVObject* focusTarget = nullptr;
	};
}

namespace DCLF::Extensions
{
	inline constexpr std::size_t kOcclusionMaps = static_cast<std::size_t>(Api::OcclusionMap::Count);
	inline constexpr std::size_t kShadowSources = static_cast<std::size_t>(Api::ShadowSource::Count);
	// The shadow engine's modes (Draws::kShadowModeCount: plain, clamped, paraboloid, then one per map) and the engine views' kinds
	// (ShadowViews::Kind: directional, frustum, parabolic, other); both checked where they are defined.
	inline constexpr std::size_t kShadowEngineModes = 3 + kOcclusionMaps;
	inline constexpr std::size_t kShadowViewKinds = 4;

	/** @brief A map's resolved entry: its feature's registration (none: registered false, every field its default). */
	struct OcclusionMapEntry
	{
		bool registered = false;
		Api::ToggleId toggle;                         // the feature's toggle
		const bool* available = nullptr;              // the load-time flag, read whenever the toggle is
		std::uint8_t phase = 0;                       // its set phase
		std::uint32_t viewBit = 0;                    // its view bit at the GPU cull
		std::uint32_t (*technique)(const Api::OccluderLeaf&) = nullptr;  // its occluder rule, per walked entry
		bool minRadius = false;                       // its cull's size test (kCullMinRadius)
		float (*floorHeight)() = nullptr;             // its cull's floor (a cull plane at the epoch's latch), none: no floor
		Api::OcclusionTarget target = Api::OcclusionTarget::EngineMask;
		std::string list, report, targetName;         // its labels, owned here
		void (*importTarget)(Api::OcclusionTargetImport&) = nullptr;
		bool (*referenceDue)() = nullptr;
		void (*camera)(Api::OcclusionCamera&) = nullptr;
		bool (*ownsAccumulator)(const Api::OcclusionAccumulator&) = nullptr;
	};
	/** @brief A source's resolved entry. */
	struct ShadowSourceEntry
	{
		bool registered = false;
		void (*camera)(Api::ShadowSourceCamera&) = nullptr;  // Registry refused a source without it
		void (*lightKept)(const Api::ShadowLightKept&) = nullptr;
		bool (*fullFrustumWindowEnd)() = nullptr;  // the Sun's: Registry refused a Sun source without it
		void (*frameStart)(std::uint32_t) = nullptr;
		void (*frameEnd)() = nullptr;
	};
	/** @brief Every map's and source's entry, and what the shadow engine reads of them. */
	struct ShadowViewSet
	{
		std::array<OcclusionMapEntry, kOcclusionMaps> maps;
		std::array<ShadowSourceEntry, kShadowSources> sources;
		// The exclusion contribution's callbacks (none: no exclusion; Registry refused one without any of them).
		void (*exclusion)(Api::ShadowExclusionBuild&) = nullptr;
		void (*exclusionPublish)(Api::ShadowSource, std::shared_ptr<void>) = nullptr;
		void (*exclusionFrameStart)() = nullptr;
		void (*listExclusion)(Api::ShadowListExclusion&) = nullptr;
		std::uint8_t occluderPhases = 0;     // the registered maps' phases
		std::uint32_t occluderViewBits = 0;  // and their view bits
		// Each mode's set phase (SetPhaseOfMode for the caster modes, the map's for an occlusion mode: 0 for a map none takes).
		std::array<std::uint8_t, kShadowEngineModes> phaseOfMode{};
		// The phases the set decision tests the slot's readiness for, in its order (Coordinator::DecideSlot): the casters' (kSetCaster,
		// kSetCasterPoint), then the registered maps', in slot order.
		std::array<std::uint8_t, 2 + kOcclusionMaps> readinessOrder{};
		std::uint32_t readinessCount = 0;
		// The source an engine view draws for, by kind and whether it is a focus view (Count: none, the kind's views have no camera of
		// DCLF's): what the capture reads the camera from and the view list covers by.
		std::array<std::array<Api::ShadowSource, 2>, kShadowViewKinds> sourceOf{};
		// The order the maps' targets are imported in (IndirectDraws::UpdateShadowCapability): the precipitation mask's, then the sky map's.
		std::array<Api::OcclusionMap, kOcclusionMaps> importOrder{};
	};

	namespace Detail
	{
		// Written only by ResolveShadowViews (the main thread, before DCLF's threads exist: their start orders these writes before every
		// read), read-only after: a plain read for the per-object paths.
		inline ShadowViewSet resolvedShadowViews;
	}

	/** @brief Any thread, after InstallFeatures (empty before): the maps' and sources' entries. */
	inline const ShadowViewSet& ShadowViewsSet() { return Detail::resolvedShadowViews; }
	/** @brief Any thread, after InstallFeatures: map a_map's entry (kOcclusionSky, kOcclusionPrecipitation). */
	inline const OcclusionMapEntry& OcclusionMapOf(std::uint32_t a_map) { return ShadowViewsSet().maps[a_map]; }
	/** @brief Any thread, after InstallFeatures: a_source's entry. */
	inline const ShadowSourceEntry& ShadowSourceOf(Api::ShadowSource a_source) { return ShadowViewsSet().sources[static_cast<std::size_t>(a_source)]; }
	/**
	 * @brief Render thread: a_source's camera for a_request's view (ShadowSourceContribution::camera), also in a_request.camera; null
	 * for a source none registered.
	 */
	inline const ShadowCameras::Camera* SourceCamera(Api::ShadowSource a_source, Api::ShadowSourceCamera& a_request)
	{
		if (const auto& source = ShadowSourceOf(a_source); source.camera)
			source.camera(a_request);
		return a_request.camera;
	}
	/**
	 * @brief The snapshot builder, in the shadow participant's payload: a_build's exclusion into a_build.result (the exclusion
	 * contribution's build; nothing without one).
	 */
	inline void BuildShadowExclusion(Api::ShadowExclusionBuild& a_build)
	{
		if (const auto exclusion = ShadowViewsSet().exclusion)
			exclusion(a_build);
	}
	/**
	 * @brief Render thread, after the shadow epoch drew (once DCLF draws): a_exclusion, the payload's opaque
	 * exclusion of a_source, handed back to the exclusion contribution (publish; nothing without one).
	 */
	inline void PublishShadowExclusion(Api::ShadowSource a_source, std::shared_ptr<void> a_exclusion)
	{
		if (const auto publish = ShadowViewsSet().exclusionPublish)
			publish(a_source, std::move(a_exclusion));
	}
	/**
	 * @brief Render thread, once a frame (FrameDriver, after the views' FrameStartBeforeCoverage work, before the lookups are installed):
	 * the exclusion contribution's frameStart (nothing without one).
	 */
	inline void ShadowExclusionFrameStart()
	{
		if (const auto frameStart = ShadowViewsSet().exclusionFrameStart)
			frameStart();
	}
	/**
	 * @brief Render thread: the Sun source's pending exclusion as the scene-list filter reads it (the exclusion contribution's
	 * listExclusion), into a_view; left empty without one.
	 */
	inline void ListExclusion(Api::ShadowListExclusion& a_view)
	{
		if (const auto listExclusion = ShadowViewsSet().listExclusion)
			listExclusion(a_view);
	}
	/**
	 * @brief Main thread, in InstallFeatures (Registry), after the features' installs: the entries from the registered contributions (by
	 * map and source; Count: none; the exclusion's, no name: none). Once.
	 */
	void ResolveShadowViews(std::span<const Api::OcclusionMapContribution, kOcclusionMaps> a_maps,
		std::span<const Api::ShadowSourceContribution, kShadowSources> a_sources, const Api::ShadowExclusionContribution& a_exclusion);
	/**
	 * @brief Main thread, from the source's hooks on (InstallSourceHooks): whether a_source's feature installed its engine culls' hooks
	 * (Api::ShadowSourceHooks::CullsInstalled; Features/Shadows/Local's point culls install only after the Sun's).
	 */
	bool SourceCullsInstalled(Api::ShadowSource a_source);
	/**
	 * @brief Render thread, once a frame (FrameDriver, after the frame requests are served, before the main renderers are refreshed): each
	 * registered source's frameStart, in source order.
	 */
	void ShadowSourcesFrameStart(std::uint32_t a_frame);
	/** @brief Render thread, at Present (first in PrimaryCull::EndFrame): each registered source's frameEnd, in source order. */
	void ShadowSourcesFrameEnd();

	/**
	 * @brief Whether DCLF draws occlusion map a_map under a_toggles: taken, available (its load-time flag: Features/OcclusionMaps' is
	 * Skylighting's, whose hook drives both maps) and its toggle on. False past the maps.
	 */
	inline bool OcclusionOn(const ToggleSet& a_toggles, std::uint32_t a_map)
	{
		if (a_map >= kOcclusionMaps)
			return false;
		const auto& map = OcclusionMapOf(a_map);
		return map.registered && *map.available && a_toggles.On(map.toggle);
	}
}
