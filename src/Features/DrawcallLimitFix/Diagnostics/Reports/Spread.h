#pragma once

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

namespace DCLF::Reports
{
	/** @brief avg / p95 / max of a_values (sorted here) with a_decimals decimals, "-" without any. */
	inline std::string Spread(std::vector<double>& a_values, int a_decimals = 3)
	{
		if (a_values.empty())
			return "-";
		std::sort(a_values.begin(), a_values.end());
		double sum = 0.0;
		for (const double value : a_values)
			sum += value;
		const std::size_t p95 = (std::min)(a_values.size() - 1, a_values.size() * 95 / 100);
		return fmt::format("{:.{}f}/{:.{}f}/{:.{}f}", sum / double(a_values.size()), a_decimals, a_values[p95], a_decimals, a_values.back(), a_decimals);
	}

	/** @brief p50 / p95 / max of a_values (sorted here), "-" without any. */
	template <class T>
	std::string Percentiles(std::vector<T>& a_values)
	{
		if (a_values.empty())
			return "-";
		std::sort(a_values.begin(), a_values.end());
		const auto at = [&a_values](std::size_t a_percent) { return a_values[(std::min)(a_values.size() - 1, a_values.size() * a_percent / 100)]; };
		if constexpr (std::is_floating_point_v<T>)
			return fmt::format("{:.2f}/{:.2f}/{:.2f}", at(50), at(95), a_values.back());
		else
			return fmt::format("{}/{}/{}", at(50), at(95), a_values.back());
	}
}
