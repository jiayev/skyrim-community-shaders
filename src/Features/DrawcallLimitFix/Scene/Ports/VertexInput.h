#pragma once

#include <array>
#include <cstdint>
#include <dxgiformat.h>

namespace DCLF
{
	/**
	 * @brief The part of a BSGraphics::VertexDesc a pipeline depends on: attribute flags and offsets,
	 * without the stride (bits 0-3), which the indirect draws set per geometry.
	 */
	constexpr std::uint64_t VertexLayoutOf(std::uint64_t a_vertexDesc) { return a_vertexDesc & ~0xFull; }

	/**
	 * @brief A vertex shader's input mask, built like ShaderCache builds BSGraphics::VertexShader::vertexDesc:
	 * for each consumed attribute, its flag in both streams (bits 44 + a and 54 + a) and its offset nibble.
	 * The engine selects its input layout by (mask & geometry VertexDesc).
	 */
	void AddVertexAttribute(std::uint64_t& a_mask, std::uint32_t a_attribute);

	/** @brief One D3D11_INPUT_ELEMENT_DESC of an engine input layout. */
	struct VertexElement
	{
		const char* semantic;
		std::uint32_t semanticIndex;
		DXGI_FORMAT format;
		std::uint32_t slot;  // 0: the geometry's vertex buffer; 1: the second stream of dynamic geometry
		std::uint32_t offset;
		bool perInstance;
	};

	struct VertexElements
	{
		std::array<VertexElement, 16> elements{};
		std::uint32_t count = 0;
	};

	/**
	 * @brief The input layout the engine creates for a layout key (VS mask & geometry VertexDesc),
	 * ported from the builder BSGraphics::Renderer calls when the vertex description changes
	 * (AE 0x140e4bf20; engine notes: vertex input). Positions are always four floats.
	 */
	VertexElements BuildVertexElements(std::uint64_t a_key);
}
