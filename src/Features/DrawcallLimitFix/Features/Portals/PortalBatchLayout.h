#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace DCLF::Portals
{
	/** @brief Vulkan portable two-dimensional dispatch, with the shader's fixed 65535-group row pitch. */
	struct BatchDispatch
	{
		std::uint32_t x = 0, y = 1;
	};
	inline BatchDispatch DispatchFor(std::uint32_t a_elements, std::uint32_t a_width)
	{
		if (!a_width)
			throw std::logic_error("portal batch: zero workgroup width");
		const auto groups = (std::uint64_t(a_elements) + a_width - 1) / a_width;
		if (groups > 65535ull * 65535ull)
			throw std::overflow_error("portal batch: dispatch range exhausted");
		return { static_cast<std::uint32_t>(groups < 65535 ? groups : 65535),
			static_cast<std::uint32_t>(groups ? (groups + 65534) / 65535 : 1) };
	}

	struct ScanLevel
	{
		std::uint32_t base = 0, elements = 0;
	};
	/** @brief Exact scratch layout for the 256-wide hierarchical uint2 exclusive scan. Empty buckets still clear indirect work. */
	struct BatchScan
	{
		std::vector<ScanLevel> levels;
		std::uint32_t units = 0;
	};
	inline BatchScan ScanFor(std::uint32_t a_buckets)
	{
		BatchScan result;
		std::uint64_t base = 0;
		for (auto elements = a_buckets ? a_buckets : 1;;) {
			const auto end = base + elements;
			if (end > std::numeric_limits<std::uint32_t>::max())
				throw std::overflow_error("portal batch: scan address range exhausted");
			result.levels.push_back({ static_cast<std::uint32_t>(base), elements });
			result.units = static_cast<std::uint32_t>(end);
			const auto blocks = static_cast<std::uint32_t>((std::uint64_t(elements) + 255) / 256);
			if (blocks == 1)
				return result;
			base = end;
			elements = blocks;
		}
	}
}
