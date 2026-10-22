#pragma once

// The frame binding contract (Api::InstallContext::RegisterFrameBinding): a buffer every DCLF draw reads at a declared texture register,
// given by the frame record (DrawBindings) because it changes by frame: the version of a feature's buffer the frame's draws read. And the
// frame texture contract (RegisterFrameTexture): an engine render target's view the frame record names at a declared texture register.
// And the frame graph buffers' (RegisterFrameGraphBuffers): another CS feature's graph buffers, read at declared texture registers. And
// the per-draw buffers' (DeclarePerDrawBuffer): another CS feature's constant buffers bound per draw, which no frame block mirrors.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/FrameBindings.h), which the features inside DCLF include for it.
	class FrameBindingSelect;

	/**
	 * @brief A frame binding.
	 *
	 * The register is the feature's compile-time constant, with its HLSL declaration; Features/Manifest.cpp checks that no two
	 * registers overlap, and the install that none is Core's or another binding's.
	 *
	 * - select: the render thread (the main and shadow commits' frame records) or the snapshot builder (the addresses a build embeds),
	 *   for a frame: the versioned buffer whose named version's descriptor the frame's draws read at the register (null: the register
	 *   keeps what the record gave it). It reads only handles made at the scene's setup, and the frame: the binding may alternate by
	 *   frame (the named-address parity takes either of two consecutive frames' as right). On the render thread (a frame record's
	 *   commit) it may answer with a descriptor index instead (a frame value pool's backing), which the builder leaves to the commit.
	 */
	struct FrameBinding
	{
		std::string_view name;  // the named-address parity's name for it
		std::uint32_t textureRegister = ~0u;
		void (*select)(FrameBindingSelect& a_select) = nullptr;
	};

	/**
	 * @brief A frame texture (Api::InstallContext::RegisterFrameTexture): a view every DCLF colour draw reads at a declared texture
	 * register, which the frame record names because it is a render target or a CS feature's own per-frame texture (a material row never
	 * holds one): Effects' soft depth, Integrations/TerrainBlending's mask, and the CS features' views (Integrations/VolumetricShadows,
	 * CloudShadows, ExponentialHeightFog, DynamicCubemaps, Skylighting, TerrainShadows, WaterEffects, IBL), which also give a view with
	 * frame constants of its own (the reflection's faces) its sources of this frame's.
	 *
	 * The register is the feature's compile-time constant, with its HLSL declaration; Features/Manifest.cpp checks it against Core's and the
	 * frame bindings', the install against those and the other frame textures'.
	 *
	 * - view: render thread, once per colour commit before its frame textures are resolved, and once per epoch of a view with frame
	 *   constants of its own: the shader resource view (ID3D11ShaderResourceView*) the register reads (null: the null texture, or with
	 *   capturedWhenNull in a colour commit the captured view).
	 * - capturedWhenNull: a null view leaves the register the view the colour commit captured there (a texture the feature names only
	 *   while it binds it itself after the epoch, and the engine's or CS's binding stands otherwise: Integrations/TerrainBlending's mask;
	 *   a CS feature's view the integration names only while it is the one CS binds there).
	 */
	struct FrameTexture
	{
		std::string_view name;
		std::uint32_t textureRegister = ~0u;
		void* (*view)() = nullptr;
		bool capturedWhenNull = false;
	};

	/**
	 * @brief Frame graph buffers (Api::InstallContext::RegisterFrameGraphBuffers): another CS feature's buffers on the render graph, which
	 * every DCLF colour draw reads at consecutive texture registers from firstRegister (Integrations/LightLimitFix's lights, light index
	 * list and light grid, t35-t37), named by the frame record by their bindless indices.
	 *
	 * The registers are the feature's compile-time constants, with their HLSL declarations; Features/Manifest.cpp checks them against Core's,
	 * the frame bindings' and the frame textures', the install against those and the other graph buffers'.
	 *
	 * - name: the registration's (the install's failures).
	 * - firstRegister: the first's texture register; the others follow it.
	 * - resources: the graph resource ids the colour passes declare (read), one a register: the leading non-empty ones count.
	 * - indices: render thread, at the main resources' check (whether they are on the graph now: false, and the registers keep the frame
	 *   buffers the commit captured there) and once per colour commit: each resource's bindless index into a_out (one a resource), and
	 *   whether they are registered.
	 */
	struct FrameGraphBuffers
	{
		std::string_view name;
		std::uint32_t firstRegister = ~0u;
		std::array<std::string_view, 4> resources{};
		bool (*indices)(std::span<std::uint32_t> a_out) = nullptr;
	};

	/** @brief A shader stage a per-draw buffer is bound to. */
	enum class ShaderStage : std::uint8_t
	{
		Vertex,
		Pixel,
	};

	/**
	 * @brief A per-draw buffer (Api::InstallContext::DeclarePerDrawBuffer): a constant buffer another CS feature binds per draw at a declared
	 * register of a stage (Integrations/LinearLighting's LLPerGeometry, PS b8; Integrations/AdvancedSkin's SkinPerGeometry, PS b7;
	 * Integrations/LightLimitFix's, PS b3): what the main pass has bound there is the last drawn object's, never the frame's, so the main
	 * pass's frame blocks do not mirror it (DCLF's draws take such values per object, from their records and rows), and the capture checks
	 * leave it out (Extensions::PerDrawMask). Declared only: nothing is called.
	 *
	 * The register is the feature's compile-time constant, with its HLSL declaration; Features/Manifest.cpp checks it against Core's and the
	 * others', the install against those (Extensions::CorePerDrawMask) and the other declarations.
	 *
	 * - name: the install's failures' name for it.
	 * - stage, constantRegister: where it is bound (below D3D11's 14 constant buffer slots).
	 */
	struct PerDrawBuffer
	{
		std::string_view name;
		ShaderStage stage = ShaderStage::Pixel;
		std::uint32_t constantRegister = ~0u;
	};
}
