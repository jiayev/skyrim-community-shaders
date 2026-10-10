#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <dxgiformat.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <bit>
#include <type_traits>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Scene/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Scene/Records.h"
#include "ShaderPrograms.h"

namespace DCLF
{
	/** @brief Constant buffer registers b0-b13 (each stage has its own, as in D3D11). */
	inline constexpr std::uint32_t kConstantBufferRegisters = 14;
	/**
	 * @brief Frame push: the registers whose block is the same for every draw of a pass under bindless draws (the frame slots, the shared light block at PS b3, the frame lighting at PS b13) read their buffers' addresses from
	 * push data (LayoutRangeSource::PushAddress), set once per pass, instead of from each draw's binding record
	 * (IndirectAddress), which saves the shaders the record's indirection (colour pass 3.3 -> 2.1 ms at Riverwood). Push data: the record address
	 * range (4 words, for alignment), then these addresses, the vertex stage's registers ascending and then the pixel stage's.
	 * The shadow views push their pass-wide blocks too, in a layout of their own (kShadowPushWords): theirs are per view and per
	 * build, not the main pass's frame slots.
	 */
	inline constexpr std::uint32_t kFramePushVS = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 11) | (1u << 12) | (1u << 13);
	inline constexpr std::uint32_t kFramePushPS = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13);
	inline constexpr std::uint32_t kFramePushBinding = 191;
	// The draw's push range (below): under device-generated commands each draw's own words; the shadow views' and the Z-prepass's
	// plain draws push them once per draw call instead (ShadowViewPass, MainOpaquePass): the call's first sequence and, a shadow
	// view's, the material rows and the vertex layout.
	inline constexpr std::uint32_t kDrawPushBinding = 190;
	// First the frame record's address (every register a draw's rows do not give), then the registers' addresses.
	inline constexpr std::uint32_t kFramePushRecord = 0;
	inline constexpr std::uint32_t kFramePushRegisters = 2;
	inline constexpr std::uint32_t kFramePushWords = kFramePushRegisters + 2 * (std::popcount(kFramePushVS) + std::popcount(kFramePushPS));
	/*
	 * A draw's own push data (DrawSequence's Constant argument): its pipeline row's address, its material row's, the object
	 * word, and a pad word so the pass's push data after it starts 8-byte aligned (BasicRHI packs the ranges back to back).
	 */
	inline constexpr std::uint32_t kDrawPushPipelineRow = 0, kDrawPushMaterialRow = 2, kDrawPushObject = 4, kDrawPushArgumentWords = 5, kDrawPushWords = 6;
	/*
	 * The Z-prepass's plain draws' push words (in the draw's range, kDrawPushBinding), per call: the call's first sequence's
	 * address, as the shadow views' (kShadowDrawPushSequences). Nothing else: a call draws every pipeline slot that shares its
	 * depth pipeline (DrawPipelines.cpp, ZPipelineKey), so the rows and the vertex layout are each draw's, read through its
	 * sequence (Lighting.hlsl, DCLF_PULLED).
	 */
	inline constexpr std::uint32_t kZDrawPushSequences = 0;
	/*
	 * A shadow view's plain draw's push words (in the draw's range, kDrawPushBinding): its first sequence's address, which the
	 * vertex stage indexes by its instance (Utility.hlsl, DCLF_PULLED); the material rows' table, an address the layout's b1
	 * and t0 ranges may resolve though the stages read the draw's row themselves; and the pipeline's vertex layout.
	 */
	inline constexpr std::uint32_t kShadowDrawPushSequences = 0, kShadowDrawPushMaterialRows = 2, kShadowDrawPushVertexLayout = 4;
	/** @brief Where a DrawSequence's draw arguments start, which the shadow views' plain indexed draws read. */
	inline constexpr std::uint32_t kSequenceDrawOffset = 72;
	/*
	 * The shadow views' layout: the draw's words are its material row's address (ShadowMaterialRow, read as the Utility
	 * vertex shader's PerMaterial block, b1, and for the diffuse, t0) and the object word; everything else is the view's,
	 * pushed once per view: six addresses (kShadowPushFrameRecord and after). The frame record gives every other texture and
	 * sampler, and each constant buffer register reads one of the pushed blocks. So the rows are one table for every view,
	 * each a constant-buffer block (256 bytes), and a view's own blocks are never copied into them.
	 */
	inline constexpr std::uint32_t kShadowPushWords = 12;
	// The view's words: the addresses of the frame record, its PerTechnique (b0) and VS_PerFrame (b12) blocks, the zero block,
	// SharedData (b5) and FeatureData (b6).
	inline constexpr std::uint32_t kShadowPushFrameRecord = 0, kShadowPushViewBlock = 2, kShadowPushPerFrame = 4, kShadowPushZeros = 6,
								   kShadowPushSharedData = 8, kShadowPushFeatureData = 10;
	inline constexpr std::uint32_t kPerFrameVertexRegister = 12;  // VS_PerFrame (Lighting.hlsl, Utility.hlsl): ViewProj at c8
	inline constexpr std::uint32_t kSharedDataRegister = 5;       // SharedData (SharedData.hlsli), bound by Community Shaders
	inline constexpr std::uint32_t kFeatureDataRegister = 6;      // FeatureData, likewise
	/** @brief Where the diffuse's descriptor index sits in a shadow material row: after its texture offset (c0). */
	inline constexpr std::uint32_t kShadowRowDiffuseOffset = 16;

	/*
	 * The main pass's rows (IndirectDraws: Resources::materialRows, pipelineRows). A draw names one of each; the registers they
	 * do not give come from the frame record. Each constant block sits at a 256-byte offset, so a table is an array of
	 * constant buffers, and each row's header holds its blocks' addresses (written at upload, from the table's base) and
	 * its descriptor indices, which the layout's indirect ranges read.
	 *
	 * A material row is a material slot's (SceneStore::Tables::materials, keyed {material, pass descriptor}): its PerMaterial
	 * blocks (b1), packed through its technique's constant tables, and its textures and samplers - the material's, a
	 * projected technique's projected textures (t3, t8, t10, t11), the features' (t71, t74). A pipeline row is a pipeline
	 * slot's: its technique blocks (b0), its PerGeometry template (b2), its permutation (b4), and its technique's shadow mask
	 * (t14, s14), and its key's vertex layout.
	 */
	inline constexpr std::uint32_t kMaterialRowBytes = 1024;
	inline constexpr std::uint32_t kMaterialRowVS = 0, kMaterialRowPS = 256, kMaterialRowHeader = 768;
	inline constexpr std::uint32_t kMaterialRowVSBytes = 256, kMaterialRowPSBytes = 512;
	struct MaterialRowHeader
	{
		std::uint64_t vsMaterial = 0, psMaterial = 0;  // b1: the row's own blocks
		std::uint32_t textures[kPixelTextureSlots]{};
		std::uint32_t samplers[16]{};
		std::uint32_t features[kFeatureMaterialTextures]{};
	};
	static_assert(kMaterialRowHeader + sizeof(MaterialRowHeader) <= kMaterialRowBytes);
	inline constexpr std::uint32_t kPipelineRowBytes = 2048;
	inline constexpr std::uint32_t kPipelineRowTechniqueVS = 0, kPipelineRowTechniquePS = 256, kPipelineRowGeometryVS = 512, kPipelineRowGeometryPS = 768,
								   kPipelineRowPermutation = 1280, kPipelineRowHeader = 1536;
	inline constexpr std::uint32_t kPipelineRowTechniqueBytes = 256, kPipelineRowGeometryVSBytes = 256, kPipelineRowGeometryPSBytes = 512,
								   kPipelineRowPermutationBytes = 256;
	struct PipelineRowHeader
	{
		std::uint64_t vsTechnique = 0, psTechnique = 0;  // b0
		std::uint64_t vsGeometry = 0, psGeometry = 0;    // b2
		std::uint64_t vsPermutation = 0, psPermutation = 0;  // b4: one block, both stages
		std::uint32_t shadowMask = 0;                    // t14
		std::uint32_t shadowMaskSampler = 0;             // s14
		std::uint64_t vertexLayout = 0;                  // the slot's key's (PipelineKey::vertexLayout), which a pulled vertex stage decodes
	};
	static_assert(kPipelineRowHeader + sizeof(PipelineRowHeader) <= kPipelineRowBytes);
	/*
	 * Every DCLF draw signature (colour, depth, shadow) is preprocessed explicitly, before the passes that execute it
	 * (CommandList::PreprocessIndirect), instead of by the driver inside each call.
	 */
	/** @brief Pixel shader resource registers t0-t127 (textures and structured buffers). */
	inline constexpr std::uint32_t kTextureRegisters = 128;
	/** @brief Pixel shader sampler registers s0-s15. */
	inline constexpr std::uint32_t kSamplerRegisters = 16;
	/**
	 * @brief t127: the per-object record buffer the DCLF_BINDLESS builds read (DCLFObjects in
	 * Lighting.hlsl), and the one texture register the layout also maps for the vertex stage.
	 *
	 * It rides the ordinary DrawBindings::textures mechanism rather than needing a binding kind of its
	 * own; every draw of an epoch carries the same heap index in it. The register is the top of the range
	 * because the engine binds nothing there, so nothing the frame capture resolves is displaced.
	 */
	inline constexpr std::uint32_t kObjectBufferRegister = kTextureRegisters - 1;
	/** @brief t126: the objects' extras rows (DCLFExtras: land blend, TextureProj, ProjectedUV), read by both stages. */
	inline constexpr std::uint32_t kExtrasBufferRegister = kTextureRegisters - 2;
	/**
	 * @brief t125: the character light's noise (DCLFCharacterLightNoise in Lighting.hlsl), which the engine binds at t11. It is a
	 * render target that alternates every frame, the frame's and not a material's: the frame record gives it, so a material row
	 * does not change with it. A character-light pass's material row binds the null texture at t11.
	 */
	inline constexpr std::uint32_t kCharacterLightRegister = kTextureRegisters - 3;
	/**
	 * @brief t124: the trees' wind the frame's draws read (DCLFTreeWind, Common/DCLFObjects.hlsli; IndirectDraws, TreeWindCS), a
	 * vertex-stage buffer like the two above it. The frame record gives it: the buffer alternates by frame.
	 */
	inline constexpr std::uint32_t kTreeWindRegister = kTextureRegisters - 4;
	/**
	 * @brief t123: the objects' placement rows (DCLFPlacements, Common/DCLFObjects.hlsli; BindlessPlacement), read by both stages: what
	 * a move changes, apart from the records (t127). The frame record gives it, like the records.
	 */
	inline constexpr std::uint32_t kPlacementBufferRegister = kTextureRegisters - 5;
	/**
	 * @brief t122: the frame's bone palettes (DCLFPalettes, Common/DCLFObjects.hlsli; FrameValues), the vertex stage's: a skinned
	 * object's current and previous palette (BindlessObject::boneOffset). The frame record gives it, like the placements; the lowest
	 * of the vertex stage's registers.
	 */
	inline constexpr std::uint32_t kPaletteBufferRegister = kTextureRegisters - 6;
	/**
	 * @brief t121: the objects' shading rows (DCLFShading, Common/DCLFObjects.hlsli; BindlessShading; FrameValues), read by both stages:
	 * MaterialData, EmitColor, SSRParams.w, the emissive multiplier and the wetness, and a tree's wind until its entry (the vertex
	 * stage's, T6b1a), by object slot. The frame record gives it, like the placements; the lowest of the vertex stage's registers.
	 */
	inline constexpr std::uint32_t kShadingBufferRegister = kTextureRegisters - 7;
	/**
	 * @brief t120 and t119: FadeStateCS's states the frame's builds read (SceneBuffers::FadeStatesReadIndex) and the fade roots'
	 * static rows, the pixel stage's: a draw's alpha takes its object's fade (T1c, Lighting.hlsl DCLFAlphaFade). The frame record
	 * gives them; a record without them (none bound) reads generation 0, which is no fade.
	 */
	inline constexpr std::uint32_t kFadeStatesRegister = kTextureRegisters - 8;
	inline constexpr std::uint32_t kFadeRootsRegister = kTextureRegisters - 9;

	/**
	 * @brief Everything one indirect draw binds, in GPU memory: its address is the draw's only push data,
	 * and the pipeline layout maps every register of the Lighting shaders onto an entry
	 * (VK_EXT_descriptor_heap indirect mappings, BasicRHI LayoutRangeSource). A register the shader does
	 * not declare is never read.
	 */
	struct DrawBindings
	{
		std::uint64_t vertexConstants[kConstantBufferRegisters];  // device addresses of the constant buffers
		std::uint64_t pixelConstants[kConstantBufferRegisters];
		std::uint32_t textures[kTextureRegisters];  // resource descriptor heap indices
		std::uint32_t samplers[kSamplerRegisters];  // sampler descriptor heap indices
	};
	static_assert(sizeof(DrawBindings) == 800);

	/** @brief The registers a pipeline's shaders declare, which a draw's DrawBindings must supply. */
	struct RegisterUsage
	{
		std::uint32_t vertexConstants = 0;  // b0-b13, bit per register
		std::uint32_t pixelConstants = 0;
		std::array<std::uint64_t, kTextureRegisters / 64> textures{};  // pixel t0-t127
		std::uint32_t samplers = 0;                                      // pixel s0-s15

		bool UsesTexture(std::uint32_t a_register) const { return (textures[a_register / 64] >> (a_register % 64)) & 1; }
	};

	/**
	 * @brief A Lighting pipeline's constant tables (Lookups::Pipeline::vsTable, psTable), which the builds pack its constant groups by:
	 * per Lighting variable (ShaderConstants::LightingVS, LightingPS), its offset in floats in its block (PerTechnique, PerMaterial or
	 * PerGeometry), 0 where the stage has none (offset 0 counts only for a group's first variable: LightingConstants.cpp, OffsetOf).
	 * What Community Shaders' ShaderCache makes of the game's shader objects (constantTable, ReflectConstantBuffers), made by the
	 * pipeline lane of DCLF's own modules (SPIR-V reflection, T6b2c): the stages the builds feed. Fixed per built pipeline.
	 */
	struct ConstantTables
	{
		std::vector<std::uint8_t> vs, ps;  // kLightingVSVariables, kLightingPSVariables entries
	};

	struct Lookups;

	/** @brief Pipeline variants of a key: the main pass's (color, depth test EQUAL) and DCLF's own Z-prepass (depth only, LESS, writes). */
	inline constexpr std::uint32_t kColorVariant = 0;
	inline constexpr std::uint32_t kDepthVariant = 1;
	inline constexpr std::uint32_t kVariantCount = 2;
	/**
	 * @brief The shadow views' pipeline classes (ShadowIndirectState::discards): a pixel stage that cannot defer the depth test,
	 * and one that can (an alpha test's discard). A view draws the first class's pipelines, then the second's.
	 */
	inline constexpr std::uint32_t kShadowDepthOnly = 0;
	inline constexpr std::uint32_t kShadowDiscards = 1;
	inline constexpr std::uint32_t kShadowClasses = 2;

	/**
	 * @brief A forward view's targets (the water reflection's cube faces: dclf-lod.md, "Water reflections"): its one colour
	 * target's format and its depth's (a depth-stencil view's format, not a typeless resource's).
	 */
	struct ForwardTargets
	{
		DXGI_FORMAT colour = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT depth = DXGI_FORMAT_UNKNOWN;

		bool operator==(const ForwardTargets&) const = default;
	};

	/**
	 * @brief A forward view's pipeline, as asked of the pipeline lane (DrawPipelines::RequestForward): a Lighting forward program's
	 * descriptors (ShaderPrograms::FindForward: the main pipeline's vertex descriptor, its pixel descriptor without Deferred) and
	 * whether it draws two-sided, or tree LOD's forward program (kForwardTreeLod, the descriptors 0). Built for the forward targets
	 * (DrawPipelines::SetForwardTargets), culling front faces unless two-sided: a cube face's projection mirrors the image, and the
	 * engine culls front faces there (dclf-lod.md, "The state").
	 */
	struct ForwardPipelineKey
	{
		static constexpr std::uint32_t kForwardTwoSided = 1, kForwardTreeLod = 2;
		std::uint32_t vertexDescriptor = 0, pixelDescriptor = 0;
		std::uint32_t flags = 0;  // kForward*

		bool operator==(const ForwardPipelineKey&) const = default;
	};
	static_assert(std::has_unique_object_representations_v<ForwardPipelineKey>);
	struct ForwardPipelineKeyHash
	{
		using is_avalanching = void;
		std::uint64_t operator()(const ForwardPipelineKey& a_key) const noexcept { return ankerl::unordered_dense::detail::wyhash::hash(&a_key, sizeof(a_key)); }
	};

	/** @brief Render target and depth formats of the native main (deferred) pass. */
	struct TargetFormats
	{
		std::array<DXGI_FORMAT, 8> colors{};
		std::uint32_t colorCount = 0;
		DXGI_FORMAT depth = DXGI_FORMAT_UNKNOWN;

		bool operator==(const TargetFormats&) const = default;
	};

	/**
	 * @brief What the pipeline lane has made of the Lighting and the shadow keys asked for (T6b2c steps 4 and 6): immutable,
	 * published whole by the lane after each pass that changed it, latest-wins (DrawPipelines::TakeCatalog).
	 *
	 * A key's set index is handed out only once a published set version holds its pipeline, and setVersion is that version (or a
	 * later one of the same generation, which holds every index an earlier one did): the indices and the set agree by construction.
	 * The frame that takes the catalog binds this version (GetIndirectState), so its lookups' indices are the set's. The shadow
	 * views' set is the same again (shadowEntries, shadowSetVersion; GetShadowIndirectState).
	 *
	 * Tree LOD's and the forward views' pipelines (T6b2c step 9: treeLod, forwardEntries) are no set's: each is a pipeline the lane keeps
	 * for the process once built, so the frame resolves them from its catalog alone (TreeLodPipelinesOf, ForwardPipelineOf).
	 */
	struct PipelineCatalog
	{
		static constexpr std::uint32_t kNone = ~0u;
		struct Entry
		{
			std::uint32_t setIndex = kNone;  // kNone while its build is pending, and for good once failed
			bool failed = false;
			std::array<RegisterUsage, kVariantCount> usage{};  // by variant, once setIndex is
			std::shared_ptr<const ConstantTables> tables;      // once setIndex is: its stages' (built with it)
		};
		// Every key the lane has requested a build for, or found failed (one still waiting on its program or an input is absent).
		ankerl::unordered_dense::map<PipelineKey, Entry, PipelineKeyHash> entries;
		std::uint32_t generation = 0;         // the main set's: a target change starts a new set, and every index of the last is stale
		std::uint32_t targetsGeneration = 0;  // the render thread's target generation the set is built for (DrawPipelines::Generation)
		std::uint64_t revision = 0;           // new with each publication
		std::shared_ptr<const void> setVersion;  // the main set version holding every setIndex above (DrawPipelines.cpp, SetVersion)

		struct ShadowEntry
		{
			std::uint32_t setIndex = kNone;  // kNone while its build is pending, and for good once failed
			bool failed = false;
		};
		// Every shadow view key (a technique with its mode's bits, under a view rasterizer state) the lane has requested a build for,
		// or found failed.
		ankerl::unordered_dense::map<ShadowPipelineKey, ShadowEntry, ShadowPipelineKeyHash> shadowEntries;
		std::uint32_t shadowGeneration = 0;                 // the shadow set's: a format change starts a new set
		DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;     // the shadow map format the set is built for (DrawPipelines::SetShadowInputs)
		std::shared_ptr<const void> shadowSetVersion;       // the shadow set version holding every setIndex of shadowEntries

		// Tree LOD's two pipelines (DrawPipelines::RequestTreeLod), of the targets of targetsGeneration: built (DrawPipelines.cpp,
		// TreeLodBuilt), or failed (its program for good, its build until the next target change). Neither while it waits.
		struct TreeLodEntry
		{
			std::shared_ptr<const void> pipelines;
			bool failed = false;
		};
		TreeLodEntry treeLod;
		struct ForwardEntry
		{
			std::shared_ptr<const void> pipeline;  // once built (an rhi::PipelinePtr)
			bool program = false;                  // its program is compiled
			bool failed = false;                   // its program or its build failed, for good
		};
		// Every forward view key (DrawPipelines::RequestForward) whose program the lane has, or found failed, built for forwardTargets.
		ankerl::unordered_dense::map<ForwardPipelineKey, ForwardEntry, ForwardPipelineKeyHash> forwardEntries;
		ForwardTargets forwardTargets;  // the forward targets the lane has (DrawPipelines::SetForwardTargets)

		const Entry* Find(const PipelineKey& a_key) const
		{
			const auto it = entries.find(a_key);
			return it == entries.end() ? nullptr : &it->second;
		}
		const ShadowEntry* FindShadow(const ShadowPipelineKey& a_key) const
		{
			const auto it = shadowEntries.find(a_key);
			return it == shadowEntries.end() ? nullptr : &it->second;
		}
		const ForwardEntry* FindForward(const ForwardPipelineKey& a_key) const
		{
			const auto it = forwardEntries.find(a_key);
			return it == forwardEntries.end() ? nullptr : &it->second;
		}
	};

	/**
	 * @brief Graphics pipelines of the DCLF indirect draws, and the indirect pipeline set (Vulkan indirect
	 * execution set) the draws select them from.
	 *
	 * Every pipeline shares one layout: two words of push data holding the draw's DrawBindings address.
	 * A pipeline is built, asynchronously through ORGModuleServices' PipelineService, from a pipeline key
	 * and its SPIR-V program: vertex input from the engine's input layout for the geometry's vertex layout
	 * (VertexInput.h), fixed-function state of the native main pass (depth EQUAL without writes, back-face
	 * culling unless two-sided, no blending), and the main pass's target formats. A program that uses a
	 * register outside the layout, or a vertex input the layout cannot supply, fails; its objects stay
	 * native. Ready pipelines get the next index of the set, which is what DrawSequence::pipelineIndex holds.
	 *
	 * One owner (T6b2c steps 4, 6 and 9). The pipeline lane (BuildExecutor.h) owns the main set, the shadow views' set, tree LOD's
	 * pipelines and the forward views': it takes the requests (RequestLighting, RequestShadow, RequestTreeLod, RequestForward), the
	 * programs (ShaderPrograms::Find, FindShadow, FindTreeLod, FindForward, FindForwardTreeLod) and the builds' completions, admits
	 * them, publishes the set versions and then the catalog (PipelineCatalog). What it needs of the render thread reaches it as the
	 * frame's inputs (SetTargetFormats, CaptureEngineStates, SetShadowInputs, ShadowRasterStateId, SetTreeLodInputs,
	 * SetForwardTargets: the targets, the engine's state objects it asked for, the winding, the shadow map format, the Utility shader,
	 * the view rasterizer states, the DistantTree shader and the forward views' targets). The render thread only posts, and reads
	 * the catalog its frame took (FrameCatalog).
	 */
	class DrawPipelines
	{
	public:
		static constexpr std::uint32_t kNotReady = ~0u;
		static constexpr std::uint32_t kMaxPipelines = 4096;

		struct Stats
		{
			std::uint32_t requested = 0;
			std::uint32_t ready = 0;  // in the set
			std::uint32_t failed = 0;
			// The admitted pipelines' Z-prepass pipelines (DrawPipelines.cpp, ZPipelineKey): distinct, and of them without a pixel stage.
			std::uint32_t zPipelines = 0, zDepthOnly = 0;
			std::uint32_t targetChanges = 0;
			std::uint32_t shadowRequested = 0;
			std::uint32_t shadowReady = 0;
			std::uint32_t shadowFailed = 0;
			// Set versions (DrawPipelines.cpp, SetVersion): published with new pipelines, and frames whose admissions
			// waited because every version was still held.
			std::uint32_t setPublishes = 0, setWaits = 0, shadowSetPublishes = 0, shadowSetWaits = 0;
			// Tree LOD's pipeline pairs and the forward views' pipelines (the lane's, this target generation's), and the frames whose tree
			// LOD or forward view wanted one its catalog did not have yet (NoteFrameWaits: they draw natively meanwhile).
			std::uint32_t treeLodRequested = 0, treeLodReady = 0, treeLodFailed = 0;
			std::uint32_t forwardRequested = 0, forwardReady = 0, forwardFailed = 0;
			std::uint32_t treeLodWaits = 0, forwardWaits = 0;
		};

		static DrawPipelines& Get();

		bool Enabled() const;

		/** @brief Render thread: the native main pass's targets; pipelines are rebuilt when they change (the lane, from its inputs). */
		void SetTargetFormats(const TargetFormats& a_formats);
		bool HasTargetFormats() const { return targets.colorCount != 0; }
		const TargetFormats& Targets() const { return targets; }

		/**
		 * @brief Any thread: asks the pipeline lane for the key's program and pipeline. Once per key is enough: the lane keeps every key
		 * asked for, and builds them all again after a target change. a_slot names the pipeline slot that asked, for the on-demand
		 * warnings.
		 */
		void RequestLighting(const PipelineKey& a_key, std::uint32_t a_slot);
		/**
		 * @brief The one consumer (the scene lane, SceneStore::ResolveLookups, T6b2c step 5): the newest catalog the lane published since
		 * the last call, null when there is none newer (the caller keeps the last). Its lookups are resolved from it and published with it.
		 */
		std::shared_ptr<const PipelineCatalog> TakeCatalog();
		/**
		 * @brief Render thread, the frame's start (SceneStore::HandOverAtFrameStart): the catalog the installed publication's lookups were
		 * resolved from, held for the frame (FrameCatalog, FrameIndirectState and FrameShadowIndirectState read it). The frame's alone:
		 * a revision names its request's catalog explicitly.
		 */
		void HoldCatalog(std::shared_ptr<const PipelineCatalog> a_catalog) { heldCatalog = std::move(a_catalog); }
		/** @brief The frame's catalog (HoldCatalog), null before the first. */
		const std::shared_ptr<const PipelineCatalog>& HeldCatalog() const { return heldCatalog; }

		/**
		 * @brief Any thread: asks the pipeline lane for a shadow view key's program and pipeline (its index arrives in a later catalog:
		 * PipelineCatalog::shadowEntries). Once per key is enough: the lane keeps every key asked for, and builds them all again after
		 * a format change. a_casterKey (no mode bits) and a_occlusion name the casters that need it, for the on-demand warnings.
		 *
		 * A separate set from the main pass's: its pipelines write depth alone, into the shadow map format (SetShadowInputs), with
		 * the rasterizer state of the view (the key's viewState, ShadowRasterStateId) and no colour attachment (engine notes: shadow
		 * maps).
		 */
		void RequestShadow(const ShadowPipelineKey& a_viewKey, const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion);
		/**
		 * @brief Render thread: the shadow views' frame inputs of the lane: the shadow map array's format (the depth-stencil view's;
		 * the shadow pipelines are rebuilt if it changes) and the Utility shader whose techniques the programs build. Posted when
		 * either changes.
		 */
		void SetShadowInputs(DXGI_FORMAT a_format, RE::BSShader& a_utility);

		/**
		 * @brief Render thread: the id (1 and up, as many as the views have) of a shadow view's rasterizer state, registering it
		 * the first time; 0 when the state has something a pipeline cannot express (no depth clipping,
		 * wireframe). Views with equal states share the id and so their pipelines.
		 *
		 * The state is the one the engine binds for the view, read from its table while the view is drawn
		 * (IndirectDraws::CaptureShadowView): Community Shaders' ShadowmapCascadeRasterizerFix swaps in
		 * per-cascade copies with their own depth bias for exactly that window, and the volumetric copy draws
		 * without culling. The registry is the render thread's (an id is used as soon as it is returned); a new
		 * state goes to the pipeline lane with the frame's inputs, which builds a key of that id once it has it.
		 * a_renderMode names the view's mode in the log.
		 */
		std::uint32_t ShadowRasterStateId(const D3D11_RASTERIZER_DESC& a_desc, std::uint32_t a_renderMode);

		/** @brief How many shadow view rasterizer states are registered (ids 1 to this). */
		std::uint32_t ShadowRasterStateCount() const;

		/**
		 * @brief Render thread: asks the pipeline lane for tree LOD's two pipelines (dclf-lod.md, "Tree LOD: the draws"), both under the
		 * Z-prepass's layout (pulled: the draw's push data holds the draw row's address): the Z-prepass's (DistantTree's depth
		 * technique, depth LESS with writes) and the colour pass's (its deferred technique, depth EQUAL, the engine's opaque write
		 * mode 1). Both draw two-sided, as the engine's do. Once is enough: the lane builds them again after a target change. They
		 * arrive in a later catalog (PipelineCatalog::treeLod; TreeLodPipelinesOf), once the DistantTree shader (SetTreeLodInputs), the
		 * targets, the winding and the opaque write mode's state are among the lane's inputs.
		 */
		void RequestTreeLod();
		/** @brief Render thread: the DistantTree shader whose programs tree LOD's pipelines build (a frame input of the lane; posted when it changes). */
		void SetTreeLodInputs(RE::BSShader& a_distantTree);
		/**
		 * @brief Render thread: asks the pipeline lane for a forward view's pipeline (ForwardPipelineKey): the forward program's pulled
		 * stages, the forward targets, depth LESS_EQUAL with writes, the key's cull, the engine's winding, no blending. Once per key is
		 * enough (repeats are dropped here). It arrives in a later catalog (PipelineCatalog::forwardEntries; ForwardPipelineOf).
		 */
		void RequestForward(const ForwardPipelineKey& a_key);
		/** @brief Render thread: the forward views' targets (a frame input of the lane; posted when they change, every forward pipeline built again). */
		void SetForwardTargets(const ForwardTargets& a_targets);
		/** @brief Render thread: a frame whose tree LOD, or whose forward view, waited for a pipeline its catalog did not have (Stats). */
		void NoteFrameWaits(bool a_treeLod, bool a_forward)
		{
			stats.treeLodWaits += a_treeLod ? 1u : 0u;
			stats.forwardWaits += a_forward ? 1u : 0u;
		}

		/**
		 * @brief Render thread: reads the engine's rasterizer and blend state objects behind the state bits the pipeline lane asked for
		 * (decals: Records.h PipelineRasterFlags) and the opaque groups' write modes, and the engine's winding, and hands what is
		 * new to the lane with the frame's inputs (a request and its reply).
		 *
		 * Call inside the deferred pass: Community Shaders swaps the engine's blend table for its deferred
		 * variants between StartDeferred and ResetBlendStates, and those are what a native decal draw in the
		 * G-buffer uses. Read outside that window the same index names the forward state. A key whose
		 * state has not been captured yet is simply not ready; nothing is guessed.
		 */
		void CaptureEngineStates();


		/** @brief Render thread: increments whenever the targets change (the lane recreates the main set for them). */
		std::uint32_t Generation() const { return generation; }
		/**
		 * @brief Render thread: the shadow map format as last posted (SetShadowInputs), which a catalog's shadow set must be built for
		 * (GetShadowIndirectState). A frame input: a revision reads its inputs' copy.
		 */
		DXGI_FORMAT ShadowFormat() const;

		/** @brief Any thread: the render thread's counters with the lane's (relaxed reads, for the report). */
		Stats GetStats() const;

		/** @brief CS_DCLF_PERSISTENT_PARITY: the catalog's constant tables against Community Shaders' (CheckConstantTables). */
		struct ConstantTableParity
		{
			std::uint32_t checks = 0;       // parity frames
			std::uint32_t entries = 0;      // resolved pipeline entries compared
			std::uint32_t uncached = 0;     // of them, ShaderCache had no shader object for yet (not compared)
			std::uint32_t variables = 0;    // variables in both tables, compared
			std::uint32_t differ = 0;       // of them, at another offset
			std::uint32_t catalogOnly = 0;  // in DCLF's table alone (its stage reads one the game's does not declare)
			std::uint32_t cacheOnly = 0;    // in ShaderCache's alone (DCLF_BINDLESS's own blocks, or a block the stage never reads)
			std::string first, catalogOnlyFirst;
		};
		/**
		 * @brief Render thread, a parity frame: each resolved entry's tables (the frame's lookups, from the catalog) against the game's
		 * shader objects' (ShaderCache, which takes its lock and may start a compile: an observer only, never what the builds read).
		 */
		void CheckConstantTables(const RE::BSShader& a_lighting, const Lookups& a_lookups);
		/** @brief Render thread: what CheckConstantTables found since the last call. */
		ConstantTableParity TakeConstantTableParity() { return std::exchange(tableParity, {}); }

	private:
		DrawPipelines();
		~DrawPipelines();

		struct Impl;
		std::unique_ptr<Impl> impl;
		TargetFormats targets;
		std::shared_ptr<const PipelineCatalog> heldCatalog;          // the frame's (HoldCatalog: the installed publication's)
		std::uint32_t generation = 0;
		Stats stats;
		ConstantTableParity tableParity;

		friend struct IndirectState GetIndirectState(const PipelineCatalog&, std::uint32_t);
		friend struct ForwardPipelineAccess;  // TreeLodPipelinesOf (DrawPipelinesRhi.h)
		friend struct ShadowIndirectState GetShadowIndirectState(const PipelineCatalog&, DXGI_FORMAT);
	};

	/**
	 * @brief Render thread: the frame's catalog, the one its tree LOD and forward views resolve their pipelines from (DecideTreeLod,
	 * PrepareReflection): the installed publication's, whose lookups were resolved from it (DrawPipelines::HoldCatalog, at HandOverAtFrameStart,
	 * before both), the same FrameIndirectState and FrameShadowIndirectState bind. The one place that names where it comes from.
	 */
	inline const PipelineCatalog* FrameCatalog()
	{
		return DrawPipelines::Get().HeldCatalog().get();
	}
}
