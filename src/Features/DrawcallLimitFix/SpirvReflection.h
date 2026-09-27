#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DCLF
{
	/**
	 * @brief What DCLF needs to know about a DXC SPIR-V module: its stage inputs (by HLSL semantic) and
	 * the descriptor bindings its code uses (not merely declares). Only the instructions DXC emits for these are parsed.
	 */
	struct SpirvReflection
	{
		struct Input
		{
			std::string semantic;         // HLSL semantic name, e.g. "TEXCOORD" (from DXC's "in.var.TEXCOORD1")
			std::uint32_t semanticIndex;  // e.g. 1
			std::uint32_t location;
		};

		enum class BindingKind : std::uint32_t
		{
			ConstantBuffer,
			Texture,  // sampled or storage image
			Sampler,
			StorageBuffer,  // structured / byte address buffers
			Other,
		};

		struct Binding
		{
			std::uint32_t set;
			std::uint32_t binding;
			BindingKind kind;
		};

		std::vector<Input> inputs;
		std::vector<Binding> bindings;

		/** @brief Parses a module; false if it is not SPIR-V. */
		bool Parse(std::span<const std::byte> a_spirv);
	};
}
