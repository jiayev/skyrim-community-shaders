#pragma once

// The texture transforms (TexcoordOffset) the frame patches into the main material rows: the snapshot's list of rows that carry one
// (MainBuild, with the payload), and the rows FrameValues samples for it, which TextureTransformsCS.hlsl writes into the rows.

#include <atomic>
#include <cstdint>
#include <vector>

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF
{
	/**
	 * @brief One row of FrameValues' texture transform buffer (TextureTransformsCS.hlsl's TextureTransformRow; must match). Row 0 is
	 * the header: rowOffset holds the entry count. Entry i is row i + 1: the material row's TexcoordOffset as the frame's shaders read
	 * it (the material's texCoordOffset / texCoordScale of the frame's buffer), stored at rowOffset bytes past the material rows'
	 * address, component c where mask bit c is set (a component the record never wrote stays the row's).
	 */
	struct TextureTransformRow
	{
		std::uint32_t rowOffset = 0;
		std::uint32_t mask = 0;
		std::uint32_t padding[2]{};
		float values[4]{};
	};
	static_assert(sizeof(TextureTransformRow) == 32);

	/**
	 * @brief The rows of one main payload that carry TexcoordOffset (MainBuild::Finish, immutable): every packed row of a live slot
	 * whose VS block places it. The frame patches all of them, not only the scrolling ones, so a payload backing holds this frame's
	 * value whatever its history (a patched value the builder's journal does not know of is overwritten every frame). The material
	 * pointers are the slots' keys, alive while the publication the payload belongs to lives (the coordinator's material references).
	 */
	struct TextureTransformList
	{
		struct Entry
		{
			const RE::BSShaderMaterial* material = nullptr;
			std::uint32_t rowOffset = 0;  // bytes from the material rows' address to the first component
			std::uint32_t mask = 0;       // the components the record wrote (bit c)
			bool operator==(const Entry&) const = default;
		};
		std::vector<Entry> entries;
	};

	/**
	 * @brief The list's way to the frame, stage by stage (any thread, relaxed; the frame values' line takes them): lists the builder
	 * made and their entries, with the rows it left out by reason; lists a payload carried (PayloadBuilder::Publish); lists installed
	 * (FrameValues::SetTextureTransforms).
	 */
	namespace TextureTransformStats
	{
		inline std::atomic<std::uint64_t> built{ 0 }, builtEntries{ 0 }, rowsUnpacked{ 0 }, rowsNoTexcoord{ 0 }, rowsFreeSlot{ 0 }, rowsStale{ 0 };
		inline std::atomic<std::uint64_t> published{ 0 }, publishedEntries{ 0 }, publishedNone{ 0 };
		inline std::atomic<std::uint64_t> adopted{ 0 }, adoptedEntries{ 0 }, adoptedNone{ 0 };
	}
}
