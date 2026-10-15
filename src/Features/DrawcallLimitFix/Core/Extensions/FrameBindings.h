#pragma once

// The frame binding contract's Core side (Api/FrameBindings.h): what a registered binding's select is handed, the values the frame
// records take from the bindings, and the texture registers Core keeps for itself (which no binding or frame texture may take). And the
// frame textures' (Api::FrameTexture): their registers' indices.

#include "Features/DrawcallLimitFix/Api/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

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
	/** @brief A binding's select (render thread or snapshot builder): the scene's tables and the frame; select answers with buffer. */
	class FrameBindingSelect
	{
	public:
		const Draws::SceneBuffers& scene;
		std::uint32_t frame;                                            // the frame whose draws read it
		const std::shared_ptr<org::VersionedBuffer>* buffer = nullptr;  // the answer: a buffer of the scene's set (null: none)
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
	 * @brief Whether Core keeps a_register: the material rows' pixel textures, the features' material registers, and the frame record's
	 * own buffers and textures (DrawPipelines.h). A binding or a frame texture may take none of them (Features/Manifest.cpp checks the
	 * features' registers against it at compile time, the install at registration).
	 */
	inline constexpr bool CoreFrameRegister(std::uint32_t a_register)
	{
		return a_register < kPixelTextureSlots || FeatureMaterialSlot(a_register) >= 0 || a_register == kObjectBufferRegister || a_register == kExtrasBufferRegister ||
		       a_register == kCharacterLightRegister || a_register == kPlacementBufferRegister || a_register == kPaletteBufferRegister ||
		       a_register == kShadingBufferRegister || a_register == kFadeStatesRegister || a_register == kFadeRootsRegister;
	}

	/** @brief The descriptor of every binding's buffer for a_frame, as a_sealed names it, into a_out (render thread or snapshot builder). */
	void FrameBindingValuesOf(const org::services::NamedVersions& a_sealed, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame, FrameBindingValues& a_out);
	/** @brief a_values (FrameBindingValuesOf) into a frame record's texture registers (DrawBindings::textures). */
	void WriteFrameBindings(const FrameBindingValues& a_values, std::span<std::uint32_t> a_textures);
	/** @brief Whether a_register is a binding's (a build counts it among the buffers, not the captured frame textures). */
	bool FrameBindingRegister(std::uint32_t a_register);
	/** @brief Binding a_binding's buffer for a_frame (its select's answer; null: none), for the named-address parity. */
	const std::shared_ptr<org::VersionedBuffer>* FrameBindingBuffer(std::size_t a_binding, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame);

	/**
	 * @brief The most frame textures there may be: a colour commit samples their views into a fixed array (ResolveFrameTextures). A
	 * texture past them fails the install: grow this first.
	 */
	inline constexpr std::size_t kFrameTextureSlots = 4;
	/** @brief A register no frame texture has (FrameTextureIndices). */
	inline constexpr std::uint8_t kNoFrameTexture = 0xFF;
	/** @brief Any thread, after InstallFeatures: by texture register, its frame texture's index (RegisteredFrameTextures), or kNoFrameTexture. */
	std::span<const std::uint8_t> FrameTextureIndices();
}
