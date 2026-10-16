#pragma once

#include <array>
#include <cstdint>

struct ID3D11ShaderResourceView;

namespace RE
{
	class NiSourceTexture;
}

namespace DCLF::MaterialPort
{
	/** @brief Render target views HookFrame keeps (RE::RENDER_TARGETS::kTOTAL fits: checked in MaterialHookFrame.cpp). */
	inline constexpr std::uint32_t kHookRenderTargets = 128;

	/**
	 * @brief The per-frame engine sources Community Shaders' SetupMaterial hooks read (the material contributors, through
	 * MaterialContributorFrame::hook and MaterialContributorEvaluation::hook) and Core's FrameCharacterLightView, sampled by the render
	 * thread at the frame's start (SampleHookFrame, MaterialHookFrame.cpp). A contributor's own frame state is its sample's
	 * (Extensions::MaterialContributorSamples), not this.
	 *
	 * Core/Extensions/Materials.h forward-declares it by this name: a struct, never an alias.
	 */
	struct HookFrame
	{
		// SetupTechniqueDescriptor's bytes (FrameGlobals): the technique +0x94 holds, which TruePBR's hook branches on.
		std::uint8_t techniqueByte12 = 0;
		std::uint8_t techniqueByte7 = 0;

		// TruePBR's TexcoordOffset: BSShaderManager::State::textureTransformCurrentBuffer (unmasked, as the hook indexes with it).
		std::uint32_t textureTransformBuffer = 0;

		// TruePBR's CharacterLight: the character light image space texture's render target (-1: none) and smState's params.
		std::int32_t characterLightTarget = -1;
		bool characterLightEnabled = false;
		std::array<float, 4> characterLightParams{};

		// Renderer render target views (TruePBR's diffuseRenderTargetSourceIndex and the character light's t11).
		std::array<ID3D11ShaderResourceView*, kHookRenderTargets> renderTargetViews{};

		// BSGraphics::State's default textures: the NiSourceTexture pointers (compared with material fields) and their views.
		const void* defaultBlack = nullptr;
		const void* defaultWhite = nullptr;
		const void* defaultNormalMap = nullptr;
		ID3D11ShaderResourceView* defaultBlackView = nullptr;
		ID3D11ShaderResourceView* defaultWhiteView = nullptr;
		ID3D11ShaderResourceView* defaultNormalMapView = nullptr;
	};

	/** @brief Render thread, the frame's start. */
	HookFrame SampleHookFrame();

	/** @brief Render target a_index's view in a_frame, or null (TruePBR's diffuseRenderTargetSourceIndex and character light). */
	inline ID3D11ShaderResourceView* RenderTargetView(const HookFrame& a_frame, std::int32_t a_index)
	{
		// The engine indexes renderTargets unchecked; an index past the array has no defined view.
		return a_index >= 0 && static_cast<std::uint32_t>(a_index) < kHookRenderTargets ? a_frame.renderTargetViews[a_index] : nullptr;
	}

	/** @brief A texture's view as the hooks resolve it (NiSourceTexture -> rendererTexture -> resourceView), or null. */
	ID3D11ShaderResourceView* ViewOfTexture(const RE::NiSourceTexture* a_texture);
}
