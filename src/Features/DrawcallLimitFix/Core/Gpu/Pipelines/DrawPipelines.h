#pragma once

// DCLF's Lighting, shadow, tree LOD, forward and refraction pipelines: the GPU layout constants the builds and shaders share (push
// words, row layouts, fixed texture registers), the catalog the pipeline lane publishes (PipelineCatalog), and DrawPipelines, whose
// requests any thread or the render thread posts and whose lane runs on the coordinator executor (BuildExecutor.h).

#include <array>
#include <cstddef>
#include <cstdint>
#include <dxgiformat.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <bit>
#include <type_traits>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

namespace DCLF
{
	/** @brief Constant buffer registers b0-b13 (each stage has its own, as in D3D11). */
	inline constexpr std::uint32_t kConstantBufferRegisters = 14;
	/**
	 * @brief Frame push: registers whose block is the same for every draw of a pass (frame slots, PS b3 shared lights, PS b13 frame
	 * lighting) read their addresses from push data set once per pass (LayoutRangeSource::PushAddress), saving an indirection. Push
	 * data: the record address range (4 words, for alignment), then these addresses, VS registers ascending, then PS. The shadow views
	 * use their own layout (kShadowPushWords).
	 */
	inline constexpr std::uint32_t kFramePushVS = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 11) | (1u << 12) | (1u << 13);
	inline constexpr std::uint32_t kFramePushPS = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13);
	inline constexpr std::uint32_t kFramePushBinding = 191;
	// The draw's push range: each draw's words under device-generated commands; once per call for the shadow views' and the
	// Z-prepass's plain draws (ShadowViewPass, MainOpaquePass).
	inline constexpr std::uint32_t kDrawPushBinding = 190;
	// The frame record's address (every register a draw's rows do not give), then the registers' addresses.
	inline constexpr std::uint32_t kFramePushRecord = 0;
	inline constexpr std::uint32_t kFramePushRegisters = 2;
	inline constexpr std::uint32_t kFramePushWords = kFramePushRegisters + 2 * (std::popcount(kFramePushVS) + std::popcount(kFramePushPS));
	// A draw's push data (DrawSequence's Constant argument: its bytes 4-55): pipeline row address, material row address, object word,
	// then the geometry's two vertex buffer views (address, size, stride each), which the vertex stage pulls its attributes through
	// (DCLF_PULLED_VERTICES; DCLFObjects.hlsli, DCLFPushData, whose packoffsets are these words), and a pad so the pass's push data
	// after it is 8-byte aligned (BasicRHI packs the ranges back to back).
	inline constexpr std::uint32_t kDrawPushPipelineRow = 0, kDrawPushMaterialRow = 2, kDrawPushObject = 4, kDrawPushVertexBuffer = 5, kDrawPushStreamBuffer = 9,
								   kDrawPushArgumentWords = 13, kDrawPushWords = 14;
	static_assert(kDrawPushWords % 2 == 0 && kDrawPushArgumentWords < kDrawPushWords);
	// Push word w is the sequence's byte 4 + 4w, and the argument ends where the index buffer view starts.
	static_assert(offsetof(DrawSequence, pipelineRowAddress) == 4 + 4 * kDrawPushPipelineRow && offsetof(DrawSequence, materialRowAddress) == 4 + 4 * kDrawPushMaterialRow &&
				  offsetof(DrawSequence, objectIndex) == 4 + 4 * kDrawPushObject && offsetof(DrawSequence, vertexBufferAddress) == 4 + 4 * kDrawPushVertexBuffer &&
				  offsetof(DrawSequence, streamBufferAddress) == 4 + 4 * kDrawPushStreamBuffer &&
				  offsetof(DrawSequence, indexBufferAddress) == 4 + 4 * kDrawPushArgumentWords);
	// Z-prepass plain draws' push word, per call: the first sequence's address. A call draws every pipeline slot sharing its depth
	// pipeline (ZPipelineKey), so rows and vertex layout are read per draw through its sequence (Lighting.hlsl, DCLF_PULLED).
	inline constexpr std::uint32_t kZDrawPushSequences = 0;
	// Terrain Blending's depth offset (Utility.hlsl, OFFSET_DEPTH), in the shadow views' vertex layout word: 1 for a terrain group 1
	// twin bucket's call (MainOpaquePass); the pulled Lighting vertex stage adds it to clip z.
	inline constexpr std::uint32_t kZDrawPushOffsetDepth = 4;
	// A shadow view's plain draw's push words: the first sequence's address (indexed by instance; Utility.hlsl, DCLF_PULLED), the
	// material rows' table (for the layout's b1/t0 ranges; the stages read the draw's row themselves), the vertex layout.
	inline constexpr std::uint32_t kShadowDrawPushSequences = 0, kShadowDrawPushMaterialRows = 2, kShadowDrawPushVertexLayout = 4;
	/** @brief Where a DrawSequence's draw arguments start, which the shadow views' plain indexed draws read. */
	inline constexpr std::uint32_t kSequenceDrawOffset = 72;
	/*
	 * The shadow views' layout: a draw's words are its material row's address (ShadowMaterialRow: Utility's PerMaterial b1, and the
	 * diffuse t0) and the object word; the rest is pushed once per view (six addresses below). The frame record gives every other
	 * texture and sampler. The rows are one table for every view (256-byte blocks); a view's blocks are never copied into them.
	 */
	inline constexpr std::uint32_t kShadowPushWords = 12;
	// Addresses of the frame record, PerTechnique (b0), VS_PerFrame (b12), the zero block, SharedData (b5), FeatureData (b6).
	inline constexpr std::uint32_t kShadowPushFrameRecord = 0, kShadowPushViewBlock = 2, kShadowPushPerFrame = 4, kShadowPushZeros = 6,
								   kShadowPushSharedData = 8, kShadowPushFeatureData = 10;
	inline constexpr std::uint32_t kPerFrameVertexRegister = 12;  // VS_PerFrame (Lighting.hlsl, Utility.hlsl): ViewProj at c8
	inline constexpr std::uint32_t kSharedDataRegister = 5;       // SharedData (SharedData.hlsli), bound by Community Shaders
	inline constexpr std::uint32_t kFeatureDataRegister = 6;      // FeatureData, likewise
	/** @brief The diffuse's descriptor index in a shadow material row, after its texture offset (c0). */
	inline constexpr std::uint32_t kShadowRowDiffuseOffset = 16;

	/*
	 * The main pass's rows (Resources::materialRows, pipelineRows). A draw names one of each; other registers come from the frame
	 * record. Blocks sit at 256-byte offsets (a table is an array of constant buffers); each row's header holds its blocks' addresses
	 * (written at upload) and descriptor indices, which the layout's indirect ranges read.
	 *
	 * Material row (SceneTables::materials, keyed {material, pass descriptor}): PerMaterial (b1), its textures and samplers, projected
	 * textures (t3, t8, t10, t11), feature textures (the material contributors', by slot: Extensions::FeatureTextureRegister, today
	 * t71, t74, t80-t91). Pipeline row: technique blocks
	 * (b0), PerGeometry template (b2), permutation (b4), shadow mask (t14, s14), vertex layout.
	 */
	inline constexpr std::uint32_t kMaterialRowBytes = 1024;
	inline constexpr std::uint32_t kMaterialRowVS = 0, kMaterialRowPS = 256, kMaterialRowHeader = 768;
	inline constexpr std::uint32_t kMaterialRowVSBytes = 256, kMaterialRowPSBytes = 512;
	struct MaterialRowHeader
	{
		std::uint64_t vsMaterial = 0, psMaterial = 0;  // b1: the row's own blocks
		std::uint32_t textures[kPixelTextureSlots]{};
		std::uint32_t samplers[16]{};
		std::uint32_t features[Extensions::kFeatureTextureSlots]{};
	};
	static_assert(kMaterialRowHeader + sizeof(MaterialRowHeader) <= kMaterialRowBytes);
	// DCLFMaterialTextureIndex (DCLFObjects.hlsli) reads the features from the 32nd word after the addresses, in register order.
	static_assert(offsetof(MaterialRowHeader, features) == 16 + 4 * 32);
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
		std::uint64_t vertexLayout = 0;                  // PipelineKey::vertexLayout, decoded by a pulled vertex stage
	};
	static_assert(kPipelineRowHeader + sizeof(PipelineRowHeader) <= kPipelineRowBytes);
	// Every DCLF draw signature is preprocessed explicitly before the passes that execute it (CommandList::PreprocessIndirect).
	/** @brief Pixel shader resource registers t0-t127 (textures and structured buffers). */
	inline constexpr std::uint32_t kTextureRegisters = 128;
	/** @brief Pixel shader sampler registers s0-s15. */
	inline constexpr std::uint32_t kSamplerRegisters = 16;
	/**
	 * @brief t127: the per-object record buffer (DCLFObjects), mapped for both stages through DrawBindings::textures. Top of the
	 * range because the engine binds nothing there.
	 */
	inline constexpr std::uint32_t kObjectBufferRegister = kTextureRegisters - 1;
	/** @brief t126: the objects' extras rows (DCLFExtras: land blend, TextureProj, ProjectedUV), read by both stages. */
	inline constexpr std::uint32_t kExtrasBufferRegister = kTextureRegisters - 2;
	/**
	 * @brief t125: the character light's noise (DCLFCharacterLightNoise), which the engine binds at t11. A render target alternating
	 * every frame, so the frame record gives it; a character-light material row binds the null texture at t11.
	 */
	inline constexpr std::uint32_t kCharacterLightRegister = kTextureRegisters - 3;
	// t124: a feature's frame binding (Api::FrameBinding; Features/TreeWind's wind rows, kWindRegister): the frame record gives it.
	/** @brief t123: the objects' placement rows (DCLFPlacements; BindlessPlacement), both stages: what a move changes. Frame record. */
	inline constexpr std::uint32_t kPlacementBufferRegister = kTextureRegisters - 5;
	/**
	 * @brief t122: the frame's bone palettes (DCLFPalettes; FrameValues), vertex stage: a skinned object's current and previous
	 * palette (BindlessObject::boneOffset). Frame record.
	 */
	inline constexpr std::uint32_t kPaletteBufferRegister = kTextureRegisters - 6;
	/**
	 * @brief t121: the objects' shading rows (DCLFShading; BindlessShading), both stages, by object slot: MaterialData, EmitColor,
	 * SSRParams.w, emissive multiplier, wetness, a tree's wind until its entry. Frame record; the lowest vertex-stage register.
	 */
	inline constexpr std::uint32_t kShadingBufferRegister = kTextureRegisters - 7;
	/**
	 * @brief t120/t119: FadeStateCS's states (the frame's read parity, FadeScene::FadeStatesRead) and the fade roots' rows, pixel stage
	 * (DCLFAlphaFade). Frame bindings of the root states (Api::FrameBinding: Core's fade roots, RegisterCoreFadeRender; Features/Fades'
	 * constants in stage F3); unbound reads generation 0, no fade.
	 */
	inline constexpr std::uint32_t kFadeStatesRegister = kTextureRegisters - 8;
	inline constexpr std::uint32_t kFadeRootsRegister = kTextureRegisters - 9;
	/**
	 * @brief t117: the fade roots' seeds the frame reads (Features/Fades' "fade-seeds" pool, two rows a root slot: FadeSeedRow), pixel stage
	 * (DCLFAlphaFade): a root's state until FadeStateCS's first update of its generation reaches t120, as BuildDrawsCS's SeedState. A frame
	 * binding answered with the pool's descriptor; unbound reads none, the row's own.
	 */
	inline constexpr std::uint32_t kFadeSeedsRegister = kTextureRegisters - 11;

	/**
	 * @brief A binding record in GPU memory, one entry per Lighting register, mapped by the layout (VK_EXT_descriptor_heap indirect
	 * mappings, LayoutRangeSource). The frame record is one. Undeclared registers are never read.
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
	 * @brief A pipeline's constant tables (Lookups::Pipeline::vsTable, psTable), which the builds pack constant groups by: per
	 * Lighting variable, its float offset in its block, 0 where absent (offset 0 counts only for a group's first variable:
	 * LightingConstants.cpp, OffsetOf). DCLF's equivalent of ShaderCache's constantTable, from SPIR-V reflection of its own modules.
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
	 * @brief A main pipeline's state class word (IndirectState::classOf, by set index; BuildDrawsCS's class map mirrors it): its class in
	 * the high bits, its index in that class's indirect execution sets in the low ones. Every pipeline of a class has the same
	 * fixed-function state and fragment interface in both variants, as an execution set's pipelines must
	 * (VUID-vkUpdateIndirectExecutionSetPipelineEXT-None-11040 and its siblings); kNoSetClass: none (BuildDraws drops the draw).
	 */
	inline constexpr std::uint32_t kSetClassShift = 16, kSetClassIndexMask = 0xFFFFu, kNoSetClass = ~0u;
	/**
	 * @brief The refraction draw's Utility technique (UtilityShaderFlags): RenderNormal | Texture | Normals (Utility.hlsl,
	 * DCLF_REFRACTION). The engine's technique (GetRenderPasses, AE 0x1414adfb0) also varies by Vc, Skinned, RenderNormalFalloff,
	 * RenderNormalClamp and model-space normals; the pulled build reads those per draw, so one pipeline draws every refraction object.
	 */
	inline constexpr std::uint32_t kRefractionTechnique = 0x20Au;
	/** @brief Shadow pipeline classes (ShadowIndirectState::discards): no deferred depth test, then discarding; drawn in that order. */
	inline constexpr std::uint32_t kShadowDepthOnly = 0;
	inline constexpr std::uint32_t kShadowDiscards = 1;
	inline constexpr std::uint32_t kShadowClasses = 2;

	/** @brief A forward view's target formats (water reflection cube faces: dclf-architecture.md, "Water reflections"); depth is the DSV format, not typeless. */
	struct ForwardTargets
	{
		DXGI_FORMAT colour = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT depth = DXGI_FORMAT_UNKNOWN;
		// Second colour attachment (first-person motion vectors, FUN_1414cb6d0's slot 1); unknown: one attachment.
		DXGI_FORMAT colour2 = DXGI_FORMAT_UNKNOWN;

		bool operator==(const ForwardTargets&) const = default;
	};

	/**
	 * @brief A forward view's pipeline (DrawPipelines::RequestForward): a Lighting forward program's descriptors (the pixel one
	 * without Deferred), or a draw family's (its lane slot's forwardFlags: tree LOD's kForwardTreeLod; descriptors 0). Culls front faces
	 * unless two-sided: a cube face's projection
	 * mirrors the image, and the engine culls front faces there (dclf-architecture.md, "The state").
	 */
	struct ForwardPipelineKey
	{
		// Core's bits; a view's own bits above them (Api::ViewKeyRule::keyFlags, the feature's) name the key's view: its targets
		// (SetViewForwardTargets) and its culling (cullBackFaces: back faces unless two-sided, else front faces).
		static constexpr std::uint32_t kForwardTwoSided = 1, kForwardTreeLod = 2;
		std::uint32_t vertexDescriptor = 0, pixelDescriptor = 0;
		std::uint32_t flags = 0;  // kForward*

		bool operator==(const ForwardPipelineKey&) const = default;
	};
	static_assert(std::has_unique_object_representations_v<ForwardPipelineKey>);
	static_assert(DrawFamilyLaneSlotOf(Api::DrawFamily::TreeInstances).forwardFlags == ForwardPipelineKey::kForwardTreeLod,
		"the TreeInstances family's lane slot is not ForwardPipelineKey::kForwardTreeLod");
	static_assert(DrawFamilyLaneSlotOf(Api::DrawFamily::Grass).forwardFlags == 0, "the Grass family has no faces, so no forward key flags");
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
	 * @brief The pipeline lane's results: immutable, published whole after each pass that changed it, latest-wins (TakeCatalog).
	 *
	 * A set index is handed out only once a published set version holds its pipeline, and setVersion holds every index here, so
	 * indices and set agree by construction; the frame binds this version (GetIndirectState). Likewise the shadow set. The draw
	 * families' and forward pipelines are in no set: the lane keeps them for the process (CatalogFamilyPipelines, ForwardPipelineOf).
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
		// Every key the lane has requested a build for or found failed (a key waiting on its program or an input is absent).
		ankerl::unordered_dense::map<PipelineKey, Entry, PipelineKeyHash> entries;
		std::uint32_t generation = 0;         // a target change starts a new set; every earlier index is stale
		std::uint32_t targetsGeneration = 0;  // DrawPipelines::Generation the set is built for
		std::uint64_t revision = 0;           // new with each publication
		std::shared_ptr<const void> setVersion;  // holds every setIndex above (DrawPipelinesImpl.h, SetVersion)

		struct ShadowEntry
		{
			std::uint32_t setIndex = kNone;  // kNone while its build is pending, and for good once failed
			bool failed = false;
		};
		// Every shadow view key the lane has requested a build for or found failed.
		ankerl::unordered_dense::map<ShadowPipelineKey, ShadowEntry, ShadowPipelineKeyHash> shadowEntries;
		std::uint32_t shadowGeneration = 0;                 // the shadow set's: a format change starts a new set
		DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;     // the shadow map format the set is built for
		std::shared_ptr<const void> shadowSetVersion;       // holds every setIndex of shadowEntries

		// By draw family: its pipelines (depth and colour ones) for targetsGeneration: built (FamilyBuilt) or failed (program: for good;
		// build: until the next target change). Neither while waiting.
		struct FamilyEntry
		{
			std::shared_ptr<const void> pipelines;
			bool failed = false;
		};
		std::array<FamilyEntry, kDrawFamilyLanes> families{};
		// The refraction pipeline for targetsGeneration and its normals format: built (an rhi::PipelinePtr) or failed.
		struct RefractionEntry
		{
			std::shared_ptr<const void> pipeline;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			bool failed = false;
		};
		RefractionEntry refraction;
		struct ForwardEntry
		{
			std::shared_ptr<const void> pipeline;  // once built (an rhi::PipelinePtr)
			bool program = false;                  // its program is compiled
			bool failed = false;                   // its program or its build failed, for good
		};
		// Every forward key whose program the lane has or found failed, built for its view's targets (by Api::EngineView).
		ankerl::unordered_dense::map<ForwardPipelineKey, ForwardEntry, ForwardPipelineKeyHash> forwardEntries;
		std::array<ForwardTargets, static_cast<std::size_t>(Api::EngineView::Count)> viewTargets{};

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
	 * @brief Graphics pipelines of the DCLF indirect draws and the indirect pipeline sets (Vulkan indirect execution sets) the draws
	 * select them from.
	 *
	 * Main pipelines share one layout (kDrawPushWords + kFramePushWords) and are built asynchronously through PipelineService from a
	 * key and its SPIR-V program: no vertex input (the vertex stages pull their attributes, decoded as the engine's input layout has
	 * them: VertexInput.h), the native main pass's fixed-function state and targets. A program using a register the layout cannot
	 * supply, or an attribute the key's vertex layout lacks, fails; its objects stay native. Ready pipelines get the next set index
	 * (DrawInput::pipelineIndex) and a place in their state class's sets (kSetClassShift: the index a sequence's pipelineIndex holds), one
	 * execution set per class and variant, since a set's pipelines may differ in their shaders alone.
	 *
	 * One owner, the pipeline lane (BuildExecutor.h): it takes requests, programs and build completions, admits them, publishes set
	 * versions and then the catalog. Every pipeline it admits is registered for shader hot reload (RenderGraphRuntime::PipelineRegistry;
	 * dclf-hot-reload.md); a reload's swapped payloads are adopted into the sets and the catalog (AdoptLivePipelines). Render-thread
	 * state reaches it only as frame inputs (SetTargetFormats, CaptureEngineStates,
	 * SetShadowInputs, ShadowRasterStateId, SetFamilyInputs, SetViewForwardTargets, ...). The render thread only posts and reads its
	 * frame's catalog (FrameCatalog).
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
			// Distinct Z-prepass pipelines (ZPipelineKey), and those without a pixel stage.
			std::uint32_t zPipelines = 0, zDepthOnly = 0;
			// The main set's state classes (one indirect execution set and signature per variant each), and the most pipelines one holds.
			std::uint32_t stateClasses = 0, largestClass = 0;
			// CS_DCLF_SET_PARITY: the published versions' class checks (each member against its class: CheckClasses), and those that failed.
			std::uint64_t classParityChecks = 0, classParityMismatches = 0;
			std::uint32_t targetChanges = 0;
			std::uint32_t shadowRequested = 0;
			std::uint32_t shadowReady = 0;
			std::uint32_t shadowFailed = 0;
			// Set versions published, and admissions that waited because every version was still held.
			std::uint32_t setPublishes = 0, setWaits = 0, shadowSetPublishes = 0, shadowSetWaits = 0;
			// By draw family, and the forward pipelines: built this target generation, and frames that wanted one not yet in their catalog
			// (NoteFamilyWait, NoteForwardWait: they draw natively meanwhile). A family's label: its reports' (Extensions::DrawFamilies';
			// null: not taken).
			struct DrawFamilyCounters
			{
				const char* label = nullptr;
				std::uint32_t requested = 0, ready = 0, failed = 0, waits = 0;
			};
			std::array<DrawFamilyCounters, kDrawFamilyLanes> drawFamilies{};
			std::uint32_t forwardRequested = 0, forwardReady = 0, forwardFailed = 0;
			std::uint32_t forwardWaits = 0;
			// By program family: its keys (KeyFamily; the Effect port's kRasterEffect) among requested, ready and failed.
			struct Family
			{
				std::uint32_t requested = 0, ready = 0, failed = 0;
			};
			std::array<Family, Extensions::kProgramFamilies> families{};
		};

		static DrawPipelines& Get();

		bool Enabled() const;

		/** @brief Render thread: the native main pass's targets; the lane rebuilds pipelines when they change. */
		void SetTargetFormats(const TargetFormats& a_formats);
		bool HasTargetFormats() const { return targets.colorCount != 0; }
		const TargetFormats& Targets() const { return targets; }

		/**
		 * @brief Any thread: asks the lane for the key's program and pipeline. Once per key: the lane keeps every key and rebuilds
		 * after a target change. a_slot is for the on-demand warnings.
		 */
		void RequestLighting(const PipelineKey& a_key, std::uint32_t a_slot);
		/** @brief Scene lane only (ResolveLookups): the newest catalog since the last call, null if none newer (keep the last). */
		std::shared_ptr<const PipelineCatalog> TakeCatalog();
		/**
		 * @brief Render thread, frame start (HandOverAtFrameStart): holds the catalog the installed publication's lookups were resolved
		 * from, for the frame (FrameCatalog, FrameIndirectState, FrameShadowIndirectState). A revision names its catalog explicitly.
		 */
		void HoldCatalog(std::shared_ptr<const PipelineCatalog> a_catalog) { heldCatalog = std::move(a_catalog); }
		/** @brief The frame's catalog (HoldCatalog), null before the first. */
		const std::shared_ptr<const PipelineCatalog>& HeldCatalog() const { return heldCatalog; }

		/**
		 * @brief Any thread: asks the lane for a shadow view key's pipeline (arrives in PipelineCatalog::shadowEntries). Once per key:
		 * rebuilt after a format change. a_casterKey and a_occlusion are for the on-demand warnings. A separate set: depth only, in
		 * the shadow map format, with the view's rasterizer state (ShadowRasterStateId).
		 */
		void RequestShadow(const ShadowPipelineKey& a_viewKey, const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion);
		/** @brief Render thread: the shadow map DSV format (a change rebuilds the shadow set) and the Utility shader. Posted on change. */
		void SetShadowInputs(DXGI_FORMAT a_format, RE::BSShader& a_utility);

		/**
		 * @brief Render thread: the id (1 and up) of a shadow view's rasterizer state, registering it the first time; 0 if a pipeline
		 * cannot express it (no depth clipping, wireframe). Equal states share an id and pipelines.
		 *
		 * The state must be read while the view is drawn (CaptureShadowView): ShadowmapCascadeRasterizerFix swaps in per-cascade
		 * copies with their own depth bias for exactly that window, and the volumetric copy draws without culling. A new state reaches
		 * the lane with the frame's inputs. a_renderMode is for the log.
		 */
		std::uint32_t ShadowRasterStateId(const D3D11_RASTERIZER_DESC& a_desc, std::uint32_t a_renderMode);

		/** @brief How many shadow view rasterizer states are registered (ids 1 to this). */
		std::uint32_t ShadowRasterStateCount() const;

		/**
		 * @brief Render thread: asks for a draw family's two pulled pipelines (Api::DrawFamilyBuild::DepthThenEqualColour; tree LOD's:
		 * dclf-architecture.md, "Tree LOD: the draws"): Z-prepass (its depth program, LESS with writes) and colour (its colour program, EQUAL, opaque write mode 1), both
		 * two-sided as the engine's. Once; rebuilt after a target change. Arrives in PipelineCatalog::families once SetFamilyInputs,
		 * targets, winding and write mode are in.
		 */
		void RequestFamily(Api::DrawFamily a_family);
		/** @brief Render thread: the engine shader a draw family's programs are of (its programSource: the DistantTree shader; frame input; posted on change). */
		void SetFamilyInputs(Api::DrawFamily a_family, RE::BSShader& a_shader);
		/**
		 * @brief Render thread: asks for the refraction pipeline: Utility's pulled RENDER_NORMAL build (kRefractionTechnique) under the
		 * shadow views' layout, one colour attachment with the engine's write mode 2 mask, main depth LESS_EQUAL without writes
		 * (FUN_1414b4860's depth mode 1), no culling (the pixel stage drops one-sided back faces). Once; rebuilt after a target or
		 * format change. Arrives in PipelineCatalog::refraction once SetRefractionInputs has posted.
		 */
		void RequestRefraction();
		/** @brief Render thread: Utility shader, kREFRACTION_NORMALS format, write mode 2's RT0 write mask (posted on change). */
		void SetRefractionInputs(RE::BSShader& a_utility, DXGI_FORMAT a_format, std::uint8_t a_writeMask);
		/**
		 * @brief Render thread: asks for a forward pipeline: pulled stages, depth LESS_EQUAL with writes, the key's cull, engine winding,
		 * no blending. Repeats are dropped. Arrives in PipelineCatalog::forwardEntries.
		 */
		void RequestForward(const ForwardPipelineKey& a_key);
		/**
		 * @brief Render thread: a view's forward targets, its keys' (Extensions::ForwardKeyView; frame input; a change rebuilds every
		 * forward pipeline).
		 */
		void SetViewForwardTargets(Api::EngineView a_view, const ForwardTargets& a_targets);
		/** @brief Render thread: counts a frame whose draw family waited for its pipelines. */
		void NoteFamilyWait(Api::DrawFamily a_family, bool a_waited) { stats.drawFamilies[static_cast<std::size_t>(a_family)].waits += a_waited ? 1u : 0u; }
		/** @brief Render thread: counts a frame whose forward view waited for a pipeline. */
		void NoteForwardWait(bool a_waited) { stats.forwardWaits += a_waited ? 1u : 0u; }

		/**
		 * @brief Render thread: reads the engine's rasterizer/blend states the lane asked for (PipelineRasterFlags), the opaque write
		 * modes and the winding, and hands what is new to the lane with the frame's inputs.
		 *
		 * Call inside the deferred pass: Community Shaders swaps the blend table for deferred variants between StartDeferred and
		 * ResetBlendStates; outside that window the same index names the forward state. A key whose state is not captured yet is not
		 * ready; nothing is guessed.
		 */
		void CaptureEngineStates();


		/** @brief Render thread: increments whenever the targets change. */
		std::uint32_t Generation() const { return generation; }
		/** @brief Render thread: the shadow map format last posted, which the catalog's shadow set must match. Revisions read their copy. */
		DXGI_FORMAT ShadowFormat() const;

		/** @brief Any thread: render-thread and lane counters (relaxed reads, for the report). */
		Stats GetStats() const;

		/** @brief Lighting constant variable names by index, per stage. */
		static std::span<const std::string_view> LightingVariableNames(bool a_pixel);

	private:
		DrawPipelines();
		~DrawPipelines();

		struct Impl;
		std::unique_ptr<Impl> impl;
		TargetFormats targets;
		std::shared_ptr<const PipelineCatalog> heldCatalog;          // the frame's (HoldCatalog)
		std::uint32_t generation = 0;
		Stats stats;

		friend struct IndirectState GetIndirectState(const PipelineCatalog&, std::uint32_t);
		friend struct ForwardPipelineAccess;  // CatalogFamilyPipelines (DrawPipelinesRhi.h)
		friend struct ShadowIndirectState GetShadowIndirectState(const PipelineCatalog&, DXGI_FORMAT);
	};

	/**
	 * @brief Render thread: the frame's catalog (HoldCatalog at HandOverAtFrameStart), which the draw families, forward views,
	 * FrameIndirectState and FrameShadowIndirectState all resolve from.
	 */
	inline const PipelineCatalog* FrameCatalog()
	{
		return DrawPipelines::Get().HeldCatalog().get();
	}
}
