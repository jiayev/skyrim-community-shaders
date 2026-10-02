#pragma once

#include <bit>
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
		// Bit 3 is free.
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
		// opaque decal group, geometry group 3, drawn first), 3 = a multi-index shape's layer (hint 12, geometry
		// group 2, drawn second), 2 = hint 3 (the blended one, geometry group 4, drawn last).
		// kDecalDrawOrder has the order of their ranges.
		kObjectDecal = 1u << 7,
		kObjectAlphaThresholdShift = 8,
		// Per-object PerGeometry values beyond World and the tree pair, kept in the epoch's row buffer at
		// BindlessObject::extraOffset (Records: kExtraRows rows, layout in SceneStore::RefreshFrameConstants):
		// the ProjectedUV texture matrix and pixel parameters (CS_DCLF_PROJECTED_UV), and the landscape
		// blend parameters of the MTLand techniques (CS_DCLF_MTLAND).
		// Bit 17 is free: an owned fade root's faded-out state is the GPU's (FadeStateCS, kFadeRootOwned).
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
		// scene phase.
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
		// A landscape property (kLandscape, kNoLODLandFade): the main pass's light selection gives it only the local shadow
		// lights that take landscape (BSShadowLight +0x61; LocalShadowLights).
		kObjectLandscapeLights = 1u << 29,
		// Bound by scene membership (SceneStore::BindByMembership): drawn whenever the GPU finds it. Every record with bindings
		// is a member; the rest are culling candidates (kObjectNoBindings).
		kObjectMember = 1u << 30,
	};

	/** @brief The high bit of a draw's object-index word: the draw misses every sun cascade (kObjectSunTest). */
	inline constexpr std::uint32_t kObjectSunMiss = 1u << 31;
	// The object word's local shadow lights (BuildDrawsCS, LocalShadowLights): Light Limit Fix's ShadowBitMask of the draw, whose
	// lights are shadow mask channels 0-3. The object index is below them.
	inline constexpr std::uint32_t kObjectLocalShadowShift = 26;
	inline constexpr std::uint32_t kObjectLocalShadowMask = 0xFu << kObjectLocalShadowShift;
	inline constexpr std::uint32_t kObjectIndexMask = (1u << kObjectLocalShadowShift) - 1;

	/**
	 * @brief The face positions buffer (FaceSnapshots) holds one float4 per vertex of every face shape drawn, each shape in a
	 * region SceneStore keeps for it while it is walked, and is bound as a face draw's second vertex stream, which is where the
	 * engine's own draw puts BSDynamicTriShape::dynamicData. A shape without a region yet has kNoFaceRegion.
	 */
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

	/** @brief GeometryRecord::nextPartition: the last partition, or a TriShape that is not one. */
	inline constexpr std::uint32_t kNoPartition = ~0u;
	/** @brief The most partitions a skin DCLF draws may have: one bit each in the draw's partition mask. */
	inline constexpr std::uint32_t kMaxSkinPartitions = 8;
	/**
	 * @brief Tables::skinPartitions for a skin whose fade node's LOD level draws none of its partitions (SkinPartitionMask 0:
	 * the engine draws nothing of it at that level). The object stays a member with its bindings; BuildDraws writes no draw.
	 */
	inline constexpr std::uint32_t kNoPartitions = 1u << kMaxSkinPartitions;
	/** @brief The draws a partition mask gives: one per partition it names, one for 0 (the one geometry), none for kNoPartitions. */
	inline constexpr std::uint32_t PartitionDraws(std::uint32_t a_partitions)
	{
		return a_partitions == kNoPartitions ? 0u : a_partitions ? static_cast<std::uint32_t>(std::popcount(a_partitions)) : 1u;
	}

	/**
	 * @brief Geometry shared by every object that uses the same BSGraphics::TriShape.
	 * The buffers are the game's.
	 */
	struct GeometryRecord
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
	 * pipelines. The raster flags carry two-sidedness; a pipeline (not a caster's base key, whose viewState is 0) is for one
	 * view rasterizer state (DrawPipelines::ShadowRasterStateId, 1 and up).
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
		float windTimers[4]{};   // wind timer, previous wind timer, then in the GPU record its tree slot and generation (bits)
	};
	static_assert(sizeof(ObjectTreeAnim) == 32);

	/**
	 * @brief A tree's wind clock on the GPU (TreeWindCS.hlsl; drawcall-limit-fix.md, "Tree wind on the GPU").
	 *
	 * The engine's tree manager (FUN_1404381e0, from Main::Update) advances a BSTreeNode's timer (+0x164) by the frame's
	 * seconds, measures its squared distance to the camera (+0x158), and within its range (manager +0x80) takes the gust
	 * amplitude (+0x15C) from the timer: 0.25 * sum over k in {1,3,5,7} of sin(k * pi * manager+0x78 * timer), times the
	 * model's scale (model +0xB0). SetupGeometry then reads them and keeps the timer as the previous one (+0x168). DCLF's
	 * members take all of it from TreeWindCS: one TreeStatic row per tree node a member draws (the CPU's, written when the
	 * node is listed), one TreeClock row per node (the GPU's, initialised from the static row's values when its generation
	 * is new), and one TreeObject per member drawing under a node, whose record's TreeParams and WindTimers the pass writes.
	 */
	struct TreeStatic
	{
		float position[3]{};       // the node's world translate (+0xA0)
		float leafFrequency = 1.0f;  // +0x160
		float modelAmplitude = 0.0f; // the model's +0xB0
		float timer = 0.0f;          // the node's clock when listed: +0x164, +0x168, +0x15C
		float previousTimer = 0.0f;
		float amplitude = 1.0f;
		std::uint32_t generation = 0;  // new for every listing: the clock row takes the values above again
		std::uint32_t animated = 0;    // a model is attached (the manager advances only such a node)
		std::uint32_t padding[2]{};
	};
	static_assert(sizeof(TreeStatic) == 48);

	struct TreeClock
	{
		float timer, previousTimer, amplitude;
		std::uint32_t generation, frame;
		std::uint32_t padding[3];
	};
	static_assert(sizeof(TreeClock) == 32);

	// A member drawing under a tree node: its object slot and the node's tree slot (kNodelessTree: the engine's defaults).
	struct TreeObject
	{
		std::uint32_t object, tree;
	};
	inline constexpr std::uint32_t kNoTree = ~0u;
	inline constexpr std::uint32_t kNodelessTree = ~0u - 1;

	/** @brief The frame's inputs to the tree clocks (TreeWindCS.hlsl's constants after its indices). */
	struct TreeWindFrame
	{
		float deltaTime = 0.0f;  // the frame's seconds (GetSecondsPassed's global)
		float camera[3]{};       // the main camera's world position
		float windSpeed = 0.0f;  // the tree manager's +0x78
		float maxDistance2 = 0.0f;  // its +0x80
		float windMagnitude = 0.0f;
		float fadeStart = 0.0f, fadeEnd = 0.0f;
		float timerScale = 0.0f;
	};

	/**
	 * @brief The occlusion maps DCLF draws from its own tables, each a view of the precipitation accumulator (render mode
	 * 0x1C) the engine would otherwise fill by culling and registering the scene lists (Precipitation::SetupMask):
	 * Skylighting's sky occlusion map, and the precipitation occlusion mask. Each has its own pass rule
	 * (Skylighting::OcclusionTechnique), technique column, pipeline keys, shadow-build mode, view slot and depth target.
	 */
	inline constexpr std::uint32_t kOcclusionViews = 2;
	inline constexpr std::uint32_t kOcclusionSky = 0;
	inline constexpr std::uint32_t kOcclusionPrecipitation = 1;

	/**
	 * @brief The main camera's cull test, as FadeStateCS repeats it for each root (the list processes' Process1, AE
	 * 0x140e28390): the process's cull mode and flags, and its compound frustum (BSCompoundFrustum, the portal graph's portals
	 * and occlusion planes), which Process1 evaluates (BSCompoundFrustum::Process, 0x140e320b0) before it runs the node's
	 * OnVisible. The compound frustum exists only while a list job culls through it: the first job to see it samples the
	 * block (PrimaryCull::StandIn), else the render thread does after the jobs; uploaded whole with the depth commit.
	 *
	 * Layout (bytes): the header (mode, operator count, plane set count, first operator), then kFadeVisibilityOps operators
	 * of four words (the engine's 12-byte {type, next if true, next if false}, padded; an operator of type 7 or 8 takes the
	 * next record's first word as its plane set), then kFadeVisibilitySets plane sets (NiFrustumPlanes: six planes as
	 * normal and constant, then the active mask).
	 */
	inline constexpr std::uint32_t kFadeVisibilityOps = 256;
	inline constexpr std::uint32_t kFadeVisibilitySets = 64;
	inline constexpr std::uint32_t kFadeVisibilityOpsOffset = 16;
	inline constexpr std::uint32_t kFadeVisibilitySetsOffset = kFadeVisibilityOpsOffset + kFadeVisibilityOps * 16;
	inline constexpr std::uint32_t kFadeVisibilitySetBytes = 112;
	// Then the process's own view planes (NiCullingProcess::planes, +0x3C), which its sphere test uses (FUN_140d3ff10).
	inline constexpr std::uint32_t kFadeVisibilityViewOffset = kFadeVisibilitySetsOffset + kFadeVisibilitySets * kFadeVisibilitySetBytes;
	inline constexpr std::uint32_t kFadeVisibilityBytes = kFadeVisibilityViewOffset + kFadeVisibilitySetBytes;
	inline constexpr std::uint32_t kFadeVisibilityValid = 1u << 0;          // sampled this frame (else: the frustum alone)
	inline constexpr std::uint32_t kFadeVisibilityCompound = 1u << 1;       // the compound frustum applies (cull mode not 3)
	inline constexpr std::uint32_t kFadeVisibilitySkipView = 1u << 2;       // its skipViewFrustum: the frustum test is not made
	inline constexpr std::uint32_t kFadeVisibilityIgnorePreprocess = 1u << 3;  // the process's ignorePreprocess, or cull mode 4
	inline constexpr std::uint32_t kFadeVisibilityCullModeShift = 8;        // the process's cull mode (0-4)
	inline constexpr std::uint32_t kFadeVisibilityViewPlanes = 1u << 4;     // the view planes are the process's (else the latch's)

	/**
	 * @brief Fade roots on the GPU (FadeStateCS.hlsl; drawcall-limit-fix.md, "Fades on the GPU"). A member's fade node is
	 * a root: one FadeRootStatic row per root (the CPU's, written when the node is listed) and one FadeNodeState row (the
	 * GPU's, seeded from the static row's `initial` when its generation is new). The pass runs the node's OnVisible for
	 * the main camera whenever its bound is in view, as the engine's cull does (Scene/FadeState.h ports it to C++).
	 *
	 * FadeNodeState is the node's own fields, as BSFadeNode keeps them (skyrim-engine-notes.md, "Fade state").
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
	// FadeStateCS's state is the members' (PrimaryCull's admitted entries): their members follow the GPU's state. The engine
	// updates every other root's node itself, and their members keep the distance test (kObjectFadeTest).
	inline constexpr std::uint32_t kFadeRootOwned = 1u << 18;
	// The node's own flags BSCullingProcess::Process1 (AE 0x140e28390) reads before OnVisible: kAlwaysDraw (bit 11) skips
	// the bound tests; kPreProcessedNode (bit 12) skips them too unless the process ignores preprocessing, and with bit 20
	// set the node is then not visited at all.
	inline constexpr std::uint32_t kFadeRootAlwaysDraw = 1u << 20;
	inline constexpr std::uint32_t kFadeRootPreprocessed = 1u << 21;
	inline constexpr std::uint32_t kFadeRootPreprocessHidden = 1u << 22;
	// An owned root with no engine-drawn part, which the stand-in culls in the engine's place: its state is FadeStateCS's alone,
	// and its node keeps what the engine last left there (nothing writes the GPU's back). DCLF's own readers take the GPU's
	// state (a shadow view's fading caster, BuildDrawsCS) or the settled one (ShadowCasterReject, the synthetic pass); the
	// engine reads the node only on the frames it culls the root again, whose OnVisible starts from it (drawcall-limit-fix.md,
	// "No fade write-back"). An owned root with engine-drawn parts is culled by the engine, whose OnVisible updates its node;
	// FadeStateCS runs the same update for its DCLF members (CS_DCLF_FADE_PARITY compares the two).
	inline constexpr std::uint32_t kFadeRootStoodIn = 1u << 19;
	inline constexpr std::uint32_t kNoFadeRoot = ~0u;

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
		// The pass's per-frame values, here rather than in its prepared invocation (which is prepared ahead of the commit):
		// the root slots, the scene frame and the parity log's first root (~0u: none).
		std::uint32_t rootCount = 0, sceneFrame = 0, logBase = ~0u, reserved = 0;
	};
	static_assert(sizeof(FadeFrame) == 208);

	/** @brief CS_DCLF_FADE_PARITY: one root's update as FadeStateCS made it, for the C++ port to make again. */
	struct FadeLogEntry
	{
		FadeNodeState before;
		FadeNodeState after;
		float centre[3]{};
		std::uint32_t root = ~0u;
	};
	static_assert(sizeof(FadeLogEntry) == 112);
	inline constexpr std::uint32_t kFadeLogEntries = 64;

	/**
	 * @brief One indirect draw, in the argument order of the command signature (BasicRHI packs arguments
	 * like D3D12, 4-byte aligned): pipeline set index, push data (its pipeline row's and material row's addresses and
	 * the object word), two vertex buffer views, index buffer view (D3D12 VBV / IBV layouts), DrawIndexed.
	 *
	 * The second view is slot 1, where a dynamic shape's positions are (FaceSnapshots; the engine's own draw
	 * binds BSDynamicTriShape::dynamicData there). Every other draw repeats its slot 0 view, which a layout
	 * without a second stream never reads.
	 *
	 * The scene tables hold the geometry part with the pipeline's table index; the main-pass epoch
	 * replaces it with the pipeline set index and fills in the rows' addresses.
	 */
#pragma pack(push, 4)
	struct DrawSequence
	{
		std::uint32_t pipelineIndex;
		// The draw's rows (IndirectDraws: the pipeline row's registers, and the material row's), and the object's index in
		// SceneStore::Tables::objects, by which the shaders reach per-object data. The five words are one contiguous Constant
		// indirect argument (the layout's per-draw push data), so they stay together, in this order.
		std::uint64_t pipelineRowAddress;
		std::uint64_t materialRowAddress;
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
	static_assert(sizeof(DrawSequence) == 92);
	static_assert(offsetof(DrawSequence, pipelineRowAddress) == 4);
	static_assert(offsetof(DrawSequence, materialRowAddress) == 12);
	static_assert(offsetof(DrawSequence, objectIndex) == 20);
	static_assert(offsetof(DrawSequence, vertexBufferAddress) == 24);
	static_assert(offsetof(DrawSequence, streamBufferAddress) == 40);
	static_assert(offsetof(DrawSequence, indexBufferAddress) == 56);
	static_assert(offsetof(DrawSequence, indexCount) == 72);
}
