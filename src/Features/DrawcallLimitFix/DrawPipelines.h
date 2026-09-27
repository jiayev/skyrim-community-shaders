#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <dxgiformat.h>
#include <memory>
#include <span>
#include <string>
#include <bit>
#include <vector>

#include "Records.h"
#include "ShaderPrograms.h"

namespace DCLF
{
	/** @brief Constant buffer registers b0-b13 (each stage has its own, as in D3D11). */
	inline constexpr std::uint32_t kConstantBufferRegisters = 14;
	/**
	 * @brief Frame push (CS_DCLF_FRAME_PUSH, default on, =0 off): the registers whose block is the same for every draw of a pass under bindless
	 * draws (the frame slots, the shared light block at PS b3, the frame lighting at PS b13) read their buffers' addresses from
	 * push data (LayoutRangeSource::PushAddress), set once per pass, instead of from each draw's binding record
	 * (IndirectAddress), which saves the shaders the record's indirection (colour pass 3.3 -> 2.1 ms at Riverwood). Push data: the record address
	 * range (4 words, for alignment), then these addresses, the vertex stage's registers ascending and then the pixel stage's.
	 * The shadow views keep every register in the binding record, in a layout of their own (their pass-wide blocks are per view and
	 * per build, not the main pass's frame slots).
	 */
	inline constexpr std::uint32_t kFramePushVS = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 11) | (1u << 12) | (1u << 13);
	inline constexpr std::uint32_t kFramePushPS = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13);
	inline constexpr std::uint32_t kFramePushBinding = 191;
	inline constexpr std::uint32_t kFramePushWords = 2 * (std::popcount(kFramePushVS) + std::popcount(kFramePushPS));
	bool FramePushEnabled();
	/**
	 * @brief CS_DCLF_DGC_PREPROCESS (default on, =0 off): every DCLF draw signature (colour, depth, shadow) is preprocessed
	 * explicitly, before the passes that execute it (CommandList::PreprocessIndirect), instead of by the driver inside each call.
	 */
	bool DgcPreprocessEnabled();
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
	/** @brief t126: the epoch's bone palette rows (DCLFBones), the other vertex-stage register. */
	inline constexpr std::uint32_t kBonesBufferRegister = kTextureRegisters - 2;

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
	 * execution set) the draws select them from (Phase 2).
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
		std::uint32_t Find(const PipelineKey& a_key, const ShaderPrograms::Program& a_program);

		/**
		 * @brief The shadow key's index in the shadow pipeline set, or kNotReady.
		 *
		 * A separate set from the main pass's: its pipelines write depth alone, into the engine's shadow
		 * map format, with the rasterizer state of the view (the key's RasterShadowState, which must be
		 * registered) and no colour attachment (engine notes: shadow maps).
		 * @param a_depthFormat the shadow map array's format; pipelines are rebuilt if it changes.
		 */
		std::uint32_t FindShadow(const ShadowPipelineKey& a_key, const ShaderPrograms::ShadowProgram& a_program, DXGI_FORMAT a_depthFormat);

		/** @brief How many distinct shadow view rasterizer states a key can name (1 to this). */
		static constexpr std::uint32_t kMaxShadowRasterStates = 15;

		/**
		 * @brief The id (1 to kMaxShadowRasterStates) of a shadow view's rasterizer state, registering it the
		 * first time; 0 when the registry is full or the state has something a pipeline cannot express (no
		 * depth clipping, wireframe). Views with equal states share the id and so their pipelines.
		 *
		 * The state is the one the engine binds for the view, read from its table while the view is drawn
		 * (IndirectDraws::ExecuteShadowView): Community Shaders' ShadowmapCascadeRasterizerFix swaps in
		 * per-cascade copies with their own depth bias for exactly that window, and the volumetric copy draws
		 * without culling. Remembers the render modes each id was seen in, for ShadowRasterStateModes.
		 */
		std::uint32_t ShadowRasterStateId(const D3D11_RASTERIZER_DESC& a_desc, std::uint32_t a_renderMode);

		/** @brief The registered ids seen with a render mode, as a mask (bit id). */
		std::uint32_t ShadowRasterStatesOfMode(std::uint32_t a_renderMode) const;

		/** @brief Adds finished pipelines to the set (call once per frame). */
		void Update();

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
		std::uint32_t generation = 0;
		Stats stats;

		friend struct IndirectState GetIndirectState();
		friend struct ShadowIndirectState GetShadowIndirectState();
	};
}
