#pragma once

// Raw access to the engine, for the hooks and the engine state DCLF reads without CommonLib declarations. DCLF runs on
// AE 1.6.1170 only: every offset here and at the hooks is that runtime's (module-relative, as REL::Offset takes them).

#include <cstdint>
#include <cstring>

namespace DCLF::Engine
{
	/** @brief Whether the instruction at a_address is a CALL rel32 to a_target: a hook site is what was reverse engineered. */
	inline bool CallsTo(std::uintptr_t a_address, std::uintptr_t a_target)
	{
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
		if (bytes[0] != 0xE8)
			return false;
		std::int32_t displacement = 0;
		std::memcpy(&displacement, bytes + 1, sizeof(displacement));
		return a_address + 5 + static_cast<std::intptr_t>(displacement) == a_target;
	}

	/** @brief A module global at a_offset. */
	template <class T>
	T& Global(std::uintptr_t a_offset)
	{
		return *reinterpret_cast<T*>(REL::Module::get().base() + a_offset);
	}

	/** @brief The field of type T at a_offset into a_base. */
	template <class T>
	T& At(const void* a_base, std::size_t a_offset)
	{
		return *reinterpret_cast<T*>(const_cast<std::byte*>(static_cast<const std::byte*>(a_base)) + a_offset);
	}

	/** @brief The performance counter, for the hooks' timings. */
	inline std::int64_t Now()
	{
		LARGE_INTEGER value{};
		QueryPerformanceCounter(&value);
		return value.QuadPart;
	}

	/**
	 * @brief BSGraphics::SetDirtyStates(false): applies the engine's pending render state (targets, viewport, clears) to
	 * D3D11, as its next draw would. Render thread.
	 */
	inline void ApplyPendingState()
	{
		static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
		setDirtyStates(false);
	}

	// BSShaderProperty's light data, and in it the mask of the shadow lights the geometry is registered with.
	constexpr std::size_t kPropertyLightData = 0x70;
	constexpr std::size_t kLightDataActiveMask = 0x1C;
}
