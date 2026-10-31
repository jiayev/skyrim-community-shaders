#pragma once

// The HZB's GPU layout, which must match HzbCS.hlsl (features/Drawcall Limit Fix/Shaders/DrawcallLimitFix/Features/GpuOcclusion): its
// shader path, the downsample's constants and its levels and tile. Included by the feature's sources only.

#include <cstdint>

namespace DCLF::Features::GpuOcclusion
{
	inline constexpr const char* kHzbShader = "DrawcallLimitFix/Features/GpuOcclusion/HzbCS.hlsl";
	// HzbCS.hlsl's constants (HzbConstants): the source, its valid and padded extents, the levels and descriptors.
	inline constexpr std::uint32_t kHzbConstantWords = 24;
	// The levels one single-pass downsample (FidelityFX SPD) writes: a 4096-texel domain to one texel.
	inline constexpr std::uint32_t kHzbDispatchMips = 12;
	// Its group's tile of the source.
	inline constexpr std::uint32_t kHzbTile = 64;

	/** @brief HzbCS.hlsl's constants: one single-pass downsample of the chain. */
	struct HzbConstants
	{
		std::uint32_t sourceIndex;
		std::uint32_t fromDepth;
		std::uint32_t validSize[2];   // the source texels holding depth
		std::uint32_t domainSize[2];  // the source's power-of-two extent; past validSize it reads the far plane
		std::uint32_t targetSize[2];  // the first written level's
		std::uint32_t mips;
		std::uint32_t workGroups;
		std::uint32_t counterIndex;
		std::uint32_t padding;
		std::uint32_t targetIndices[kHzbDispatchMips];
	};
	static_assert(sizeof(HzbConstants) / 4 == kHzbConstantWords);
}
