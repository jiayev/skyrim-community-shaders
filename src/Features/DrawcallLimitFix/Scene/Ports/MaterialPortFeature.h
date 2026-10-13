#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF::MaterialPort
{
	struct MaterialSnapshot;

	/** @brief Render target views FeatureFrame keeps (RE::RENDER_TARGETS::kTOTAL fits: checked in MaterialPortFeature.cpp). */
	inline constexpr std::uint32_t kFeatureRenderTargets = 128;

	/** @brief Advanced Skin's t71/t74 views for one material hash key (Skin::skinExtraTextures, resolved as MaterialTexturesOf does). */
	struct SkinTextures
	{
		std::uint32_t hashKey = 0;
		ID3D11ShaderResourceView* rfaos = nullptr;    // t71
		ID3D11ShaderResourceView* wetness = nullptr;  // t74
	};

	/**
	 * @brief The per-frame sources Community Shaders' SetupMaterial hooks read (TruePBR, Advanced Skin, TerrainHelper), sampled by
	 * the render thread at the frame's start (SampleFeatureFrame, MaterialPortFeature.cpp).
	 */
	struct FeatureFrame
	{
		// Which hooks are installed and run (Feature::loaded; the hooks are installed only for loaded features).
		bool truePBR = false;
		bool skin = false;

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
		std::array<ID3D11ShaderResourceView*, kFeatureRenderTargets> renderTargetViews{};

		// BSGraphics::State's default textures: the NiSourceTexture pointers (compared with material fields) and their views.
		const void* defaultBlack = nullptr;
		const void* defaultWhite = nullptr;
		const void* defaultNormalMap = nullptr;
		ID3D11ShaderResourceView* defaultBlackView = nullptr;
		ID3D11ShaderResourceView* defaultWhiteView = nullptr;
		ID3D11ShaderResourceView* defaultNormalMapView = nullptr;

		// Advanced Skin's per-hash-key textures, sorted by hashKey; shared between frames while unchanged. A key missing here is one
		// MaterialTexturesOf has not set up yet (SetupExtraTexture, render thread): FeatureHooksCovered is false for it.
		std::shared_ptr<const std::vector<SkinTextures>> skinTextures;
	};

	/** @brief Render thread, the frame's start. */
	FeatureFrame SampleFeatureFrame();

	/**
	 * @brief Whether TruePBR's hook takes the pass (TruePbr set and the technique +0x94 holds is not LODLand or LODLandNoise): then
	 * EvaluatePBR gives the record (false from it: not covered); otherwise vanilla SetupMaterial runs (EvaluateVanilla).
	 */
	bool PBRTakesPass(std::uint32_t a_passDescriptor, const FeatureFrame& a_frame);

	/** @brief Whether EvaluateFeatureHooks can give a_snapshot's featureTextures (false: Advanced Skin has not set its key up yet). */
	bool FeatureHooksCovered(const MaterialSnapshot& a_snapshot, const FeatureFrame& a_frame);

	/** @brief Bytes Capture takes of CS's PBR classes (sizeof BSLightingShaderMaterialPBR, or ...PBRLandscape for feature 19). */
	std::uint32_t PBRMaterialBytes(const RE::BSShaderMaterial& a_material);
}
