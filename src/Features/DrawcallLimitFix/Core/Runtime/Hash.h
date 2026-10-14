#pragma once

#include <cstdint>

namespace DCLF
{
	/** @brief A 64-bit FNV-1a hash over values folded in one at a time: the input signatures' hash. */
	struct Fnv1a
	{
		std::uint64_t value = 0xcbf29ce484222325ull;
		void Mix(std::uint64_t a_value) { value = (value ^ a_value) * 0x100000001b3ull; }
		void Mix(const void* a_pointer) { Mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_pointer))); }
	};
}
