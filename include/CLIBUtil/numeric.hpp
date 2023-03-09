#pragma once

#include <cmath>

namespace clib_util
{
	// https://stackoverflow.com/a/253874
	namespace numeric
	{
		constexpr float EPSILON = std::numeric_limits<float>::epsilon();

		inline bool approximately_equal(const float a, const float b)
		{
			return std::fabs(a - b) <= ((std::fabs(a) < std::fabs(b) ? std::fabs(b) : std::fabs(a)) * EPSILON);
		}

		inline bool essentially_equal(const float a, const float b)
		{
			return std::fabs(a - b) <= ((std::fabs(a) > std::fabs(b) ? std::fabs(b) : std::fabs(a)) * EPSILON);
		}

		inline bool definitely_greater_than(const float a, const float b)
		{
			return (a - b) > ((std::fabs(a) < std::fabs(b) ? std::fabs(b) : std::fabs(a)) * EPSILON);
		}

		inline bool definitely_less_than(const float a, const float b)
		{
			return (b - a) > ((std::fabs(a) < std::fabs(b) ? std::fabs(b) : std::fabs(a)) * EPSILON);
		}
	}
}
