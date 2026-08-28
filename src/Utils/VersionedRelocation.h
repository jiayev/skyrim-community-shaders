#pragma once

#include <REL/REL.h>
#include <SKSE/Version.h>

namespace Util::VersionedRelocation
{
	/**
	 * Select a value for SE, pre-1.7 AE, or 1.7.99-or-newer AE.
	 */
	template <class T>
	[[nodiscard]] T Select(T a_se, T a_ae, T a_ae1799) noexcept
	{
		if (REL::Module::IsAE()) {
			return REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99) ? a_ae1799 : a_ae;
		}
		return a_se;
	}

	/** Resolve an Address Library ID whose numeric ID changed in Skyrim 1.7.99. */
	[[nodiscard]] inline std::uintptr_t ResolveID(
		std::uint64_t a_se,
		std::uint64_t a_ae,
		std::uint64_t a_ae1799)
	{
		if (REL::Module::IsAE() && REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99)) {
			return REL::ID(a_ae1799).address();
		}
		return REL::RelocationID(a_se, a_ae).address();
	}
}
