#pragma once

// The TreeInstances draw family's GPU layout (Api::DrawFamily::TreeInstances; dclf-architecture.md, "Tree LOD: the draws"): the records, rows and list header its draws
// read - TreeLodCullCS.hlsl (the depth segment's cull and a trees view's per-face cull), DistantTree.hlsl's DCLF_PULLED path, a trees view's
// faces' rows and lists (sized for Core by the family's faces data: rowBytes, headerBytes, unitWords) - and its engine shapes. The shaders
// mirror it, so it stays compile-time. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <array>
#include <cstdint>

namespace DCLF::TreeInstances
{
	/*
	 * The engine's instanced shapes (AE 1.6.1170; dclf-architecture.md, "Tree LOD: what the engine does"): each BGSDistantTreeBlock's TreeGroup
	 * becomes one BSMultiStreamInstanceTriShape under the LOD trees root, drawn instanced from one instance group, whose records the
	 * refill (FUN_140504a40) packs, at most 75 of them, into 32-byte records.
	 */
	inline constexpr std::uint32_t kMaxGroupInstances = 75;  // the refill's cap (0x4B), and the property's (+0x88)
	inline constexpr std::uint32_t kInstanceShorts = 16;     // shorts per instance record (AddGroup's p2)

	/**
	 * @brief One instance record, as the refill packs it (FUN_140504a40), halfs unless noted:
	 * [0..2] position (InstanceData x, y, z, block-relative); [3] scale, 0 when hidden (the block's allVisible ignores hidden);
	 * [4] cos and [5] sin of rotZ; [6] alpha (InstanceData::alpha, copied); [7] 1.0; [8..15] zero.
	 * DistantTree.hlsl reads it as TEXCOORD4 (position, scale) and TEXCOORD5 (cos, sin, alpha, 1).
	 */
	struct Instance
	{
		std::array<std::uint16_t, kInstanceShorts> shorts{};
		bool operator==(const Instance&) const = default;
	};
	static_assert(sizeof(Instance) == 32);

	/*
	 * The GPU's tables (TreeLodCullCS.hlsl, DistantTree.hlsl's DCLF_PULLED path), all read through device addresses:
	 * - a shape row per shape slot, and kMaxGroupInstances instance records per shape slot (slot * 75 + i);
	 * - a mesh row per mesh slot;
	 * - the draw row, one, every depth commit's: the tables' addresses and the frame's texture, sampler and alpha reference;
	 * - the visible list (VisibleHeader): the draws' arguments and the retest count, then the visible instances' indices, which
	 *   the cull appends and the draws' vertex stage reads by its instance, then the retest list (phase 1's occluded instances,
	 *   which phase 2 tests again against the rebuilt HZB), each a word per instance record the tables hold.
	 */
	struct ShapeRow
	{
		float translate[3]{};
		std::uint32_t mesh = 0;
		std::uint32_t count = 0;  // the slot's records; 0 for a free slot
		float extent = 0.0f;      // the type's: an instance's bound is its scale times this, about its position
		std::uint32_t pad[2]{};
	};
	static_assert(sizeof(ShapeRow) == 32);

	struct MeshRow
	{
		std::uint64_t vertexAddress = 0;
		std::uint64_t indexAddress = 0;  // R16 indices
		std::uint32_t vertexStride = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t texcoordOffset = 0;  // TEXCOORD0's, a half2; the position is a float4 at 0 (VertexInput.cpp)
		std::uint32_t pad = 0;
	};
	static_assert(sizeof(MeshRow) == 32);

	struct DrawRow
	{
		std::uint64_t instances = 0, shapes = 0, meshes = 0, visible = 0;  // visible: the list's indices (past the arguments)
		std::uint32_t textureIndex = 0, samplerIndex = 0;
		float alphaRef = 0.0f;
		std::uint32_t shapeSlots = 0;
		std::uint32_t maxIndices = 0;
		// CS_DCLF_TREE_LOD_STAGES: the stage stamps' buffer (0: none; DistantTree.hlsl's DCLFTreeStamp) and the depth commit's frame.
		std::uint32_t statsLow = 0, statsHigh = 0, frame = 0;
	};
	static_assert(sizeof(DrawRow) == 64);
	/**
	 * @brief The visible list's header (TreeLodCullCS.hlsl): three non-indexed DrawInstanced's arguments (the meshes' largest index
	 * count, the instances, 0, 0) - phase 1's depth draw, phase 2's (its instances follow phase 1's in the list), the colour
	 * pass's (both) - and the retest list's count. The depth commit writes it with no instances.
	 */
	struct VisibleHeader
	{
		std::uint32_t phaseOne[4]{}, phaseTwo[4]{}, colour[4]{};
		std::uint32_t retests = 0, pad[3]{};
	};
	static_assert(sizeof(VisibleHeader) == 64);
	inline constexpr std::uint32_t kVisibleHeaderWords = sizeof(VisibleHeader) / 4;
	/**
	 * @brief CS_DCLF_TREE_LOD_STAGES' stamps: kStageSlots rings of kStageWords words, slot frame % kStageSlots; each stage of the main draws
	 * stores the draw row's frame in its word (DistantTree.hlsl), the colour vertex stage also the list header's counts it saw.
	 */
	inline constexpr std::uint32_t kStageSlots = 8, kStageWords = 16;
	enum StageWord : std::uint32_t
	{
		kStageDepthVS = 0,
		kStagePhaseTwoVS = 1,
		kStageDepthPS = 2,
		kStageColourVS = 3,
		kStageColourPS = 4,
		kStageColourSawPhaseOne = 5,
		kStageColourSawPhaseTwo = 6,
		kStageColourSawColour = 7,
	};

	/**
	 * @brief Whether a geometry is one of the family's engine shapes: its shader property is a BSDistantTreeShaderProperty. Any thread
	 * (its feature's channel and parity; PassCapture's withholding reads the family's DrawFamilyEntry::engineShapeVtable instead).
	 */
	inline bool IsEngineShape(const RE::BSGeometry* a_shape)
	{
		const auto* property = a_shape ? a_shape->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		static const std::uintptr_t treeVtable = RE::VTABLE_BSDistantTreeShaderProperty[0].address();
		return Engine::VtableIs(property, treeVtable);
	}
}
