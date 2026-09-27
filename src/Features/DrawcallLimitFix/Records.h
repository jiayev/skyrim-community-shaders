#pragma once

#include <cstdint>
#include <type_traits>

struct ID3D11Buffer;

namespace DCLF
{
	/**
	 * @brief Per-object record. This is the GPU layout of the object table (16-byte aligned,
	 * no implicit padding), so it can be uploaded as is.
	 */
	struct ObjectRecord
	{
		float world[12];          // row-major 3x4, as Lighting.hlsl's World (translation in column 3)
		float previousWorld[12];  // same layout, from NiAVObject::previousWorld
		float boundCenter[3];     // world-space bounding sphere
		float boundRadius;
		std::uint32_t geometryIndex;
		std::uint32_t materialIndex;
		std::uint32_t pipelineIndex;
		std::uint32_t flags;  // ObjectFlags, alpha-test threshold in bits 8-15
	};
	static_assert(sizeof(ObjectRecord) == 128);
	static_assert(offsetof(ObjectRecord, boundCenter) == 96);
	static_assert(offsetof(ObjectRecord, geometryIndex) == 112);

	enum ObjectFlags : std::uint32_t
	{
		kObjectAlphaTest = 1u << 0,  // alpha test on, reference = threshold / 255 (as the native draw's AlphaTestRef)
		kObjectTwoSided = 1u << 1,   // no culling (the native main pass culls back faces otherwise)
		kObjectSuppressExternalEmittance = 1u << 2,  // ExtraShaderDescriptors::SuppressExternalEmittance in the permutation buffer
		// The engine's main-camera accumulator holds this object this frame, so its own culling (frustum,
		// occlusion planes, rooms and portals) kept it. The tables carry the whole tracked set so that the
		// GPU culling has a real input to reject from; this bit is what tells the two apart, and it is the
		// reference the culling is measured against (BuildDrawsCS: RequireNativeVisible).
		kObjectNativeVisible = 1u << 3,
		// A skinned object (CS_DCLF_SKINNED): its vertices come from the skin partition's own buffer and its
		// vertex shader reads the bone palette rows at BindlessObject::boneOffset / previousBoneOffset.
		kObjectSkinned = 1u << 4,
		// The object is a culling candidate only: it cannot be drawn this frame, so no material or
		// pipeline entry was built for it and its materialIndex and pipelineIndex mean nothing. Anything
		// that indexes the tables with them must check this first - the tables can be empty entirely (the
		// first frame after a teleport has tracked geometry but nothing accumulated yet), so even index 0
		// is not safe.
		kObjectNoBindings = 1u << 5,
		// Technique 12. Only a tree's TreeParams and WindTimers are its own; for everything else those
		// two variables belong to the pipeline template, and writing a zeroed per-object copy over them
		// clobbers values the native draw does use - which is what the first attempt at trees did, to
		// every object in the frame rather than only to trees.
		kObjectTreeAnim = 1u << 6,
		// A decal (CS_DCLF_DECALS). Never an occluder: it is left out of the depth segment entirely and
		// drawn by the colour segment's second pass, after the opaque draws, in the engine's group order.
		// The group is in bits 20-21 (kObjectDecalGroupShift): 1 = accumulation hint 2 (the engine's
		// opaque decal group, drawn first), 2 = hint 3 (the blended one, drawn second).
		kObjectDecal = 1u << 7,
		kObjectAlphaThresholdShift = 8,
		// Per-object PerGeometry values beyond World and the tree pair, kept in the epoch's row buffer at
		// BindlessObject::extraOffset (Records: kExtraRows rows, layout in SceneStore::RefreshFrameConstants):
		// the ProjectedUV texture matrix and pixel parameters (CS_DCLF_PROJECTED_UV), and the landscape
		// blend parameters of the MTLand techniques (CS_DCLF_MTLAND).
		kObjectProjectedUV = 1u << 18,
		kObjectLandBlend = 1u << 19,
		kObjectDecalGroupShift = 20,
		// The engine would draw no shadow-map pass for this object (ShadowViews.h: ShadowCasterReject),
		// so no shadow view's culling may accept it. Decided by the scene phase, which is before any
		// shadow view is drawn.
		kObjectNoShadow = 1u << 22,
		// A volumetric-only caster (ShadowReject::VolumetricOnly): the engine draws it into the sun's volumetric
		// lighting copy and nowhere else (batch group 15, accumulation hint 8). A shadow view of the copy draws
		// only these inputs, and every other shadow view skips them (BuildDrawsLatch::cullFlags). Set by the
		// scene phase, and only when PassCapture can withhold the copy's passes (VolumetricClaimsAvailable).
		kObjectVolumetricOnly = 1u << 23,
		// A shadow-only object: the main pass cannot take it (its Ineligible reason is one the shadow views do
		// not care about, SceneStore::ShadowOnlyReason), but it is a shadow caster, so it has a record for the
		// shadow epochs alone. It always has kObjectNoBindings; the accumulate phase and the main epochs skip it.
		kObjectShadowOnly = 1u << 24,
		// A free object slot (SceneStore's persistent slots): no object at all. It always has kObjectNoBindings and
		// kObjectNoShadow too, so every loop that skips those skips it; the main build skips it before counting.
		kObjectFree = 1u << 25,
		// A synthetic main pass (PrimaryCull) whose pipeline carries the sun's bits (DefShadow, ShadowDir) because
		// the object may take the sun's shadow: the colour epoch's BuildDraws tests its bound against this frame's
		// cascades and, on a miss, marks the draw (kObjectSunMiss) so the pixel stage drops the two bits, as
		// GetRenderPasses would have left them off. Set per frame by the accumulate phase.
		kObjectSunTest = 1u << 26,
		// A resident object under a fade root (PrimaryCull, dclf-cull-job-elimination.md "Phase 4 in detail"): the depth
		// segment's first phase tests its entry root's distance against the fade-out distance (Tables::fadeDistance,
		// Tables::sunEntry) and drops it, as a final verdict, where BSFadeNode::OnVisible would snap its fade to 0: past
		// the distance and not in view last frame. Set by the accumulate phase with the resident patch.
		kObjectFadeTest = 1u << 27,
		// A resident tree (PrimaryCull): the depth segment's first phase drops it, as a final verdict, where
		// BSTreeNode::OnVisible's height test would (its entry root's centre above the frame's limit).
		kObjectHeightTest = 1u << 28,
	};

	/** @brief The high bit of a draw's object-index word: the draw misses every sun cascade (kObjectSunTest). */
	inline constexpr std::uint32_t kObjectSunMiss = 1u << 31;

	/**
	 * @brief The shadow epochs' face positions buffer (FaceSnapshots): one float4 per vertex of every face shape
	 * drawn, each shape in a region SceneStore keeps for it while it is walked. Bound as a face draw's second
	 * vertex stream, which is where the engine's own draw puts BSDynamicTriShape::dynamicData.
	 */
	inline constexpr std::uint32_t kFacePositionVertices = 1u << 20;
	inline constexpr std::uint32_t kNoFaceRegion = ~0u;
	inline constexpr std::uint32_t kNoFaceStream = ~0u;

	/** @brief Rows of per-object extras in the row buffer: LandBlendParams, TextureProj x3, ProjectedUVParams x3. */
	inline constexpr std::uint32_t kExtraRows = 7;
	inline constexpr std::uint32_t kExtraRowLandBlend = 0;
	inline constexpr std::uint32_t kExtraRowTextureProj = 1;
	inline constexpr std::uint32_t kExtraRowProjectedParams = 4;
	inline constexpr std::uint32_t kNoExtraRows = ~0u;

	/** @brief ObjectFlags -> decal group (0 for anything that is not a decal). */
	inline constexpr std::uint32_t ObjectDecalGroup(std::uint32_t a_flags) { return (a_flags & kObjectDecal) ? (a_flags >> kObjectDecalGroupShift) & 3u : 0u; }

	/**
	 * @brief Geometry shared by every object that uses the same BSGraphics::TriShape.
	 * The buffers are the game's.
	 */
	/** @brief GeometryRecord::nextPartition: the last partition, or a TriShape that is not one. */
	inline constexpr std::uint32_t kNoPartition = ~0u;
	/** @brief The most partitions a skin DCLF draws may have: one bit each in the draw's partition mask. */
	inline constexpr std::uint32_t kMaxSkinPartitions = 8;

	struct GeometryRecord
	{
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		std::uint64_t vertexDesc = 0;  // BSGraphics::VertexDesc (stride, attribute offsets, flags)
		std::uint32_t vertexStride = 0;
		std::uint32_t vertexCount = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t firstIndex = 0;
		// Phase 2: the buffers' device addresses (GpuResources), 0 when the render graph is off.
		std::uint64_t vertexAddress = 0;
		std::uint64_t indexAddress = 0;
		std::uint64_t vertexBytes = 0;
		std::uint64_t indexBytes = 0;
		// For a skin partition's TriShape: the slot of the same skin's next partition, rewritten every frame the
		// skin is drawn (SceneStore::LinkPartitions), else kNoPartition. BuildDrawsCS walks it.
		std::uint32_t nextPartition = kNoPartition;
	};

	/**
	 * @brief Per-object light assignment Light Limit Fix passes per draw (StrictLightData, PS b3). In the
	 * main pass there are no strict lights (NumStrictLights is 0); what varies is the object's room in
	 * interiors and the shadow mask channels of its shadow-casting lights.
	 */
	struct ObjectLights
	{
		std::int32_t roomIndex = -1;
		std::uint32_t shadowBitMask = 0;
	};

	/**
	 * @brief Community Shaders' permutation buffer (State::PermutationCB, b4) for a pipeline, as
	 * BeginTechnique and the SetupGeometry hooks leave it for the draw. Per-object bits
	 * (kObjectSuppressExternalEmittance) are added on top of extraShaderDescriptor.
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
	 * @brief A pipeline of the shadow views: one Utility technique, one vertex layout, one raster state.
	 *
	 * Its own key space, not a variant of PipelineKey: a shadow draw runs the Utility shader with the
	 * technique the engine derives for the caster (ShadowViews.h), which does not follow the Lighting
	 * descriptors the main pass's key is built from - two objects sharing a Lighting pipeline can cast
	 * with different Utility techniques, and one Utility technique serves objects of many Lighting
	 * pipelines. The raster flags carry two-sidedness and, for a pipeline (not a caster's base key), the
	 * view's rasterizer state at kRasterShadowStateShift (DrawPipelines::ShadowRasterStateId).
	 */
	struct ShadowPipelineKey
	{
		std::uint32_t technique = 0;     // Utility technique, mode bits included
		std::uint32_t rasterFlags = 0;   // kRasterTwoSided, and the view's rasterizer state id at kRasterShadowStateShift
		std::uint64_t vertexLayout = 0;  // as PipelineKey::vertexLayout

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
	 * For an opaque object only bit 0 is ever set: the native main (deferred) pass draws it with depth
	 * test EQUAL against the Z-prepass (no write), stencil, blending and depth bias off, back-face
	 * culling unless two-sided, and none of that needs stating.
	 *
	 * A decal is drawn by the engine with a depth bias and, in its blended group, with the geometry's
	 * alpha property applied, so its key also carries the INDICES of the engine's own state objects -
	 * RendererShadowState's rasterStateDepthBiasMode, alphaBlendMode, alphaBlendAlphaToCoverage,
	 * alphaBlendWriteMode and alphaBlendModeExtra - which DrawPipelines reads back from the engine's
	 * state tables (EngineStates.h) rather than re-deriving what each index means. Zero for everything
	 * that is not a decal, so every existing key is unchanged.
	 */
	enum PipelineRasterFlags : std::uint32_t
	{
		kRasterTwoSided = 1u << 0,
		kRasterDecalGroupShift = 1,  // 2 bits: ObjectDecalGroup
		kRasterDepthBiasShift = 4,   // 4 bits: rasterStateDepthBiasMode (0-11)
		kRasterBlendModeShift = 8,   // 3 bits: alphaBlendMode (0-6)
		kRasterAlphaToCoverage = 1u << 11,
		kRasterWriteModeShift = 12,  // 4 bits: alphaBlendWriteMode (0-12)
		kRasterBlendExtra = 1u << 16,
		// Shadow keys only: 4 bits, the view's rasterizer state (DrawPipelines::ShadowRasterStateId, 1-15).
		kRasterShadowStateShift = 17,
		// Main keys only: 3 bits, Extended Translucency's material model for the draw XOR DescriptorDisabled (so an
		// opaque key, whose model is disabled, keeps 0 here). ExtendedTranslucency::MaterialModelOf sets it per
		// geometry in the feature's SetupGeometry hook; it differs from disabled only for blended geometry (blended
		// decals, such as NPC hairlines and beards), and it is the permutation's ExtraFeatureDescriptor.
		kRasterTranslucencyShift = 21,
	};
	inline constexpr std::uint32_t kRasterTranslucencyMask = 7u << kRasterTranslucencyShift;

	inline constexpr std::uint32_t RasterDecalGroup(std::uint32_t a_flags) { return (a_flags >> kRasterDecalGroupShift) & 3u; }
	inline constexpr std::uint32_t RasterDepthBiasMode(std::uint32_t a_flags) { return (a_flags >> kRasterDepthBiasShift) & 15u; }
	inline constexpr std::uint32_t RasterBlendMode(std::uint32_t a_flags) { return (a_flags >> kRasterBlendModeShift) & 7u; }
	inline constexpr std::uint32_t RasterWriteMode(std::uint32_t a_flags) { return (a_flags >> kRasterWriteModeShift) & 15u; }
	inline constexpr std::uint32_t RasterShadowState(std::uint32_t a_flags) { return (a_flags >> kRasterShadowStateShift) & 15u; }
	inline constexpr std::uint32_t WithShadowState(std::uint32_t a_flags, std::uint32_t a_state)
	{
		return (a_flags & ~(15u << kRasterShadowStateShift)) | ((a_state & 15u) << kRasterShadowStateShift);
	}
	/** @brief Everything but the two-sided bit and the translucency model: what selects the engine's state objects. */
	inline constexpr std::uint32_t RasterStateBits(std::uint32_t a_flags) { return a_flags & ~(kRasterTwoSided | kRasterTranslucencyMask); }
	inline constexpr std::uint32_t RasterTranslucency(std::uint32_t a_flags) { return (a_flags & kRasterTranslucencyMask) >> kRasterTranslucencyShift; }

	/** @brief Packs a decal's state indices (EngineStates.h says where each comes from). */
	inline constexpr std::uint32_t PackDecalRasterFlags(std::uint32_t a_group, std::uint32_t a_depthBiasMode, std::uint32_t a_blendMode,
		std::uint32_t a_writeMode)
	{
		return ((a_group & 3u) << kRasterDecalGroupShift) | ((a_depthBiasMode & 15u) << kRasterDepthBiasShift) |
		       ((a_blendMode & 7u) << kRasterBlendModeShift) | ((a_writeMode & 15u) << kRasterWriteModeShift);
	}

	/**
	 * @brief Per-object values of the PerGeometry pixel constants (everything else in that group is
	 * per frame). Components the engine does not write for this object hold kUnwrittenBits.
	 */
	struct ObjectShading
	{
		float materialData[4];  // MaterialData: envmap LOD fade, specular LOD fade, alpha
		float emitColor[3];     // EmitColor: emissive colour * emissive multiplier
		float ssrSpecular;      // SSRParams.w: specular LOD fade (0 when the pass disables it)
	};
	static_assert(sizeof(ObjectShading) == 32);

	/**
	 * @brief The two PerGeometry variables tree animation displaces its vertices with (technique 12).
	 *
	 * Per object, not per pipeline. Every other PerGeometry value DCLF treats as a property of the
	 * pipeline, taken from one template object, and for trees that is wrong twice over: TreeParams
	 * carries the individual tree's amplitude and leaf frequency, and WindTimers is a per-tree clock
	 * the engine advances as it sets the draw up. Left per-pipeline, one template tree's wind is handed
	 * to every tree sharing its pipeline - capture parity counted 47,851 and 83,390 mismatched draws.
	 *
	 * windTimers uses only xy; it is a float4 so the struct keeps 16-byte alignment on both sides of
	 * the GPU record, where a float2 would let HLSL pad and the two layouts disagree.
	 */
	struct ObjectTreeAnim
	{
		float treeParams[4]{};   // 0, wind magnitude, amplitude, leaf frequency
		float windTimers[4]{};   // wind timer, previous wind timer, unused, unused
	};
	static_assert(sizeof(ObjectTreeAnim) == 32);

	/**
	 * @brief One indirect draw, in the argument order of the command signature (BasicRHI packs arguments
	 * like D3D12, 4-byte aligned): pipeline set index, push data (the DrawBindings record's address),
	 * two vertex buffer views, index buffer view (D3D12 VBV / IBV layouts), DrawIndexed.
	 *
	 * The second view is slot 1, where a dynamic shape's positions are (FaceSnapshots; the engine's own draw
	 * binds BSDynamicTriShape::dynamicData there). Every other draw repeats its slot 0 view, which a layout
	 * without a second stream never reads.
	 *
	 * The scene tables hold the geometry part with the pipeline's table index; the main-pass epoch
	 * replaces it with the pipeline set index and fills in the record address.
	 */
#pragma pack(push, 4)
	struct DrawSequence
	{
		std::uint32_t pipelineIndex;
		std::uint64_t bindingsAddress;
		// The object's index in SceneStore::Tables::objects, pushed as the third root constant word so the
		// shaders can reach per-object data without it travelling in the binding record. The three words
		// are one contiguous Constant indirect argument, so this must stay adjacent to bindingsAddress.
		std::uint32_t objectIndex;
		std::uint64_t vertexBufferAddress;  // GeometryRecord::vertexAddress (0 without the render graph)
		std::uint32_t vertexBufferSize;
		std::uint32_t vertexStride;
		std::uint64_t streamBufferAddress;  // slot 1: a face shape's positions, else the slot 0 view again
		std::uint32_t streamBufferSize;
		std::uint32_t streamStride;
		std::uint64_t indexBufferAddress;  // GeometryRecord::indexAddress
		std::uint32_t indexBufferSize;
		std::uint32_t indexFormat;  // DXGI_FORMAT_R16_UINT
		std::uint32_t indexCount;
		std::uint32_t instanceCount;
		std::uint32_t firstIndex;
		std::int32_t vertexOffset;
		std::uint32_t firstInstance;
	};
#pragma pack(pop)
	static_assert(sizeof(DrawSequence) == 84);
	static_assert(offsetof(DrawSequence, bindingsAddress) == 4);
	static_assert(offsetof(DrawSequence, objectIndex) == 12);
	static_assert(offsetof(DrawSequence, vertexBufferAddress) == 16);
	static_assert(offsetof(DrawSequence, streamBufferAddress) == 32);
	static_assert(offsetof(DrawSequence, indexBufferAddress) == 48);
	static_assert(offsetof(DrawSequence, indexCount) == 64);
}
