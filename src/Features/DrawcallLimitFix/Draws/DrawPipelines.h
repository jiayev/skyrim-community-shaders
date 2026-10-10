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

	/** @brief Pipeline variants of a key: the main pass's (color, depth test EQUAL) and DCLF's own Z-prepass (depth only, LESS, writes). */
	inline constexpr std::uint32_t kColorVariant = 0;
	inline constexpr std::uint32_t kDepthVariant = 1;
	inline constexpr std::uint32_t kVariantCount = 2;
	/**
	 * @brief The shadow views' pipeline classes (DrawPipelines::ShadowDiscards): a pixel stage that cannot defer the depth test,
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

	/** @brief Render target and depth formats of the native main (deferred) pass. */
	struct TargetFormats
	{
		std::array<DXGI_FORMAT, 8> colors{};
		std::uint32_t colorCount = 0;
		DXGI_FORMAT depth = DXGI_FORMAT_UNKNOWN;

		bool operator==(const TargetFormats&) const = default;
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
	 * Render thread only.
	 */
	/**
	 * @brief The shadow views' program for a technique (mode bits included), requesting it, and a warning for each stage the
	 * request starts that no precompile had (on demand). a_casterKey (no mode bits) and a_occlusion name the casters that need
	 * it (SceneStore::DescribeShadowKeyUsers). The rest as ShaderPrograms::FindShadow.
	 */
	const ShaderPrograms::ShadowProgram* RequestShadowProgram(std::uint32_t a_technique, const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion,
		RE::BSShader& a_utility, bool a_allowRequest = true, bool* a_requested = nullptr);
	/** @brief DrawPipelines::FindShadow, and a warning for each build it starts (every pipeline is built at runtime). */
	std::uint32_t RequestShadowPipeline(const ShadowPipelineKey& a_viewKey, const ShaderPrograms::ShadowProgram& a_program, DXGI_FORMAT a_format,
		const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion);

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
		};

		static DrawPipelines& Get();

		bool Enabled() const;

		/** @brief The native main pass's targets; pipelines are rebuilt when they change. */
		void SetTargetFormats(const TargetFormats& a_formats);
		bool HasTargetFormats() const { return targets.colorCount != 0; }
		const TargetFormats& Targets() const { return targets; }

		/**
		 * @brief The key's index in the pipeline set, or kNotReady. Requests the pipeline on the first call
		 * once the program is compiled.
		 */
		std::uint32_t Find(const PipelineKey& a_key, const ShaderPrograms::Program& a_program, bool* a_requested = nullptr);

		/**
		 * @brief The shadow key's index in the shadow pipeline set, or kNotReady.
		 *
		 * A separate set from the main pass's: its pipelines write depth alone, into the engine's shadow
		 * map format, with the rasterizer state of the view (the key's viewState, which must be registered)
		 * and no colour attachment (engine notes: shadow maps).
		 * @param a_depthFormat the shadow map array's format; pipelines are rebuilt if it changes.
		 */
		std::uint32_t FindShadow(const ShadowPipelineKey& a_key, const ShaderPrograms::ShadowProgram& a_program, DXGI_FORMAT a_depthFormat,
			bool* a_requested = nullptr);
		/**
		 * @brief For the on-demand build warnings: the pipeline already requested whose key is nearest a_key (fewest differing
		 * bits), with the fields that differ, or why there is none (the set was recreated, and by what).
		 */
		std::string NearestKey(const PipelineKey& a_key) const;
		std::string NearestShadowKey(const ShadowPipelineKey& a_key) const;

		/**
		 * @brief The id (1 and up, as many as the views have) of a shadow view's rasterizer state, registering it
		 * the first time; 0 when the state has something a pipeline cannot express (no depth clipping,
		 * wireframe). Views with equal states share the id and so their pipelines.
		 *
		 * The state is the one the engine binds for the view, read from its table while the view is drawn
		 * (IndirectDraws::CaptureShadowView): Community Shaders' ShadowmapCascadeRasterizerFix swaps in
		 * per-cascade copies with their own depth bias for exactly that window, and the volumetric copy draws
		 * without culling. Remembers the render modes each id was seen in, for ShadowRasterStateModes.
		 */
		std::uint32_t ShadowRasterStateId(const D3D11_RASTERIZER_DESC& a_desc, std::uint32_t a_renderMode);

		/** @brief How many shadow view rasterizer states are registered (ids 1 to this). */
		std::uint32_t ShadowRasterStateCount() const;

		/** @brief The registered ids seen with a render mode, ascending. */
		std::vector<std::uint32_t> ShadowRasterStatesOfMode(std::uint32_t a_renderMode) const;

		/** @brief Adds finished pipelines to the set (call once per frame). */
		void Update();

		/**
		 * @brief Tree LOD's two pipelines (dclf-lod.md, "Tree LOD: the draws"), both under the Z-prepass's layout (pulled: the
		 * draw's push data holds the draw row's address): the Z-prepass's (DistantTree's depth technique, depth LESS with writes)
		 * and the colour pass's (its deferred technique, depth EQUAL, the engine's opaque write mode 1). Both draw two-sided, as
		 * the engine's do. Requested on the first call with the program; false until built, and again after a target change.
		 */
		bool FindTreeLod(const ShaderPrograms::TreeLodProgram& a_program, struct TreeLodPipelines& a_out);

		/** @brief Forward pipelines: requested, built, failed (FindForwardPipeline). */
		struct ForwardStats
		{
			std::uint32_t requested = 0, ready = 0, failed = 0;
		};
		const ForwardStats& GetForwardStats() const { return forwardStats; }

		/**
		 * @brief Reads the engine's rasterizer and blend state objects behind the state bits of these keys
		 * (decals: Records.h PipelineRasterFlags), so that Find can build them.
		 *
		 * Call inside the deferred pass: Community Shaders swaps the engine's blend table for its deferred
		 * variants between StartDeferred and ResetBlendStates, and those are what a native decal draw in the
		 * G-buffer uses. Read outside that window the same index names the forward state. A key whose
		 * state has not been captured yet is simply not ready; nothing is guessed.
		 */
		void CaptureEngineStates(std::span<const PipelineKey> a_keys);

		/** @brief The registers of a variant of the pipeline at a set index (one Find returned). */
		const RegisterUsage& Usage(std::uint32_t a_index, std::uint32_t a_variant = kColorVariant) const { return usage[a_index][a_variant]; }

		/** @brief The registers of the shadow pipeline at a shadow set index (one FindShadow returned). */
		const RegisterUsage& ShadowUsage(std::uint32_t a_index) const { return shadowUsage[a_index]; }
		/**
		 * @brief Whether the shadow pipeline at a set index is of the discarding class (kShadowDiscards): its pixel stage can
		 * discard or export depth, so the hardware tests its fragments' depth after shading them. A view draws the other class
		 * first, so that these fragments meet the opaque casters' depth.
		 */
		bool ShadowDiscards(std::uint32_t a_index) const { return a_index < shadowDiscards.size() && shadowDiscards[a_index] != 0; }

		/** @brief Increments whenever the set is recreated (target change): indices from before are stale. */
		std::uint32_t Generation() const { return generation; }

		const Stats& GetStats() const { return stats; }

	private:
		DrawPipelines();
		~DrawPipelines();

		struct Impl;
		std::unique_ptr<Impl> impl;
		TargetFormats targets;
		std::vector<std::array<RegisterUsage, kVariantCount>> usage;  // by set index, then variant
		std::vector<RegisterUsage> shadowUsage;                       // by shadow set index
		std::vector<std::uint8_t> shadowDiscards;                     // by shadow set index: ShadowDiscards
		std::vector<std::uint64_t> shadowLayouts;                     // by shadow set index: its key's vertex layout
		std::uint32_t generation = 0;
		Stats stats;
		ForwardStats forwardStats;

		friend struct IndirectState GetIndirectState();
		friend struct ForwardPipelineAccess;  // FindForwardPipeline (DrawPipelinesRhi.h)
		friend struct ShadowIndirectState GetShadowIndirectState();
	};
}
