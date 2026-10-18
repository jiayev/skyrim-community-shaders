#pragma once

#include <cstdint>

#include <ankerl/unordered_dense.h>

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	/**
	 * @brief The DCLF set (dclf-architecture.md, "The DCLF set"): the objects DCLF draws, in every phase each takes part in,
	 * and the engine never draws in a view DCLF draws.
	 *
	 * An object is in it for a frame, per phase, when it is eligible (its verdict is None), takes part in the phase (bound, for the
	 * main camera's: its record has its pipeline and material slots) and is ready for it: everything the phase draws it with is
	 * compiled and imported (Coordinator::CommitSet). Each phase is whole - its views withhold exactly what its draws draw - and
	 * the set is decided once a frame, at the scene phase, held in the tables (SceneTables::setPhases, and the record's kObjectMember
	 * for the main phase), which every build selects by. SetSnapshot is the engine-facing copy of the same decision: published whole
	 * with the installed publication (FrameView::FrameClaims for DCLF's readers; PassCapture::PublishSet, the same object, for the
	 * registration hooks, which withhold a member's passes from the batch renderers of the views DCLF draws). It decides nothing.
	 */
	enum SetPhase : std::uint8_t
	{
		kSetMain = 1u << 0,                    // the main camera's Z-prepass and colour pass
		kSetCaster = 1u << 1,                  // the plain and clamped shadow views (the sun's cascades, spot lights)
		// 1u << 2, 1u << 3: the occlusion maps' (Features/OcclusionMaps' kSetOccluderSky, Skylighting's occlusion map, and
		// kSetOccluderPrecipitation, the precipitation occlusion mask; Api::OcclusionMapContribution::bits, Features/Manifest.cpp).
		kSetCasterPoint = 1u << 4,             // the paraboloid shadow views (point lights)
		// 1u << 5, 1u << 6: features' views' (Features/Reflection's kSetReflection: a LOD member whose forward pipeline is ready and that
		// was a main member at the last commit too, the faces drawing from the last frame's depth inputs; Features/FirstPerson's
		// kSetFirstPerson; Api::ViewContribution::phase, Features/Manifest.cpp).
	};
	/**
	 * @brief Core's set phases: a view's phase (a feature's own bit, of either view family) is none of them (Extensions::ResolveViewBits,
	 * Features/Manifest.cpp).
	 */
	inline constexpr std::uint8_t kCoreSetPhases = kSetMain | kSetCaster | kSetCasterPoint;

	/**
	 * @brief The phase a shadow build's caster mode draws (IndirectDraws' mode index: 0 plain, 1 clamped, 2 paraboloid). An occlusion
	 * map's mode (kFirstOcclusionMode + kOcclusionSky / kOcclusionPrecipitation) draws its map's phase: Extensions::ShadowViewSet::phaseOfMode.
	 */
	inline constexpr std::uint8_t SetPhaseOfMode(std::uint32_t a_mode)
	{
		return a_mode < 2 ? kSetCaster : kSetCasterPoint;
	}

	/** @brief The set for one frame, by geometry: each member's phases. Immutable once published. */
	struct SetSnapshot
	{
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint8_t> phases;
		std::uint32_t frame = 0;
		std::uint8_t drawn = 0;  // the phases DCLF draws this frame (a member's phases are within them)

		std::uint8_t PhasesOf(const RE::BSGeometry* a_geometry) const
		{
			const auto it = phases.find(a_geometry);
			return it != phases.end() ? it->second : std::uint8_t{ 0 };
		}
	};
}
