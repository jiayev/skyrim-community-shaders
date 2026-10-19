#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace DCLF::Portals::Parabolic
{
	/** @brief Captured-input replay ABI; process flags are ignorePreprocess bit 0 and back-accumulator-present bit 1. */
	struct Query
	{
		std::array<float, 4> bound{}, sphere{}, plane{};
		std::array<std::uint32_t, 4> control{};  // local program, effective mode, node flags, process flags
	};
	static_assert(sizeof(Query) == 64);
	/** @brief Process1's effect on traversal and alphaGroupStopIndex, not a replacement for ancestor/room traversal. */
	struct Decision
	{
		std::uint32_t visible = 0, hemispheres = 0, writesHemispheres = 0, invalid = 0;
		bool operator==(const Decision&) const = default;
	};
	static_assert(sizeof(Decision) == 16);
	inline constexpr std::uint32_t kAlwaysDraw = 1u << 11, kPreprocessed = 1u << 12, kHidden = 1u << 20;

	/** @brief FUN_14151a1e0: preserve the sqrt, subtraction order, strict comparisons and native hemisphere codes. */
	inline std::uint32_t SphereMask(const std::array<float, 4>& a_bound, const std::array<float, 4>& a_sphere,
		const std::array<float, 4>& a_plane, bool a_bothSides)
	{
		const float dx = a_bound[0] - a_sphere[0], dy = a_bound[1] - a_sphere[1], dz = a_bound[2] - a_sphere[2];
		if (!((std::sqrt((dx * dx + dy * dy) + dz * dz) - a_bound[3]) - a_sphere[3] < 0.0f))
			return 0;
		const float side = ((a_bound[1] * a_plane[1] + a_bound[0] * a_plane[0]) + a_bound[2] * a_plane[2]) - a_plane[3];
		if (side - a_bound[3] < 0.0f)
			return a_bothSides ? 3 : 1;
		return side > 0.0f ? 1 : (a_bothSides ? 2 : 0);
	}

	/** @brief Native Process1 control flow, given the independently evaluated compound predicate. CPU parity/reference only. */
	inline Decision Process(std::uint32_t a_mode, std::uint32_t a_flags, bool a_ignorePreprocess,
		bool a_compound, bool a_skipView, bool a_compoundPass, std::uint32_t a_sphereMask)
	{
		if (a_mode == 1)
			return { 1, 0, 0, 0 };
		if (a_mode == 2)
			return {};
		if ((a_flags & kPreprocessed) && !a_ignorePreprocess)
			return { (a_flags & kHidden) ? 0u : 1u, 0, 0, 0 };
		if (a_flags & kAlwaysDraw)
			return { 1, 0, 0, 0 };
		if (a_compound && a_mode != 3) {
			if (!a_compoundPass || (!a_skipView && !a_sphereMask))
				return {};
		}
		return { a_sphereMask ? 1u : 0u, a_sphereMask, 1, 0 };
	}
}
