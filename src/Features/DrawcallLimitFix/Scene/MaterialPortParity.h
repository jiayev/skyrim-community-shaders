#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "Features/DrawcallLimitFix/Scene/ConstantEvaluator.h"

namespace DCLF::MaterialPort
{
	/**
	 * @brief T6b2's material parity (an observer): the port's record against the engine evaluator's for the same material and pass
	 * descriptor, at the same point (the render thread, where the evaluator runs).
	 */
	struct ParityStats
	{
		std::uint64_t checked = 0, differ = 0, uncovered = 0;
		std::uint64_t iblDrift = 0;  // IBLParams components that moved between the port's sample and the engine's (unread: not compared)
		// By kind: VS constants, PS constants, textures, address modes, filter modes, textureWritten, featureTextures.
		std::array<std::uint64_t, 7> kinds{};
		std::string first;
	};

	/**
	 * @brief Empty when the records agree; else the first difference, named (the stage, variable and component of a constant, the
	 * texture slot), with the kinds that differ added to a_kinds (ParityStats::kinds' order).
	 */
	std::string DescribeDifference(const MaterialRecord& a_port, const MaterialRecord& a_engine, std::array<std::uint64_t, 7>* a_kinds = nullptr);
}
