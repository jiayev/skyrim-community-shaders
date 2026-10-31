#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics
{
	/**
	 * @brief A parity check's counters, for its report line: what was checked, what differed, and the first difference.
	 * Every kept structure checks itself against a rebuild from the tables (CS_DCLF_PERSISTENT_PARITY) through one.
	 */
	struct ParityCounter
	{
		std::uint64_t checks = 0, mismatches = 0;
		std::string first;

		/** @brief One comparison; a_describe() names the first difference. Returns a_equal. */
		template <class F>
		bool Check(bool a_equal, F&& a_describe)
		{
			++checks;
			if (!a_equal && mismatches++ == 0)
				first = a_describe();
			return a_equal;
		}
		bool Check(bool a_equal) { return Check(a_equal, [] { return std::string(); }); }
		/** @brief The report's verdict: " <- OK", " <- DIFFER" (with the first difference when a_first), or nothing unchecked. */
		std::string Verdict(bool a_first = false) const
		{
			if (!checks)
				return {};
			if (!mismatches)
				return " <- OK";
			return a_first ? " <- DIFFER; first: " + first : " <- DIFFER";
		}
		void Reset() { *this = {}; }
	};
}
