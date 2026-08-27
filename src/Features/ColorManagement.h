#pragma once

#include "RE/N/NiColor.h"

#include <cstdint>

namespace ColorManagement
{
	enum class Encoding : std::uint8_t
	{
		SRGB,
		Linear,
		GameGamma
	};

	enum class Gamut : std::uint8_t
	{
		SRGB,
		Working
	};

	struct ColorSpace
	{
		Encoding encoding = Encoding::SRGB;
		Gamut gamut = Gamut::SRGB;
	};

	struct ColorValue
	{
		RE::NiColor value;
		ColorSpace space;
	};

	inline constexpr ColorSpace LinearSRGB{ Encoding::Linear, Gamut::SRGB };
	inline constexpr ColorSpace LinearWorking{ Encoding::Linear, Gamut::Working };
}
