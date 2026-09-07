#pragma once

// MIRROR OF: package/Shaders/Common/TransferFunctions.hlsli
//            package/Shaders/Common/ColorSpaces.hlsli (sRGBToAP1)

namespace Util::ColorSpace
{
	inline constexpr float GAME_GAMMA = 1.6f;
	inline constexpr float GAME_GAMMA_INV = 1.0f / GAME_GAMMA;

	/** @brief sRGB EOTF：Encoded -> Linear. */
	float SRGBToLinear(float encodedSRGB);
	/** @brief sRGB OETF：Linear -> Encoded. */
	float LinearToSRGB(float linearValue);
	/** @brief In game gamma 1.6 -> Linear. */
	float GameGammaToLinear(float encoded);
	/** @brief Linear -> In game gamma 1.6. */
	float LinearToGameGamma(float linearValue);

	/** @brief Apply SRGBToLinear to RGB 3-component in-place. */
	void SRGBToLinear(float* a_rgb3);
	/** @brief Apply LinearToSRGB to RGB 3-component in-place. */
	void LinearToSRGB(float* a_rgb3);
	/** @brief Apply GameGammaToLinear to RGB 3-component in-place. */
	void GameGammaToLinear(float* a_rgb3);

	/** @brief Linear sRGB primaries -> Linear AP1(ACEScg) primaries, in-place.
	 *  The matrix is equivalent to the one in package/Shaders/Common/ColorSpaces.hlsli
	 *  (the result of combining sRGB_2_XYZ_MAT and XYZ_2_AP1_MAT). */
	void SRGBGamutToAP1(float* a_rgb3);
}
