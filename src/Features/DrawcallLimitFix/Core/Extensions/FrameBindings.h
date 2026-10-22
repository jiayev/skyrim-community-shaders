#pragma once

// The frame binding contract's Core side (Api/FrameBindings.h): what a registered binding's select is handed, the values the frame
// records take from the bindings, and the texture registers Core keeps for itself (which no binding or frame texture may take). And the
// frame textures' (Api::FrameTexture): their registers' indices. And the frame graph buffers' (Api::FrameGraphBuffers): their entries.
// And the per-draw buffers' (Api::PerDrawBuffer): the per-draw registers by stage, Core's and the declared ones (PerDrawMask).

#include "Features/DrawcallLimitFix/Api/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <tuple>

namespace org
{
	class VersionedBuffer;
	namespace services
	{
		class NamedVersions;
	}
}

namespace DCLF::Draws
{
	struct SceneBuffers;
}

namespace DCLF::Api
{
	/**
	 * @brief A binding's select (render thread or snapshot builder): the scene's tables and the frame; select answers with buffer, or on the
	 * render thread (a frame record's commit) with index instead.
	 */
	class FrameBindingSelect
	{
	public:
		const Draws::SceneBuffers& scene;
		std::uint32_t frame;                                            // the frame whose draws read it
		bool renderThread = false;                                      // a frame record's commit: index may answer
		const std::shared_ptr<org::VersionedBuffer>* buffer = nullptr;  // the answer: a buffer of the scene's set (null: none)
		std::uint32_t index = ~0u;  // or the descriptor the frame's draws read (a frame value pool's backing), render thread only
	};
}

namespace DCLF::Extensions
{
	/**
	 * @brief The most frame bindings there may be: a build embeds their values (Draws::ResourceAddresses::frameBindings). A binding past
	 * them fails the install: grow this first.
	 */
	inline constexpr std::size_t kFrameBindingSlots = 4;
	/** @brief A binding's value when its select gave no buffer: the register keeps what the record gave it. */
	inline constexpr std::uint32_t kNoFrameBinding = ~0u;
	/** @brief The bindings' values for one frame, by registration (Extensions::RegisteredFrameBindings). */
	using FrameBindingValues = std::array<std::uint32_t, kFrameBindingSlots>;

	/**
	 * @brief Whether Core keeps a_register: the material rows' pixel textures and the frame record's own buffers and textures
	 * (DrawPipelines.h). A binding, a frame texture or a material contributor's register may take none of them (Features/Manifest.cpp checks
	 * the features' registers against it at compile time, the install at registration). The material contributors' registers
	 * (Api::MaterialContributor::textureRegisters) are theirs, not Core's: the install refuses a binding, a frame texture or graph buffers on
	 * one, and one on theirs (Extensions::FeatureTextureSlotOf once resolved).
	 */
	inline constexpr bool CoreFrameRegister(std::uint32_t a_register)
	{
		return a_register < kPixelTextureSlots || a_register == kObjectBufferRegister || a_register == kExtrasBufferRegister ||
		       a_register == kCharacterLightRegister || a_register == kPlacementBufferRegister || a_register == kPaletteBufferRegister ||
		       a_register == kShadingBufferRegister;
	}

	/**
	 * @brief The descriptor of every binding's buffer for a_frame, as a_sealed names it, into a_out (render thread or snapshot builder); on
	 * the render thread (a_renderThread: a frame record's commit) a binding's index answer too, which the builder leaves to the commit.
	 */
	void FrameBindingValuesOf(const org::services::NamedVersions& a_sealed, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame, FrameBindingValues& a_out,
		bool a_renderThread);
	/** @brief a_values (FrameBindingValuesOf) into a frame record's texture registers (DrawBindings::textures). */
	void WriteFrameBindings(const FrameBindingValues& a_values, std::span<std::uint32_t> a_textures);
	/** @brief Whether a_register is a binding's (a build counts it among the buffers, not the captured frame textures). */
	bool FrameBindingRegister(std::uint32_t a_register);
	/** @brief Binding a_binding's buffer for a_frame (its select's answer; null: none), for the named-address parity. */
	const std::shared_ptr<org::VersionedBuffer>* FrameBindingBuffer(std::size_t a_binding, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame);

	/**
	 * @brief The most frame textures there may be: one a frame register (t16-t127), as no two share one, so a colour commit's fixed array
	 * of their views (ResolveFrameTextures) holds every registration there can be. The install still fails one past it.
	 */
	inline constexpr std::size_t kFrameTextureSlots = kTextureRegisters - kPixelTextureSlots;
	/** @brief A register no frame texture has (FrameTextureIndices). */
	inline constexpr std::uint8_t kNoFrameTexture = 0xFF;
	static_assert(kFrameTextureSlots < kNoFrameTexture, "a frame texture's index (FrameTextureIndices) would reach kNoFrameTexture");
	/** @brief Any thread, after InstallFeatures: by texture register, its frame texture's index (RegisteredFrameTextures), or kNoFrameTexture. */
	std::span<const std::uint8_t> FrameTextureIndices();

	/**
	 * @brief The most frame graph buffers registrations there may be (a commit keeps each one's indices in a fixed array), and each one's
	 * registers (Api::FrameGraphBuffers::resources). One past them fails the install: grow this first.
	 */
	inline constexpr std::size_t kFrameGraphBufferSlots = 2;
	inline constexpr std::size_t kFrameGraphBufferRegisters = std::tuple_size_v<decltype(Api::FrameGraphBuffers::resources)>;
	/** @brief Each registration's indices for one commit (FrameGraphBufferIndices), by registration then register. */
	using FrameGraphBufferValues = std::array<std::array<std::uint32_t, kFrameGraphBufferRegisters>, kFrameGraphBufferSlots>;
	/** @brief A registered frame graph buffers (its names owned): registers [firstRegister, firstRegister + count). */
	struct FrameGraphBuffersEntry
	{
		std::string name;
		std::uint32_t firstRegister = ~0u;
		std::uint32_t count = 0;                                            // the leading non-empty resources
		std::array<std::string, kFrameGraphBufferRegisters> resources;      // the graph ids, owned (c_str for ORG's ResourceIdentifier)
		decltype(Api::FrameGraphBuffers::indices) indices = nullptr;
	};
	/** @brief Any thread, after InstallFeatures (empty before): the registrations, in order. */
	std::span<const FrameGraphBuffersEntry> RegisteredFrameGraphBuffers();
	/** @brief Whether a_register is a registered frame graph buffer's (the captured frame buffers skip it while it is registered). */
	bool FrameGraphBufferRegister(std::uint32_t a_register);
	/**
	 * @brief Render thread (the main resources' check, each colour commit): registration a_buffers's indices into a_out (its indices),
	 * whether they are registered; false past the registrations.
	 */
	bool FrameGraphBufferIndices(std::size_t a_buffers, std::span<std::uint32_t, kFrameGraphBufferRegisters> a_out);

	/**
	 * @brief The constant buffers Core treats as bound per draw, by stage: the Lighting groups (b0-b2), the permutation (b4), VS b9 and b10,
	 * PS b11. The features' per-draw buffers (Api::PerDrawBuffer, below kConstantBufferRegisters) are added to them (PerDrawMask).
	 */
	inline constexpr std::uint32_t kCorePerDrawVS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 9) | (1u << 10);
	inline constexpr std::uint32_t kCorePerDrawPS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 11);
	/** @brief Core's per-draw registers of a_stage. */
	inline constexpr std::uint32_t CorePerDrawMask(Api::ShaderStage a_stage) { return a_stage == Api::ShaderStage::Vertex ? kCorePerDrawVS : kCorePerDrawPS; }
	/**
	 * @brief Any thread, after InstallFeatures (Core's alone before): a_stage's per-draw constant buffer registers, Core's and the declared
	 * per-draw buffers' (a bit by register): the main pass's frame blocks skip them (MainEpochs' MirrorFrameBlocks), and so do the capture
	 * checks (CapturePoint, CaptureParity).
	 */
	std::uint32_t PerDrawMask(Api::ShaderStage a_stage);
}
