#pragma once

#include <chrono>
#include <random>

#include "detail/XoshiroCpp.hpp"

namespace clib_util
{
	class RNG
	{
	public:
		RNG() :
			_rng(std::chrono::steady_clock::now().time_since_epoch().count())
		{}
		explicit RNG(const std::uint32_t a_seed) :
			_rng(a_seed)
		{}

		template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
		T Generate(T a_min, T a_max)
		{
			if constexpr (std::is_integral_v<T>) {
				std::uniform_int_distribution<T> distr(a_min, a_max);
				return distr(_rng);
			} else {
				std::uniform_real_distribution<T> distr(a_min, a_max);
				return distr(_rng);
			}
		}

		double Generate()
		{
			return XoshiroCpp::DoubleFromBits(_rng());
		}

	private:
		XoshiroCpp::Xoshiro256StarStar _rng;
	};
}