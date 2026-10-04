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
	 * @brief The DCLF set (drawcall-limit-fix.md, "The DCLF set"): the objects DCLF draws, in every phase each takes part in,
	 * and the engine never draws in a view DCLF draws.
	 *
	 * An object is in it for a frame, per phase, when it is eligible (its verdict is None), takes part in the phase (bound, for the
	 * main camera's: its record has its pipeline and material slots) and is ready for it: everything the phase draws it with is
	 * compiled and imported (SceneStore::CommitSet). Each phase is whole - its views withhold exactly what its draws draw - and
	 * the set is decided once a frame, at the scene phase, held in the tables (Tables::setPhases, and the record's kObjectMember
	 * for the main phase), which every build selects by. This is the engine's copy of the same
	 * decision: published whole at the same point (PassCapture::PublishSet) and read by the registration hooks, which withhold
	 * a member's passes from the batch renderers of the views DCLF draws. It decides nothing.
	 */
	enum SetPhase : std::uint8_t
	{
		kSetMain = 1u << 0,                    // the main camera's Z-prepass and colour pass
		kSetCaster = 1u << 1,                  // the plain and clamped shadow views (the sun's cascades, spot lights)
		kSetOccluderSky = 1u << 2,             // Skylighting's occlusion map
		kSetOccluderPrecipitation = 1u << 3,   // the precipitation occlusion mask
		kSetCasterPoint = 1u << 4,             // the paraboloid shadow views (point lights)
		// The water reflection's cube map faces (dclf-lod.md, "Water reflections"): a LOD member whose forward pipeline is ready
		// and that was a main member at the last commit too (the faces draw from the last frame's depth inputs).
		kSetReflection = 1u << 5,
	};

	/**
	 * @brief The phase a shadow build's mode draws (IndirectDraws' mode index: 0 plain, 1 clamped, 2 paraboloid, then the occlusion
	 * maps, kFirstOcclusionMode + kOcclusionSky / kOcclusionPrecipitation).
	 */
	inline constexpr std::uint8_t SetPhaseOfMode(std::uint32_t a_mode)
	{
		return a_mode < 2 ? kSetCaster : a_mode == 2 ? kSetCasterPoint : a_mode == 3 ? kSetOccluderSky : kSetOccluderPrecipitation;
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
