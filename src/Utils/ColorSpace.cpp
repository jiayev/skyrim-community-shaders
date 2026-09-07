#include "ColorSpace.h"

#include <cmath>

namespace Util::ColorSpace
{
	float SRGBToLinear(float encodedSRGB)
	{
		const float magnitude = std::abs(encodedSRGB);
		const float linearSRGB = magnitude <= 0.04045f ? magnitude / 12.92f : std::pow((magnitude + 0.055f) / 1.055f, 2.4f);
		return std::copysign(linearSRGB, encodedSRGB);
	}

	float LinearToSRGB(float linearValue)
	{
		const float magnitude = std::abs(linearValue);
		const float encoded = magnitude <= 0.0031308f ? magnitude * 12.92f : 1.055f * std::pow(magnitude, 1.0f / 2.4f) - 0.055f;
		return std::copysign(encoded, linearValue);
	}

	float GameGammaToLinear(float encoded)
	{
		return std::copysign(std::pow(std::abs(encoded), GAME_GAMMA), encoded);
	}

	float LinearToGameGamma(float linearValue)
	{
		return std::copysign(std::pow(std::abs(linearValue), GAME_GAMMA_INV), linearValue);
	}

	void SRGBToLinear(float* a_rgb3)
	{
		for (int i = 0; i < 3; ++i) {
			if (std::isfinite(a_rgb3[i]))
				a_rgb3[i] = SRGBToLinear(a_rgb3[i]);
		}
	}

	void LinearToSRGB(float* a_rgb3)
	{
		for (int i = 0; i < 3; ++i) {
			if (std::isfinite(a_rgb3[i]))
				a_rgb3[i] = LinearToSRGB(a_rgb3[i]);
		}
	}

	void GameGammaToLinear(float* a_rgb3)
	{
		for (int i = 0; i < 3; ++i) {
			if (std::isfinite(a_rgb3[i]))
				a_rgb3[i] = GameGammaToLinear(a_rgb3[i]);
		}
	}

	void SRGBGamutToAP1(float* a_rgb3)
	{
		const float x = 0.4123907993f * a_rgb3[0] + 0.3575843394f * a_rgb3[1] + 0.1804807884f * a_rgb3[2];
		const float y = 0.2126390059f * a_rgb3[0] + 0.7151686788f * a_rgb3[1] + 0.0721923154f * a_rgb3[2];
		const float z = 0.0193308187f * a_rgb3[0] + 0.1191947798f * a_rgb3[1] + 0.9505321522f * a_rgb3[2];

		a_rgb3[0] = 1.6410233797f * x - 0.3248032942f * y - 0.2364246952f * z;
		a_rgb3[1] = -0.6636628587f * x + 1.6153315917f * y + 0.0167563477f * z;
		a_rgb3[2] = 0.0117218943f * x - 0.0082844420f * y + 0.9883948585f * z;
	}
}
