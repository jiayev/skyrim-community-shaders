#pragma once

// The texture transforms (TexcoordOffset) and the ports' declared frame variables (Api::MaterialFrameVariable: the Effect port's falloff
// and base colour) the frame patches into the main material rows: the snapshot's list of rows that carry one (the payload product,
// built with the main payload), the rows the feature's frame-value pool samples for it, and the pass that writes them into the rows
// (TextureTransformsCS.hlsl). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF::Api
{
	struct MaterialFrameComponent;
}

namespace DCLF::Features::TextureTransforms
{
	/**
	 * @brief One row of the pool's texture transform buffer (TextureTransformsCS.hlsl's TextureTransformRow; must match). Row 0 is
	 * the header: rowOffset holds the entry count E, mask the animated rows' count A. Entry i is row i + 1: the material row's
	 * TexcoordOffset as the frame's shaders read it (the material's texCoordOffset / texCoordScale of the frame's buffer), or a declared
	 * frame variable's live value (its read), stored at rowOffset bytes past the material rows' address - into that row's frame copy
	 * (MaterialRowHeader::animatedRow), component c where mask bit c is set (a component the record never wrote stays the row's). Animated
	 * row k is row E + 1 + k: rowOffset the installed row's bytes past the material rows' address (~0u: a free row, not copied), which
	 * the pass copies whole into the frame copy's row k before any entry is stored.
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
	 * @brief The rows of one main payload that carry TexcoordOffset or a declared frame variable (the product's build, immutable): an
	 * entry per variable a packed row of a live slot places (MainRows::MaterialState: texcoordDword, frameVariables). The frame patches all
	 * of them, not only the moving ones, so a payload backing holds this frame's value whatever its history (a patched value the builder's
	 * journal does not know of is overwritten every frame). The material pointers are the slots' keys, alive while the publication the
	 * payload belongs to lives (the coordinator's material references).
	 */
	struct TextureTransformList
	{
		struct Entry
		{
			const RE::BSShaderMaterial* material = nullptr;
			std::uint32_t rowOffset = 0;  // bytes from the material rows' address to the first component
			std::uint32_t mask = 0;       // the components the record wrote (bit c)
			// A declared frame variable's read under the row's pass (Api::MaterialFrameVariable::ComponentsFor: 4, the port's static
			// storage); null: TexcoordOffset, from the frame's texture-transform buffer.
			const Api::MaterialFrameComponent* components = nullptr;
			bool operator==(const Entry&) const = default;
		};
		std::vector<Entry> entries;
		std::size_t variables = 0;  // the entries of declared frame variables
		// The payload's animated rows (MainRows::AnimatedRows::slotOf, as its build left them): per row of the frame copy, the material slot
		// copied into it (~0u free). Every row whose header names one, so the copy holds whatever the draws are pointed at.
		std::vector<std::uint32_t> animatedSlots;
		// The build's own (TextureTransformsList.cpp), so the next one rewrites only the slots that moved and keeps the rest: each entry's
		// material slot (ascending, parallel to entries), each covered slot's verdict, and the slots left out by reason (unpacked, without
		// TexcoordOffset or a frame variable, free, stale).
		std::vector<std::uint32_t> entrySlots;
		std::vector<std::uint8_t> slotVerdicts;
		std::array<std::uint64_t, 4> leftOut{};
	};

	/**
	 * @brief The list's way to the frame, stage by stage (any thread, relaxed; the pool's line takes them): lists the builder made and
	 * their entries, with the rows it left out by reason; lists a payload carried (PayloadPools::MakeSource); lists installed (adoption).
	 */
	namespace TextureTransformStats
	{
		inline std::atomic<std::uint64_t> built{ 0 }, builtEntries{ 0 }, builtVariables{ 0 }, rowsUnpacked{ 0 }, rowsNoTexcoord{ 0 }, rowsFreeSlot{ 0 }, rowsStale{ 0 };
		inline std::atomic<std::uint64_t> published{ 0 }, publishedEntries{ 0 }, publishedNone{ 0 };
		inline std::atomic<std::uint64_t> adopted{ 0 }, adoptedEntries{ 0 }, adoptedNone{ 0 };
	}

	// TextureTransformsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
	// TextureTransformsPool.cpp: the frame-value pool, and the installed list it samples (the product's install).
	void RegisterPool(Api::InstallContext& a_context);
	void InstallList(std::shared_ptr<const void> a_list);
	/** @brief Render thread: the pool's backing the current frame reads (0 before the first frame): the depth segment's stage word. */
	std::uint32_t RowsIndex();
	/**
	 * @brief Render thread: the device address of the frame's copy of the animated material rows (the animated-material-rows pool's
	 * backing, kMaterialRowBytes a row; 0 without a producer or without the pass's program): every main-rows latch's animatedRows.
	 */
	std::uint64_t AnimatedRowsAddress();
	/** @brief Render thread, the pass's setup: whether its program was made (without it nothing writes the copy, and no draw is pointed at it). */
	void SetCopyAvailable(bool a_available);
	// TextureTransformsList.cpp: the payload product.
	void RegisterProduct(Api::InstallContext& a_context);
	// Diagnostics/TextureTransformsParity.cpp, the builder's parity (Api::PayloadProductBuild::check): the list a build made from its previous
	// one against the list made whole from the same rows and tables.
	void CheckListParity(const TextureTransformList& a_built, const TextureTransformList& a_whole);
	// TextureTransformsPass.cpp: the pass (texture-transforms.apply).
	void RegisterPass(Api::InstallContext& a_context);
}
