#pragma once

#include <array>
#include <cstdint>

struct ID3D11ShaderResourceView;

namespace DCLF::MaterialPort
{
	/**
	 * @brief Render target slots SetupMaterial can index (the renderer's array, SRVs at 0x143289230 + 0x30 * index): 116 in AE
	 * (CommonLib's RENDER_TARGETS::kTOTAL comment). t0 takes a material's own index (+0x50), t11 the character light's.
	 */
	inline constexpr std::uint32_t kVanillaRenderTargets = 116;

	/**
	 * @brief The per-frame sources vanilla SetupMaterial reads (the S, G and R rows of the engine notes' table), sampled by the render
	 * thread at the frame's start (SampleVanillaFrame, MaterialPortVanilla.cpp). Floats are the engine's bits, copied, never computed.
	 */
	struct VanillaFrame
	{
		bool sampled = false;

		// G: SetupTechnique's technique remap (SetupTechniqueDescriptor), so EvaluateVanilla sees the +0x94 the draw has: 0x12 ->
		// 9 unless the byte at 0x142032fdb is set, 7 -> 0 unless the byte at 0x142035500 is set.
		std::uint8_t techniqueByte12 = 0;
		std::uint8_t techniqueByte7 = 0;

		// G: the texture-transform buffer the frame reads (0x142033180, a dword, flipped by Main::Update): VS 11's pair.
		std::uint32_t textureTransformBuffer = 0;

		// G: LODTexParams.z (MTLand, LODLand): 1 when the byte at 0x142032fda is set, else 0.
		std::uint8_t lodTexParamsZ = 0;
		// G: LandscapeTexture5to6IsSnow (MTLand with Snow): .z 1 when the byte at 0x142035548 is set; .w = 1 / (float)(int at 0x1420355f0).
		std::uint8_t landSnowZ = 0;
		std::int32_t landSnowDivisor = 0;

		// G: AmbientSpecularTintAndFresnelPower (PS 6, AmbientSpecular): 16 bytes at 0x14203315c. Not written when SSE Engine Fixes'
		// BSLightingAmbientSpecular fix NOPs the write out (SetupMaterial +0x8CF, 0x20 bytes; it writes PS 6 in SetupGeometry instead).
		std::array<float, 4> ambientSpecular{};
		bool ambientSpecularWritten = true;
		// G: SnowRimLightParameters (PS 34, Snow): 0x142035590, 0x1420355a8, 0x1420355c0; .w 1 when the byte at 0x1420355d8 is set.
		std::array<float, 3> snowRim{};
		std::uint8_t snowRimW = 0;
		// G: CharacterLightParams (PS 35, CharacterLight): 16 bytes at 0x14203316c while the full-bright byte (0x1420330a6) is set,
		// else zero.
		std::array<float, 4> characterLight{};
		std::uint8_t fullBright = 0;

		// S: IBLParams (PS 29, always): x = shader +0xcc; yzw = shader +0xd0..+0xd8 when the byte at +0xf0 is set, else
		// +0xe0..+0xe8 (resolved here). False: the lighting shader is not known yet (EvaluateVanilla then fails).
		bool shaderKnown = false;
		std::array<float, 4> ibl{};

		// R: the character light's t11 (CharacterLight): the render target index at 0x142033db0 (FUN_1414e8b30 returns
		// 0x142033da8 +8), bound only while it is not negative (-1 while a cell loads); it alternates frame to frame.
		std::int32_t characterLightTarget = -1;

		// G: the view t5 takes when an envmap material has no mask (NiSourceTexture__sub 1414e0710: the texture global at
		// 0x14328ccb8, a BSGraphics::State default texture).
		ID3D11ShaderResourceView* envMaskDefaultView = nullptr;

		// R: every render target's SRV (0x143289230 + 0x30 * index), for t0 (a material's +0x50) and t11.
		std::array<ID3D11ShaderResourceView*, kVanillaRenderTargets> renderTargetViews{};
	};

	/** @brief Render thread, the frame's start. */
	VanillaFrame SampleVanillaFrame();
}
