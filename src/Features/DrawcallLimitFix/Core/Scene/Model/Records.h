#pragma once

// The scene's record types and their GPU layouts: object records and flags, geometry buffers, pipeline keys and raster flags, fade
// roots, the fade write-back and the indirect draw sequence. Written by the coordinator into SceneTables, read
// by the render thread's builds and by the shaders, whose HLSL layouts must match these.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <type_traits>

struct ID3D11Buffer;

namespace DCLF
{
	/** @brief Per-object record: the object table's GPU layout (16-byte aligned, no implicit padding), uploaded as is. */
	struct ObjectRecord
	{
		// The placement (world, previous world, bound) is not the record's: FrameValues samples it into the frame's rows.
		std::uint32_t geometryIndex;
		std::uint32_t materialIndex;
		std::uint32_t pipelineIndex;
		std::uint32_t flags;  // ObjectFlags, alpha-test threshold in bits 8-15
	};
	static_assert(sizeof(ObjectRecord) == 16);

	enum ObjectFlags : std::uint32_t
	{
		kObjectAlphaTest = 1u << 0,  // alpha test on, reference = threshold / 255 (as the native draw's AlphaTestRef)
		kObjectTwoSided = 1u << 1,   // no culling (the native main pass culls back faces otherwise)
		kObjectSuppressExternalEmittance = 1u << 2,  // ExtraShaderDescriptors::SuppressExternalEmittance in the permutation buffer
		// 1u << 3: a feature's (kFeatureObjectFlags; a Lighting object flag, Api::LightingObjectFlag).
		// A palette block: rows at BindlessObject::boneOffset / previousBoneOffset. A skin's (CS_DCLF_SKINNED: vertices from the skin
		// partition's own buffer, bone palette rows) or a particle member's quads. The shaders' name is kObjectSkinned.
		kObjectPaletteBlock = 1u << 4,
		// A culling candidate only: materialIndex and pipelineIndex mean nothing; check before indexing. The tables may be empty
		// (first frame after a teleport), so even index 0 is unsafe.
		kObjectNoBindings = 1u << 5,
		// 1u << 6: a feature's (kFeatureObjectFlags).
		// CS_DCLF_DECALS. Never an occluder: not in the depth segment, drawn by the colour segment's second pass after the opaque
		// draws. Group (bits 20-21): 1 = accumulation hint 2 (opaque, geometry group 3, first), 3 = multi-index layer (hint 12,
		// group 2, second), 2 = hint 3 (blended, group 4, last); the group's range is main range slot Fixed(group - 1)
		// (Api/MainRanges.h; Features/Decals registers their order).
		kObjectDecal = 1u << 7,
		kObjectAlphaThresholdShift = 8,
		// Alpha-tested and blended: the depth pass uses its own reference (BSUtilityShader::SetupGeometry's AlphaTestRef;
		// DCLFDepthAlphaTestRef).
		kObjectAlphaBlended = 1u << 17,
		// Extra PerGeometry rows at BindlessObject::extraOffset (kExtraRows, layout in FrameView::RefreshFrameConstants):
		// ProjectedUV's texture matrix and parameters (CS_DCLF_PROJECTED_UV), MTLand's blend parameters (CS_DCLF_MTLAND).
		kObjectProjectedUV = 1u << 18,
		kObjectLandBlend = 1u << 19,
		kObjectDecalGroupShift = 20,
		// Without kObjectDecal, a terrain group (CS_DCLF_TB_TERRAIN, while Terrain Blending holds the terrain): 1 = a
		// kMultiTextureLandscape block, drawn after opaque and decals with Terrain Blending's blend state, near blocks' depth into
		// its terrain depth and, offset, the main depth; 2 = a kNoTransparencyMultiSample mesh, drawn after it testing EQUAL.
		kObjectTerrainGroupShift = kObjectDecalGroupShift,
		// The engine draws no shadow-map pass for it (ShadowCasterReject), so no shadow view may accept it. Set by the scene phase.
		kObjectNoShadow = 1u << 22,
		// ShadowReject::VolumetricOnly: the engine draws it only into the sun's volumetric lighting copy (batch group 15, hint 8),
		// whose view draws only these; every other view skips them (BuildDrawsLatch::cullFlags). Set by the scene phase.
		kObjectVolumetricOnly = 1u << 23,
		// Main-pass ineligible for a reason the shadow views ignore (ShadowOnlyReason): a record for the shadow epochs alone.
		// Always with kObjectNoBindings.
		kObjectShadowOnly = 1u << 24,
		// A free object slot. Always with kObjectNoBindings and kObjectNoShadow, so loops skipping those skip it.
		kObjectFree = 1u << 25,
		// A synthetic main pass (PrimaryCull) with the sun's bits (DefShadow, ShadowDir): the colour BuildDraws tests its bound
		// against the cascades and on a miss sets kObjectSunMiss so the pixel stage drops them, as GetRenderPasses would.
		kObjectSunTest = 1u << 26,
		// A resident under a fade root (PrimaryCull): the depth segment's first phase drops it for good where BSFadeNode::OnVisible
		// would snap its fade to 0 (entry root past SceneTables::fadeDistance and not in view last frame).
		kObjectFadeTest = 1u << 27,
		// A resident tree (PrimaryCull): dropped for good where BSTreeNode::OnVisible's height test would (entry root's centre
		// above the frame's limit).
		kObjectHeightTest = 1u << 28,
		// A landscape property (Api::LightVolumesContribution::landscapeFlags: kLandscape, kNoLODLandFade): the main pass's light selection
		// gives it only the local shadow lights that take landscape (BSShadowLight +0x61; Features/LocalLights' LocalShadowLights).
		kObjectLandscapeLights = 1u << 29,
		// Bound by scene membership (Coordinator::BindByMembership): drawn whenever the GPU finds it. Every record with bindings.
		kObjectMember = 1u << 30,
		// CS_DCLF_FORWARD: a blended Lighting object. Never an occluder (view mask kViewForward alone); culled by the colour
		// BuildDraws (frustum, rebuilt HZB), sorted back to front (ForwardSortCS) and forward-shaded into the main target at the
		// engine's blended point (FUN_1414b39d0). Key: kRasterForward.
		kObjectForward = 1u << 31,
	};

	/**
	 * @brief The object flags the features' technique contributions own (Api::TechniqueContribution::objectFlags), each bit declared in
	 * its feature's header and checked by Features/Manifest.cpp against Core's and each other's: Features/TreeWind's kObjectTreeAnim
	 * (technique 12, whose TreeParams and WindTimers are per object; for anything else they are the pipeline template's, which a zeroed
	 * per-object copy would clobber); and the root test's bits a root states contribution sets (Api::RootStatesContribution::objectFlags):
	 * the fade roots' kObjectFadeTest (27) and kObjectHeightTest (28), still declared in Core's enum until Features/Fades holds them;
	 * and the Lighting object flags (Api::LightingObjectFlag: Integrations/SubsurfaceScattering's kObjectBeastRace, 3).
	 */
	inline constexpr std::uint32_t kFeatureObjectFlags = (1u << 3) | (1u << 6) | (1u << 27) | (1u << 28);

	/** @brief The high bit of a draw's object-index word: the draw misses every sun cascade (kObjectSunTest). */
	inline constexpr std::uint32_t kObjectSunMiss = 1u << 31;
	// The object word's local shadow lights (BuildDrawsCS, Api::LightVolumesContribution): Light Limit Fix's ShadowBitMask of the draw, whose
	// lights are shadow mask channels 0-3. The object index is below them.
	inline constexpr std::uint32_t kObjectLocalShadowShift = 26;
	inline constexpr std::uint32_t kObjectLocalShadowMask = 0xFu << kObjectLocalShadowShift;
	inline constexpr std::uint32_t kObjectIndexMask = (1u << kObjectLocalShadowShift) - 1;

	/**
	 * @brief An object's row of the vertex stream (Api/VertexStreams.h: SceneTables::streamRow), none: the buffer of its feature's scene
	 * resources (Features/Faces' face positions) holds a float4 per vertex of each stream object, in a range per row, bound as the second
	 * vertex stream (where the engine puts BSDynamicTriShape::dynamicData).
	 */
	inline constexpr std::uint32_t kNoStreamRow = ~0u;

	/** @brief Rows of per-object extras in the row buffer: LandBlendParams, TextureProj x3, ProjectedUVParams x3. */
	inline constexpr std::uint32_t kExtraRows = 7;
	inline constexpr std::uint32_t kExtraRowLandBlend = 0;
	inline constexpr std::uint32_t kExtraRowTextureProj = 1;
	inline constexpr std::uint32_t kExtraRowProjectedParams = 4;
	inline constexpr std::uint32_t kNoExtraRows = ~0u;

	/** @brief ObjectFlags -> decal group (0 for anything that is not a decal). */
	inline constexpr std::uint32_t ObjectDecalGroup(std::uint32_t a_flags) { return (a_flags & kObjectDecal) ? (a_flags >> kObjectDecalGroupShift) & 3u : 0u; }
	/** @brief ObjectFlags -> terrain group (0 for a decal, or anything Terrain Blending does not hold). */
	inline constexpr std::uint32_t ObjectTerrainGroup(std::uint32_t a_flags) { return (a_flags & kObjectDecal) ? 0u : (a_flags >> kObjectTerrainGroupShift) & 3u; }

	/** @brief GeometryBuffers::nextPartition: the last partition, or a TriShape that is not one. */
	inline constexpr std::uint32_t kNoPartition = ~0u;
	/** @brief The most partitions a skin DCLF draws may have: one bit each in the draw's partition mask. */
	inline constexpr std::uint32_t kMaxSkinPartitions = 8;
	/** @brief skinPartitions when the current LOD level draws no partition (SkinPartitionMask 0): still a member, no draw. */
	inline constexpr std::uint32_t kNoPartitions = 1u << kMaxSkinPartitions;
	/** @brief skinPartitions for a chain (Object LOD's visible ranges, LodSegments): the count of nextPartition links drawn. */
	inline constexpr std::uint32_t kPartitionChain = 1u << 15;
	inline constexpr std::uint32_t kPartitionChainCount = kPartitionChain - 1;
	/**
	 * @brief Blood decals: the slots a decal takes from its ordinal, one per partition up to the last its mask names (a chain's
	 * count; one for 0 or kNoPartitions). BuildDrawsCS writes each every frame.
	 */
	inline constexpr std::uint32_t DecalSpanOf(std::uint32_t a_partitions)
	{
		if (a_partitions & kPartitionChain)
			return std::max(a_partitions & kPartitionChainCount, 1u);
		const std::uint32_t mask = a_partitions & (kNoPartitions - 1);
		return a_partitions == kNoPartitions || !mask ? 1u : static_cast<std::uint32_t>(std::bit_width(mask));
	}
	/** @brief The draws a partition mask gives: one per partition it names, one for 0 (the one geometry), none for kNoPartitions. */
	inline constexpr std::uint32_t PartitionDraws(std::uint32_t a_partitions)
	{
		if (a_partitions & kPartitionChain)
			return a_partitions & kPartitionChainCount;
		return a_partitions == kNoPartitions ? 0u : a_partitions ? static_cast<std::uint32_t>(std::popcount(a_partitions)) : 1u;
	}

	/** @brief Geometry shared by every object that uses the same BSGraphics::TriShape. The buffers are the game's. */
	struct GeometryBuffers
	{
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		std::uint64_t vertexDesc = 0;  // BSGraphics::VertexDesc (stride, attribute offsets, flags)
		std::uint32_t vertexStride = 0;
		std::uint32_t vertexCount = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t firstIndex = 0;
		// The buffers' device addresses (GpuResources), 0 when the render graph is off.
		std::uint64_t vertexAddress = 0;
		std::uint64_t indexAddress = 0;
		std::uint64_t vertexBytes = 0;
		std::uint64_t indexBytes = 0;
		// A skin partition's TriShape: the skin's next partition slot (whatever the frame's mask), else kNoPartition.
		std::uint32_t nextPartition = kNoPartition;
	};

	/**
	 * @brief The object's ancestor value (Api::AncestorValueContribution: Integrations/LightLimitFix's room index, its StrictLightData's
	 * RoomIndex), the record's word 0. -1 (the contribution's none) until a join takes it.
	 */
	struct ObjectAncestorValue
	{
		std::int32_t value = -1;
	};

	/**
	 * @brief Community Shaders' permutation buffer (State::PermutationCB, b4) for a pipeline, as BeginTechnique and the
	 * SetupGeometry hooks leave it. Per-object bits (kObjectSuppressExternalEmittance) go on top of extraShaderDescriptor.
	 */
	struct PipelinePermutation
	{
		std::uint32_t vertexShaderDescriptor = 0;  // descriptor BeginTechnique receives
		std::uint32_t pixelShaderDescriptor = 0;   // received pixel descriptor bits the shader lookup dropped
		std::uint32_t extraShaderDescriptor = 0;
		std::uint32_t extraFeatureDescriptor = 0;
	};

	/** @brief Everything that selects a pipeline: the final shader descriptors and fixed-function state. */
	struct PipelineKey
	{
		std::uint32_t vertexDescriptor = 0;  // after State::ModifyShaderLookup, as the native draw uses
		std::uint32_t pixelDescriptor = 0;
		std::uint32_t rasterFlags = 0;     // PipelineRasterFlags
		std::uint32_t passDescriptor = 0;  // raw technique the Setup* functions read (selects the per-frame constants)
		std::uint64_t vertexLayout = 0;    // geometry's BSGraphics::VertexDesc without the stride (VertexInput.h)

		bool operator==(const PipelineKey&) const = default;
	};
	static_assert(std::has_unique_object_representations_v<PipelineKey>);

	/**
	 * @brief A shadow pipeline: one Utility technique, vertex layout and raster state. Its own key space: a caster's Utility
	 * technique (ShadowViews.h) does not follow its Lighting descriptors.
	 */
	struct ShadowPipelineKey
	{
		std::uint32_t technique = 0;     // Utility technique, mode bits included
		std::uint32_t rasterFlags = 0;   // kRasterTwoSided
		std::uint64_t vertexLayout = 0;  // as PipelineKey::vertexLayout
		std::uint32_t viewState = 0;     // the view's rasterizer state id; 0 for a caster's base key (a key slot)
		std::uint32_t reserved = 0;      // hashed as bytes: no padding

		bool operator==(const ShadowPipelineKey&) const = default;
	};
	static_assert(std::has_unique_object_representations_v<ShadowPipelineKey>);

	struct ShadowPipelineKeyHash
	{
		using is_avalanching = void;
		std::uint64_t operator()(const ShadowPipelineKey& a_key) const noexcept
		{
			return ankerl::unordered_dense::detail::wyhash::hash(&a_key, sizeof(a_key));
		}
	};

	struct PipelineKeyHash
	{
		using is_avalanching = void;
		std::uint64_t operator()(const PipelineKey& a_key) const noexcept
		{
			return ankerl::unordered_dense::detail::wyhash::hash(&a_key, sizeof(a_key));
		}
	};

	/**
	 * @brief Fixed-function state of a pipeline, packed.
	 *
	 * An opaque object sets at most bit 0 (the native deferred pass: depth EQUAL against the Z-prepass, no write, no stencil,
	 * blend or bias, back faces culled unless two-sided). Decal, terrain and forward keys carry the INDICES of the engine's state
	 * objects (RendererShadowState's rasterStateDepthBiasMode, alphaBlendMode, alphaBlendAlphaToCoverage, alphaBlendWriteMode,
	 * alphaBlendModeExtra), which DrawPipelines reads back from the engine's tables (EngineStates.h) rather than re-deriving.
	 */
	enum PipelineRasterFlags : std::uint32_t
	{
		kRasterTwoSided = 1u << 0,
		kRasterDecalGroupShift = 1,  // 2 bits: ObjectDecalGroup
		// A decal's draw (a decal group's but an opaque effect's, a first-person forward decal's): the vertex stage moves it toward the
		// eye (the record's kRecordDecalNudge, LightingConstants.h), in place of the depth bias below, which a device-generated command's
		// pipeline switch does not carry (VK_EXT_device_generated_commands: an execution set's pipelines change shaders only). Not an
		// engine state index: RasterStateBits leaves it out.
		kRasterDecalNudge = 1u << 3,
		// 4 bits: rasterStateDepthBiasMode (0-11). The engine's; its bias is not drawn (kRasterDecalNudge), the index keeps the key the
		// engine's state.
		kRasterDepthBiasShift = 4,
		kRasterBlendModeShift = 8,   // 3 bits: alphaBlendMode (0-6)
		kRasterAlphaToCoverage = 1u << 11,
		kRasterWriteModeShift = 12,  // 4 bits: alphaBlendWriteMode (0-12)
		kRasterBlendExtra = 1u << 16,
		// CS_DCLF_EFFECTS: an Effect shader key (BSEffectShader; Effect.hlsl), descriptors and pass descriptor that shader's
		// (Features/Effects): the Effect program family's key bit (ProgramFamilySlots.h, KeyFamily). Not an engine state index:
		// RasterStateBits leaves it out.
		kRasterEffect = 1u << 17,
		// Lighting keys only, 3 bits: the Lighting key bits' claimant's (Api::LightingKeyBits; Integrations/ExtendedTranslucency's
		// material model XOR DescriptorDisabled, the permutation's ExtraFeatureDescriptor: non-zero only for blended geometry, e.g. NPC
		// hairlines).
		kRasterLightingSpareShift = 21,
		// Effect keys only, same 3 bits: billboard mode + 1 of a leaf under an NiBillboardNode (CS_DCLF_BILLBOARDS), else 0. The
		// vertex stage turns the leaf to its view's camera (DCLFBillboardWorld); the record carries the mode (FamilyRecordFlagsOf). The
		// Effect family's spare key bits (ProgramFamilySlots.h; Extensions::FamilyRules' turnedKeyBits).
		kRasterEffectBillboardShift = kRasterLightingSpareShift,
		// 2 bits, ObjectTerrainGroup. Group 1: blend and write modes 1 and 1, LESS_EQUAL with depth writes; group 2: write mode 1,
		// no blending, EQUAL. No depth bias.
		kRasterTerrainGroupShift = 24,
		// A forward key (kObjectForward): the forward pixel stage (no Deferred), after the deferred composite, onto the main target
		// alone. Blends with the engine's state for its modes (FUN_14150bc80's mode from the alpha property, write mode 11 as
		// FUN_1414b3390 leaves it); depth LESS_EQUAL with kZBufferTest, written with kZBufferWrite (SetupGeometry's derivation).
		kRasterForward = 1u << 26,
		kRasterForwardDepthTest = 1u << 27,
		kRasterForwardDepthWrite = 1u << 28,
		// Refraction (with kRasterForward; LightingDescriptors::refraction). Its Lighting pipeline is never drawn: the refraction
		// range draws Utility RENDER_NORMAL into the refraction normals (RefractionDrawPass), variants _FALLOFF / _CLAMP from these.
		kRasterRefraction = 1u << 29,
		kRasterRefractionFalloff = 1u << 30,
		kRasterRefractionClamp = 1u << 31,
	};
	inline constexpr std::uint32_t kRasterLightingSpareMask = 7u << kRasterLightingSpareShift;

	inline constexpr std::uint32_t RasterDecalGroup(std::uint32_t a_flags) { return (a_flags >> kRasterDecalGroupShift) & 3u; }
	inline constexpr std::uint32_t RasterDepthBiasMode(std::uint32_t a_flags) { return (a_flags >> kRasterDepthBiasShift) & 15u; }
	inline constexpr std::uint32_t RasterBlendMode(std::uint32_t a_flags) { return (a_flags >> kRasterBlendModeShift) & 7u; }
	inline constexpr std::uint32_t RasterWriteMode(std::uint32_t a_flags) { return (a_flags >> kRasterWriteModeShift) & 15u; }
	inline constexpr std::uint32_t RasterTerrainGroup(std::uint32_t a_flags) { return (a_flags >> kRasterTerrainGroupShift) & 3u; }
	/** @brief What selects the engine's state objects: everything but two-sidedness, the Lighting key bits and the shader class. */
	inline constexpr std::uint32_t RasterStateBits(std::uint32_t a_flags) { return a_flags & ~(kRasterTwoSided | kRasterLightingSpareMask | kRasterEffect | kRasterDecalNudge); }
	inline constexpr std::uint32_t RasterLightingSpare(std::uint32_t a_flags) { return (a_flags & kRasterLightingSpareMask) >> kRasterLightingSpareShift; }

	/** @brief Packs a decal's state indices (EngineStates.h says where each comes from). */
	inline constexpr std::uint32_t PackDecalRasterFlags(std::uint32_t a_group, std::uint32_t a_depthBiasMode, std::uint32_t a_blendMode,
		std::uint32_t a_writeMode)
	{
		return ((a_group & 3u) << kRasterDecalGroupShift) | ((a_depthBiasMode & 15u) << kRasterDepthBiasShift) |
		       ((a_blendMode & 7u) << kRasterBlendModeShift) | ((a_writeMode & 15u) << kRasterWriteModeShift);
	}

	/** @brief Packs a terrain group's state: the blend and write modes TerrainBlending::RenderTerrainBlendingPasses sets. */
	inline constexpr std::uint32_t PackTerrainRasterFlags(std::uint32_t a_group, std::uint32_t a_blendMode, std::uint32_t a_writeMode)
	{
		return ((a_group & 3u) << kRasterTerrainGroupShift) | ((a_blendMode & 7u) << kRasterBlendModeShift) | ((a_writeMode & 15u) << kRasterWriteModeShift);
	}

	inline constexpr bool RasterForward(std::uint32_t a_flags) { return (a_flags & kRasterForward) != 0; }
	/** @brief A refraction key: drawn from the refraction range, not the forward one. */
	inline constexpr bool RasterRefraction(std::uint32_t a_flags) { return (a_flags & kRasterRefraction) != 0; }
	/** @brief Packs a refraction key's variant bits, on top of its forward state. */
	inline constexpr std::uint32_t PackRefractionRasterFlags(bool a_falloff, bool a_clamp)
	{
		return kRasterRefraction | (a_falloff ? kRasterRefractionFalloff : 0u) | (a_clamp ? kRasterRefractionClamp : 0u);
	}

	/** @brief Packs a forward key's state: the engine's blend and write modes, and the property's depth flags. */
	inline constexpr std::uint32_t PackForwardRasterFlags(std::uint32_t a_blendMode, std::uint32_t a_writeMode, bool a_depthTest, bool a_depthWrite)
	{
		return kRasterForward | ((a_blendMode & 7u) << kRasterBlendModeShift) | ((a_writeMode & 15u) << kRasterWriteModeShift) |
		       (a_depthTest ? kRasterForwardDepthTest : 0u) | (a_depthWrite ? kRasterForwardDepthWrite : 0u);
	}

	/**
	 * @brief Per-object values of the PerGeometry pixel constants (the rest of that group is per frame). Components the engine
	 * does not write for this object hold kUnwrittenBits.
	 */
	struct ObjectShading
	{
		float materialData[4];  // MaterialData: envmap LOD fade, specular LOD fade, alpha
		float emitColor[3];     // EmitColor: emissive colour * emissive multiplier
		float ssrSpecular;      // SSRParams.w: specular LOD fade (0 when the pass disables it)
	};
	static_assert(sizeof(ObjectShading) == 32);


	/**
	 * @brief The occlusion maps DCLF draws from its tables, each a view of the precipitation accumulator (render mode 0x1C) the
	 * engine would fill by culling the scene lists (Precipitation::SetupMask): Skylighting's sky map and the precipitation mask.
	 * Each has its own pass rule (Skylighting::OcclusionTechnique), technique column, keys, build mode, view slot and depth target.
	 */
	inline constexpr std::uint32_t kOcclusionViews = 2;
	inline constexpr std::uint32_t kOcclusionSky = 0;
	inline constexpr std::uint32_t kOcclusionPrecipitation = 1;


	/**
	 * @brief Fade roots on the GPU (FadeStateCS.hlsl; dclf-architecture.md, "Fades on the GPU"). A member's fade node is a root:
	 * a FadeRootStatic row (CPU, when listed) and a FadeNodeState row (GPU, reseeded from `initial` on a new generation). The pass
	 * runs the node's OnVisible for the main camera whenever its bound is in view (C++ port: Features/Fades/FadeState.h).
	 * FadeNodeState holds BSFadeNode's own fields (skyrim-engine-notes.md, "Fade state").
	 */
	struct FadeNodeState
	{
		std::uint32_t flags = 0;          // +0xF4, only kFadeFlag* (bits 14, 15, 27)
		float currentFade = 1.0f;         // +0x130
		float snapRadius = 0.0f;          // +0x134: the bound radius at the placement snap (FUN_14147aa20)
		std::int32_t lastVisible = 0;     // +0x13C: the fade frame counter it was last in view
		float amountFade = 1.0f;          // +0x140: the last fadeAmount-driven fade
		float metric = 0.0f;              // +0x144: the LOD metric (GetRenderPasses' LOD fades)
		float previousMetric = 0.0f;      // +0x148
		float blend = 0.0f;               // +0x14C: the LOD cross-fade
		std::uint32_t levels = 0;         // +0x152 (levels: current, previous) | +0x153 (LOD type, transition) << 8
		std::uint32_t generation = 0;     // the static row's it was seeded from
		std::uint32_t verdict = 0;        // kFadeVerdict*: what its last update saw and decided
		std::uint32_t frame = 0;          // the scene frame of its last update
	};
	static_assert(sizeof(FadeNodeState) == 48);
	inline constexpr std::uint32_t kFadeFlagFadedIn = 1u << 14;    // the fade's target: faded in
	inline constexpr std::uint32_t kFadeFlagSettled = 1u << 15;    // OnVisible skips the update while fully faded in
	inline constexpr std::uint32_t kFadeFlagLodInUpdate = 1u << 27;  // FUN_14147a160 runs the LOD step first
	inline constexpr std::uint32_t kFadeFlagMask = kFadeFlagFadedIn | kFadeFlagSettled | kFadeFlagLodInUpdate;
	inline constexpr std::uint32_t kFadeVerdictInView = 1u << 0;     // the bound was in the frustum
	inline constexpr std::uint32_t kFadeVerdictAboveLimit = 1u << 1;  // a tree above the height limit: no update
	inline constexpr std::uint32_t kFadeVerdictServiced = 1u << 2;   // OnVisible ran
	inline constexpr std::uint32_t kFadeVerdictDrawn = 1u << 3;      // OnVisible went on into the children
	inline constexpr std::uint32_t kFadeVerdictAnimated = 1u << 4;   // the animation job's update ran first (FadeFrame anim*)

	struct FadeRootStatic
	{
		FadeNodeState initial;            // the node as listed
		float radius = 0.0f;              // +0xF0, the world bound's (the centre is the member record's fade node row)
		float fadeAmount = 1.0f;          // NiAVObject +0x100
		float nearDistance = 0.0f;        // +0x128
		float farDistance = 0.0f;         // +0x12C
		std::uint32_t object = ~0u;       // a member's object slot, whose record's fade node row is the root's centre
		std::uint32_t bits = 0;           // kFadeRoot*, and +0x109 in bits 8-15
		float lodScale = 1.0f;            // FUN_14147a430's distance scale: powf(e44, logf(+0xF0 / e40) * e54) with +0x109 bit 1
		std::uint32_t generation = 0;     // new for every listing (and every input refresh): the state row is seeded again
	};
	static_assert(sizeof(FadeRootStatic) == 80);
	inline constexpr std::uint32_t kFadeRootPlanMask = 0x3u;  // which OnVisible the node has
	inline constexpr std::uint32_t kFadeRootFade = 0;         // BSFadeNode's
	inline constexpr std::uint32_t kFadeRootLeaf = 1;         // BSLeafAnimNode's: the LOD step, then BSFadeNode's
	inline constexpr std::uint32_t kFadeRootTree = 2;         // BSTreeNode's: the height test, BSLeafAnimNode's, the LOD fix-up
	inline constexpr std::uint32_t kFadeRootOther = 3;        // another class's: not serviced
	inline constexpr std::uint32_t kFadeRootBitsShift = 8;    // +0x109
	inline constexpr std::uint32_t kFadeRootTreeLod = 1u << 16;     // a tree whose LOD switch selects past child 0 (+0x180's +0x12C)
	inline constexpr std::uint32_t kFadeRootTreeThresholds = 1u << 17;  // a BSTreeNode: a type-4 node takes the tree LOD thresholds
	// A listed root (every one FadeRoots.cpp lists, admitted by PrimaryCull or not), whose members follow FadeStateCS's state. The
	// engine updates roots DCLF does not list; their members keep the distance test (kObjectFadeTest).
	inline constexpr std::uint32_t kFadeRootOwned = 1u << 18;
	// The node's own flags BSCullingProcess::Process1 (AE 0x140e28390) reads before OnVisible: kAlwaysDraw (bit 11) skips
	// the bound tests; kPreProcessedNode (bit 12) skips them too unless the process ignores preprocessing, and with bit 20
	// set the node is then not visited at all.
	inline constexpr std::uint32_t kFadeRootAlwaysDraw = 1u << 20;
	inline constexpr std::uint32_t kFadeRootPreprocessed = 1u << 21;
	inline constexpr std::uint32_t kFadeRootPreprocessHidden = 1u << 22;
	// An owned root with no engine-drawn part, culled by the stand-in: its state is FadeStateCS's alone and its node takes the
	// GPU's milestones and LOD levels from the fade write-back (FadeEvent; the animation gate skips it,
	// Features/Fades' FadeWrittenByDclf). DCLF's readers take the GPU's state (BuildDrawsCS) or the settled one (ShadowCasterReject,
	// the synthetic pass). An owned root with engine-drawn parts is culled by the engine; FadeStateCS runs the same update for its
	// DCLF members (CS_DCLF_FADE_PARITY compares).
	inline constexpr std::uint32_t kFadeRootStoodIn = 1u << 19;
	inline constexpr std::uint32_t kNoFadeRoot = ~0u;
	// Which of the slot's two seed rows (FrameValues' fade seeds, 2 * slot + this bit) holds its generation's. Every listing
	// and reseed toggles it, so a new seed never overwrites the row the installed tables' generation still reads.
	inline constexpr std::uint32_t kFadeRootSeedOdd = 1u << 23;
	// The node is DCLF's alone to write (the write-back stores FadeStateCS's milestones at Main::Update's entry; neither OnVisible
	// nor the animation gate steps it). Every listed root; Features/Fades' FadeWrittenByDclf reads it from the accepted tables.
	inline constexpr std::uint32_t kFadeRootWritten = 1u << 24;
	// The bits the seed holds (the node's, as FrameValues samples it in the frame) and those the coordinator's row holds (its inputs).
	inline constexpr std::uint32_t kFadeRootNodeBits = kFadeRootPlanMask | (0xFFu << kFadeRootBitsShift) | kFadeRootTreeThresholds | kFadeRootAlwaysDraw |
	                                                   kFadeRootPreprocessed | kFadeRootPreprocessHidden;
	inline constexpr std::uint32_t kFadeRootCoordinatorBits = kFadeRootTreeLod | kFadeRootOwned | kFadeRootStoodIn | kFadeRootSeedOdd | kFadeRootWritten;
	/** @brief A root slot's seed row in FrameValues' fade seeds (kFadeRootSeedOdd). */
	inline constexpr std::size_t FadeSeedRow(std::uint32_t a_slot, std::uint32_t a_bits) { return 2ull * a_slot + ((a_bits & kFadeRootSeedOdd) ? 1u : 0u); }
	/**
	 * @brief The row FadeStateCS updates with: the seed FrameValues sampled, with the coordinator's object, generation, fadeAmount
	 * and bits. False while no seed of the row's generation is there (must match FadeStateCS.hlsl's MergeSeed).
	 */
	inline bool MergeFadeSeed(const FadeRootStatic& a_root, const FadeRootStatic* a_seeds, std::size_t a_seedCount, std::uint32_t a_slot, FadeRootStatic& a_out)
	{
		const std::size_t at = FadeSeedRow(a_slot, a_root.bits);
		if (!a_seeds || at >= a_seedCount || a_seeds[at].generation != a_root.generation)
			return false;
		// a_out may be a_root.
		FadeRootStatic merged = a_seeds[at];
		merged.object = a_root.object;
		merged.generation = a_root.generation;
		merged.fadeAmount = a_root.fadeAmount;
		merged.bits = (a_seeds[at].bits & kFadeRootNodeBits) | (a_root.bits & kFadeRootCoordinatorBits);
		a_out = merged;
		return true;
	}

	/** @brief The frame's inputs to the fade update: the main camera and the engine's fade globals (AE addresses). */
	struct FadeFrame
	{
		float eye[3]{};                   // the main camera's world position
		float lodAdjust = 0.0f;           // its +0x184; 0: no camera this frame, nothing is updated
		std::int32_t counter = 0;         // the fade frame counter (0x142032e50)
		float deltaTime = 0.0f;           // 0x142033084
		std::uint32_t fadesOn = 0;        // 0x142032dfd: OnVisible updates fades
		std::uint32_t lodUpdates = 0;     // 0x142032dfc
		float fadeInTime = 0.0f;          // 0x142032e2c
		float fadeOutTime = 0.0f;         // 0x142032e30
		float fadeInAbove = 0.0f;         // 0x142032e34
		float fadeOutBelow = 0.0f;        // 0x142032e38
		float blendTime = 0.0f;           // 0x142032e3c
		float distanceMult = 0.0f;        // 0x142032e48
		float stepMax = 0.0f;             // 0x142032e4c
		float amountTime = 0.0f;          // 0x14332a214
		float metricScale = 0.0f;         // 0x141aa6300
		float defaultScale = 0.0f;        // 0x141ad2840
		float metricOverride = 0.0f;      // 0x14332a254
		std::uint32_t overridden = 0;     // 0x14332a254 != 0x141769578 (also selects OnVisible's type-6 branch when clear)
		float lodFar = 0.0f;              // 0x14332a22c
		float lodNear = 0.0f;             // 0x14332a25c
		float treeLodFar = 0.0f;          // 0x14332a238
		float treeLodNear = 0.0f;         // 0x14332a268
		float lodMinimum = 0.0f;          // 0x1433dcfa8
		float one = 1.0f;                 // 0x141ad2870
		float amountSnapAbove = 0.0f;     // 0x141ad2874
		float amountSnapOffset = 0.0f;    // 0x141ad288c
		float snapRadiusLimit = 0.0f;     // 0x141ad29f4
		float treeHeightBase = 0.0f;      // BSTreeNode::OnVisible's height test: 0x14332a2f0
		float treeHeightLimit = 0.0f;     // 0x142032fa0, +infinity while the test is off
		std::uint32_t padding = 0;
		float divisors[16]{};             // 0x142032e00, per LOD type
		// Per-frame values (the invocation is prepared ahead of the commit): root slots, scene frame, parity log's first root.
		std::uint32_t rootCount = 0, sceneFrame = 0, logBase = ~0u, reserved = 0;
		// The animation job's fade update (FUN_1402cff60 -> FUN_14147a160) on a root its last cull missed: its inputs, for the roots
		// the animated buffer stamps with this frame.
		float animEye[3]{};
		float animLodAdjust = 0.0f;
		std::int32_t animCounter = 0;
		float animDeltaTime = 0.0f;
		std::uint32_t animPadding[2]{};
	};
	// The animated buffer's word per root: the scene frame (low 28 bits) of the batch that updated it, and how many updates that
	// batch made (the job can update a node more than once between two culls).
	inline constexpr std::uint32_t kFadeAnimatedCountBits = 4;
	inline constexpr std::uint32_t kFadeAnimatedCountMask = (1u << kFadeAnimatedCountBits) - 1;
	inline constexpr std::uint32_t FadeAnimatedWord(std::uint32_t a_frame, std::uint32_t a_count)
	{
		return (a_frame << kFadeAnimatedCountBits) | (a_count < kFadeAnimatedCountMask ? a_count : kFadeAnimatedCountMask);
	}
	static_assert(sizeof(FadeFrame) == 240);

	/**
	 * @brief One indirect draw, in the command signature's argument order (BasicRHI packs arguments like D3D12, 4-byte aligned):
	 * pipeline set index, push data (words 0-12: pipeline row and material row addresses, object word, two vertex buffer views in
	 * D3D12's VBV layout), index buffer view (D3D12's IBV layout), DrawIndexed.
	 *
	 * No DCLF pipeline has input assembly: the vertex stages pull their attributes through the vertex buffer views, the device-generated
	 * draws' from their push data (DrawPipelines.h, kDrawPushWords; DCLF_PULLED_VERTICES), the plain draws' from the sequence itself
	 * (DCLF_PULLED). No draw reads the views' sizes. Slot 1 is a dynamic shape's positions (Features/Faces' snapshots; where
	 * the engine binds BSDynamicTriShape::dynamicData); other draws repeat slot 0. The tables hold a template with the pipeline's table
	 * index; the epoch substitutes the set index and rows.
	 */
#pragma pack(push, 4)
	struct DrawSequence
	{
		std::uint32_t pipelineIndex;
		// The draw's rows (the pipeline row's registers and the material row) and the object's index in SceneTables::objects, then
		// the two vertex buffer views: thirteen words, one contiguous Constant indirect argument (the per-draw push data, whose
		// packoffsets are DCLFObjects.hlsli's DCLFPushData): keep them together, in order.
		std::uint64_t pipelineRowAddress;
		std::uint64_t materialRowAddress;
		std::uint32_t objectIndex;
		std::uint64_t vertexBufferAddress;  // GeometryBuffers::vertexAddress (0 without the render graph)
		std::uint32_t vertexBufferSize;  // written; no draw reads it
		std::uint32_t vertexStride;
		std::uint64_t streamBufferAddress;  // slot 1: a face shape's positions, else the slot 0 view again
		std::uint32_t streamBufferSize;  // written; no draw reads it
		std::uint32_t streamStride;
		std::uint64_t indexBufferAddress;  // GeometryBuffers::indexAddress
		std::uint32_t indexBufferSize;
		std::uint32_t indexFormat;  // DXGI_FORMAT_R16_UINT
		std::uint32_t indexCount;
		std::uint32_t instanceCount;
		std::uint32_t firstIndex;
		std::int32_t vertexOffset;
		std::uint32_t firstInstance;
	};
#pragma pack(pop)
	static_assert(sizeof(DrawSequence) == 92);
	static_assert(offsetof(DrawSequence, pipelineRowAddress) == 4);
	static_assert(offsetof(DrawSequence, materialRowAddress) == 12);
	static_assert(offsetof(DrawSequence, objectIndex) == 20);
	static_assert(offsetof(DrawSequence, vertexBufferAddress) == 24);
	static_assert(offsetof(DrawSequence, streamBufferAddress) == 40);
	static_assert(offsetof(DrawSequence, indexBufferAddress) == 56);
	static_assert(offsetof(DrawSequence, indexCount) == 72);
}
