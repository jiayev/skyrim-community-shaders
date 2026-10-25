#pragma once

// PortalPrograms.hlsli's evaluation on the CPU, for the CPU-driven pipeline (Core/Render/Direct/Cull.h): PortalTestAt over the encoding
// PortalViews::Encode writes (PortalPrograms.h), and StrictGateRejects over the strict gates' words (StrictGates.h) and the light context
// atlas (PortalContextAtlas.h). The same arithmetic and comparisons as the shader (PortalDistance's order, the six-plane set test, the
// validated instructions' walk), without PortalModel::Visible's reasons or throws: a producer invariant failure rejects the bound, as the
// GPU does. Any thread; reads only the words it is given.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"

#include <cstdint>
#include <memory>
#include <span>

namespace DCLF::Portals
{
	/**
	 * @brief PortalTestAt(a_words, a_baseWord * 4, a_program, ...): 1 in view, 0 culled, -1 no program. a_nodeFlags: always-draw bit 11,
	 * preprocessed 12, preprocess-hidden 20 (a member's query passes 0).
	 */
	int PortalTestCpuAt(std::span<const std::uint32_t> a_words, std::size_t a_baseWord, std::uint32_t a_program, const float a_centre[3], float a_radius,
		std::uint32_t a_nodeFlags);
	/** @brief The main camera's programs (the encoding at word 0): Direct::PortalTestFn. */
	int PortalTestCpu(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags);

	/** @brief StrictGateRejects: whether a strict light (a_context: its atlas entry) culls object a_object of the bound. Direct::StrictGates::rejects. */
	bool StrictGateRejectsCpu(const Direct::StrictGates& a_gates, std::uint32_t a_context, std::uint32_t a_object, const float a_centre[3], float a_radius);

	/**
	 * @brief The gates a strict light's CPU cull reads: the verdicts' words (StrictGates.h), the frame's atlas (PortalContextAtlas.h) and the
	 * object gates' column, held by a_hold for as long as the cull reads them.
	 */
	Direct::StrictGates StrictGatesOf(std::span<const std::uint32_t> a_gates, std::span<const std::uint32_t> a_atlas, std::span<const std::uint32_t> a_objectGates,
		std::shared_ptr<const void> a_hold);
}
