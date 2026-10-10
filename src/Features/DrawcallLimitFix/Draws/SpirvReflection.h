#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DCLF
{
	/**
	 * @brief What DCLF needs to know about a DXC SPIR-V module: its stage inputs (by HLSL semantic), the descriptor bindings its
	 * code uses (not merely declares), and its constant buffers' members. Only the instructions DXC emits for these are parsed.
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

		/**
		 * @brief A member of a constant buffer the module keeps (a Uniform block variable; DXC names it after its cbuffer and keeps
		 * every member of one it keeps, used or not, with the cbuffer's layout: -fvk-use-dx-layout, packoffset honoured).
		 */
		struct BlockMember
		{
			std::string block;     // the block variable's name, e.g. "PerGeometry"
			std::string name;      // the member's (OpMemberName), e.g. "EyePosition"
			std::uint32_t offset;  // bytes from the block's start (its Offset decoration)
		};

		std::vector<Input> inputs;
		std::vector<Binding> bindings;
		std::vector<BlockMember> blockMembers;

		/** @brief Parses a module; false if it is not SPIR-V. */
		bool Parse(std::span<const std::byte> a_spirv);
	};
}
