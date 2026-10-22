#pragma once

// The Grass draw family's GPU layout (Api::DrawFamily::Grass; dclf-architecture.md, "Grass: the draws"): the records, rows and
// argument arrays its passes and draws read - GrassCullCS.hlsl and GrassArgsCS.hlsl (the depth segment's cull and the draws' arguments,
// in the feature's shader folder) and RunGrass.hlsl's DCLF_PULLED path (package/Shaders) - and its engine shapes. The shaders mirror
// it byte for byte (little-endian), so it stays compile-time. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/OwnedRows.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace DCLF::GrassInstances
{
	/*
	 * The engine's grass (AE 1.6.1170): each cell's grass types become BSMultiStreamInstanceTriShapes with a BSGrassShaderProperty,
	 * each drawn instanced from its records - 32 bytes, 16 halfs an instance - which GrassShared's capture hands DCLF when the shape
	 * gets them (src/GrassShared/GrassCapture.h). The mirror (GrassMirror.h) keeps them in a flat instance table, a range per shape.
	 */
	inline constexpr std::uint32_t kInstanceShorts = 16;

	/**
	 * @brief One instance record, as the engine packs it (halfs): [0..2] the position, local to the shape's origin; [3] the colour
	 * scale; [4..6] InstanceData2.xyz and [7] .w; [8..10] InstanceData3.xyz and [11] .w; [12] InstanceData4.x; [13] InstanceData4.y,
	 * the scale variance; [14..15] unused. The rotation's rows are (h4, h5, h6), (h8, h9, h10) and (h12, h7, h11).
	 */
	struct Instance
	{
		std::array<std::uint16_t, kInstanceShorts> shorts{};
		bool operator==(const Instance&) const = default;
	};
	static_assert(sizeof(Instance) == 32);

	/** @brief A shape slot's row: its origin (the shape's world translation at capture), bucket and instance range. */
	struct ShapeRow
	{
		float origin[3]{};
		std::uint32_t bucket = 0;
		std::uint32_t firstInstance = 0;  // its range in the instance table
		std::uint32_t count = 0;          // 0: free, or not uploaded
		float spawnStart = 0.0f;          // the mirror's fade clock when the shape came (the spawn fade-in)
		std::uint32_t flags = 0;
	};
	static_assert(sizeof(ShapeRow) == 32);

	/** @brief A mesh slot's row: the vertex and index buffers' device addresses (R16 indices, read as words), its layout. */
	struct MeshRow
	{
		std::uint64_t vertexAddress = 0;
		std::uint64_t indexAddress = 0;
		std::uint64_t vertexDesc = 0;  // the engine's vertex descriptor (the pulled vertex stage's layout)
		std::uint32_t indexCount = 0;
		std::uint32_t vertexStride = 0;
	};
	static_assert(sizeof(MeshRow) == 32);

	// A bucket's LOD tiers: 0 the full mesh, 1 the middle LOD mesh, 2 the far one (Grass Optimizations' _LOD0 and _LOD1).
	inline constexpr std::uint32_t kTiers = 3;
	inline constexpr std::uint32_t kNoMesh = ~0u;
	// BucketRow::flags.
	inline constexpr std::uint32_t kBucketTruePbr = 1u << 0;       // drawn by the second colour program (TruePbr)
	inline constexpr std::uint32_t kBucketSphereNormal = 1u << 1;  // the GrassSphereNormal descriptor
	inline constexpr std::uint32_t kBucketTierOne = 1u << 2;       // a middle LOD mesh
	inline constexpr std::uint32_t kBucketTierTwo = 1u << 3;       // a far LOD mesh

	/**
	 * @brief A bucket slot's row: the shapes that draw alike (mesh, material, texture, descriptors), their meshes per tier, their
	 * visible lists' bases (one per tier, each the bucket's instance count long), and what their draws bind and the cull reads.
	 */
	struct BucketRow
	{
		std::uint32_t mesh[kTiers]{ kNoMesh, kNoMesh, kNoMesh };  // mesh slots; kNoMesh: none
		std::uint32_t flags = 0;
		std::uint32_t visibleBase[kTiers]{};  // the tiers' first entries in the visible list
		std::uint32_t instanceCount = 0;      // each tier's capacity in the visible list; 0: a free slot (or not drawable yet)
		DescriptorIndex diffuseIndex, depthSampler, colourSampler;  // OwnedRow's (GrassTables.cpp, BindBucket)
		float alphaRef = 0.0f;
		float boundCenter[3]{};
		float modelRadius = 0.0f;
		float wavePeriod = 0.0f;
		float distScale = 1.0f, minPixelScale = 1.0f;  // Grass Optimizations' mesh cost bias: 1/sqrt(cost), sqrt(cost)
		std::uint32_t pbrFlags = 0;
		float scaleMask[3]{};
		std::uint32_t pad = 0;
		DescriptorIndex normalIndex, rmaosIndex, featuresIndex, pbrSampler;
		float pbrParams1[4]{};  // roughness scale, specular level, 0, 0
		float pbrParams2[4]{};  // subsurface colour rgb, opacity
		std::uint32_t reserved[4]{};
	};
	static_assert(sizeof(BucketRow) == 160);
	static_assert(offsetof(BucketRow, flags) == 12 && offsetof(BucketRow, visibleBase) == 16 && offsetof(BucketRow, instanceCount) == 28 &&
				  offsetof(BucketRow, diffuseIndex) == 32 && offsetof(BucketRow, alphaRef) == 44 && offsetof(BucketRow, boundCenter) == 48 &&
				  offsetof(BucketRow, modelRadius) == 60 && offsetof(BucketRow, wavePeriod) == 64 && offsetof(BucketRow, distScale) == 68 &&
				  offsetof(BucketRow, minPixelScale) == 72 && offsetof(BucketRow, pbrFlags) == 76 && offsetof(BucketRow, scaleMask) == 80 &&
				  offsetof(BucketRow, normalIndex) == 96 && offsetof(BucketRow, pbrSampler) == 108 && offsetof(BucketRow, pbrParams1) == 112 &&
				  offsetof(BucketRow, pbrParams2) == 128 && offsetof(BucketRow, reserved) == 144);

	// InstanceFrame::flags: the tier, whether the instance is in the collision's range, whether it is shaded simply (far).
	inline constexpr std::uint32_t kFrameTierMask = 3u;
	inline constexpr std::uint32_t kFrameCollision = 1u << 2;
	inline constexpr std::uint32_t kFrameSimpleShading = 1u << 3;

	/** @brief An instance slot's per-frame values, the cull's (written before its occlusion verdict), the vertex stage's. */
	struct InstanceFrame
	{
		float windCur = 0.0f, windPrev = 0.0f;  // the wind scalars at this frame's and the previous frame's timer
		float fade = 0.0f;
		std::uint32_t flags = 0;
	};
	static_assert(sizeof(InstanceFrame) == 16);

	// CullRow::flags.
	inline constexpr std::uint32_t kCullMidLod = 1u << 0;     // Grass Optimizations' mesh LOD, middle tier
	inline constexpr std::uint32_t kCullFarLod = 1u << 1;     // and far tier
	inline constexpr std::uint32_t kCullOcclusion = 1u << 2;  // test the HZB

	/** @brief The cull's frame values (Grass Optimizations' CullParams, from GrassShared::Settings() and the engine's). */
	struct CullRow
	{
		float minPixelSize = 0.0f, fullDetailPx = 0.0f, minKeep = 1.0f, fadeBand = 0.15f;
		float costBias = 0.0f, costBiasStart = 0.0f, projScale = 0.0f, maxDistSq = 0.0f;
		float edgeFadeStart = 1.0f, alphaParam1 = 0.0f, alphaParam2 = 0.0f, invisibleFadeCull = 0.0f;
		float simpleShadingPx = 0.0f, collisionDistSq = 0.0f, midLodPx = 0.0f, farLodPx = 0.0f;
		float lodBandPx = 0.0f;
		std::uint32_t flags = 0;
		std::uint32_t pad[14]{};
	};
	static_assert(sizeof(CullRow) == 128 && offsetof(CullRow, alphaParam1) == 36 && offsetof(CullRow, alphaParam2) == 40 && offsetof(CullRow, flags) == 68);

	/**
	 * @brief The draw row: one, written by every depth commit (and latched by it), read by the cull, the arguments' pass and the
	 * pulled vertex and pixel stages (its address is their push data's). The tables' addresses (the adopted versions'), the slots the
	 * cull walks (0: grass draws nothing this frame), the engine's per-frame grass constants (BSGrassShader::SetupGeometry's b2),
	 * the shadow mask's and collision texture's descriptors, Grass Collision's PerFrame, the eye and the cull's values.
	 */
	struct DrawRow
	{
		std::uint64_t instances = 0, shapes = 0, buckets = 0, meshes = 0, owners = 0, frame = 0, visible = 0;
		std::uint32_t instanceSlots = 0, shapeSlots = 0;
		float windVector[4]{};
		float timeBase = 0.0f, prevTimeBase = 0.0f, fadeNow = 0.0f, fadeInTimeRcp = 0.0f;
		float dirLightDirection[4]{};  // w: ShadowClampValue
		float dirLightColor[4]{};      // w: FogNearColor.w
		float ambientColor[4]{};
		DescriptorIndex shadowMaskIndex;  // OwnedRow's (GrassTables.cpp, BindFrameTextures)
		DescriptorIndex collisionIndex = DescriptorIndex::Sentinel(~0u);  // ~0u: no collision displacement
		std::uint32_t bucketSlots = 0;
		std::uint32_t flags = 0;
		std::array<std::byte, 64> collision{};  // GrassCollision::PerFrame (GrassShared::Collision::perFrame)
		float eye[4]{};                         // the main camera's world position (the cull's distances)
		std::uint32_t reserved[4]{};
		CullRow cull;
	};
	static_assert(sizeof(DrawRow) == 384);
	static_assert(offsetof(DrawRow, owners) == 32 && offsetof(DrawRow, frame) == 40 && offsetof(DrawRow, visible) == 48 && offsetof(DrawRow, instanceSlots) == 56 &&
				  offsetof(DrawRow, windVector) == 64 && offsetof(DrawRow, timeBase) == 80 && offsetof(DrawRow, dirLightDirection) == 96 &&
				  offsetof(DrawRow, dirLightColor) == 112 && offsetof(DrawRow, ambientColor) == 128 && offsetof(DrawRow, shadowMaskIndex) == 144 &&
				  offsetof(DrawRow, collisionIndex) == 148 && offsetof(DrawRow, bucketSlots) == 152 && offsetof(DrawRow, flags) == 156 &&
				  offsetof(DrawRow, collision) == 160 && offsetof(DrawRow, eye) == 224 && offsetof(DrawRow, cull) == 256);

	/*
	 * The draws buffer (fixed, not versioned; the family's declared indirect-argument resource): per draw slot (a bucket's tier,
	 * ds = bucket * 3 + tier) the cull's count, phase 1's count, and four arrays of non-indexed DrawInstanced arguments - phase 1's
	 * depth draws, phase 2's, the colour draws of each colour program - each packed by GrassArgsCS with its draws' count in the header.
	 */
	inline constexpr std::uint32_t kMaxBuckets = 1024;
	inline constexpr std::uint32_t kMaxDrawSlots = kTiers * kMaxBuckets;

	/** @brief The draws buffer's header: the arrays' draw counts (read by ExecuteIndirect as its count), the cull's counters. */
	struct DrawsHeader
	{
		std::uint32_t phaseOneDraws = 0, phaseTwoDraws = 0, colourDraws[2]{};
		std::uint32_t retests = 0;            // phase 1's occluded instances (the retest list's length)
		std::uint32_t tested = 0;             // instances that reached the occlusion test in phase 1
		std::uint32_t phaseOneInstances = 0, phaseTwoInstances = 0;
		std::uint32_t stats[8]{};             // kStat*
	};
	static_assert(sizeof(DrawsHeader) == 64 && offsetof(DrawsHeader, retests) == 16 && offsetof(DrawsHeader, phaseOneInstances) == 24 &&
				  offsetof(DrawsHeader, stats) == 32);
	// DrawsHeader::stats: the live instances the cull walked, those culled by distance or the frustum, by size or thinning, by fade.
	inline constexpr std::uint32_t kStatLive = 0, kStatOutside = 1, kStatThinned = 2, kStatFaded = 3;

	/** @brief One non-indexed DrawInstanced's arguments (the family's signature). */
	struct DrawArgs
	{
		std::uint32_t vertexCount = 0, instanceCount = 0, firstVertex = 0, firstInstance = 0;
	};
	static_assert(sizeof(DrawArgs) == 16);

	inline constexpr std::uint64_t kCountsOffset = sizeof(DrawsHeader);
	inline constexpr std::uint64_t kPhaseOneCountsOffset = kCountsOffset + kMaxDrawSlots * sizeof(std::uint32_t);
	inline constexpr std::uint64_t kPhaseOneArgs = kPhaseOneCountsOffset + kMaxDrawSlots * sizeof(std::uint32_t);
	inline constexpr std::uint64_t kPhaseTwoArgs = kPhaseOneArgs + kMaxDrawSlots * sizeof(DrawArgs);
	inline constexpr std::uint64_t kColourArgs[2] = { kPhaseTwoArgs + kMaxDrawSlots * sizeof(DrawArgs), kPhaseTwoArgs + 2ull * kMaxDrawSlots * sizeof(DrawArgs) };
	inline constexpr std::uint64_t kDrawsBytes = kColourArgs[1] + kMaxDrawSlots * sizeof(DrawArgs);
	static_assert(kCountsOffset == 64 && kPhaseOneCountsOffset == 12352 && kPhaseOneArgs == 24640 && kPhaseTwoArgs == 73792 && kColourArgs[0] == 122944 &&
				  kColourArgs[1] == 172096 && kDrawsBytes == 221248);

	/*
	 * The visible list (versioned, a unit per instance slot, 16 bytes a unit): the visible entries (instance indices) at
	 * [0, 3 * units) words, each bucket's tiers at their visibleBase; then the retest list (phase 1's occluded instances) at
	 * kRetestOffsetPerUnit * units bytes.
	 */
	inline constexpr std::uint32_t kVisibleUnitBytes = 16;
	inline constexpr std::uint32_t kRetestOffsetPerUnit = 12;

	/**
	 * @brief Whether a geometry is one of the family's engine shapes: its shader property is a BSGrassShaderProperty. Any thread
	 * (its feature's parity; PassCapture's withholding reads the family's DrawFamilyEntry::engineShapeVtable instead).
	 */
	inline bool IsEngineShape(const RE::BSGeometry* a_shape)
	{
		const auto* property = a_shape ? a_shape->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		static const std::uintptr_t grassVtable = RE::VTABLE_BSGrassShaderProperty[0].address();
		return Engine::VtableIs(property, grassVtable);
	}
}
