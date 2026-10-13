#pragma once

namespace LegacyGraphicsCompatibility
{
	enum class ImageSpaceSelectorSource
	{
		kAlwaysAdjusted,
		kImageSpaceEffectUnk88
	};

	struct ImageSpaceSelectorTranslation
	{
		std::size_t floatIndex;
		ImageSpaceSelectorSource source;
	};

	[[nodiscard]] bool IsLegacyVersion() noexcept;

	[[nodiscard]] constexpr std::uint32_t NormalizeLegacyUtilityDescriptor(std::uint32_t a_descriptor) noexcept
	{
		constexpr std::uint32_t shadowMaskTechniques = 0x01E00000;
		constexpr std::uint32_t selectorMask = 0x001E0000;

		if ((a_descriptor & shadowMaskTechniques) == 0) {
			return a_descriptor;
		}

		auto selector = (a_descriptor >> 17) & 0x7;
		if (selector == 3) {
			selector = 4;
		}
		if (selector != 0 && selector != 1 && selector != 2 && selector != 4) {
			selector = 0;
		}

		return (a_descriptor & ~selectorMask) | (selector << 17);
	}

	static_assert(NormalizeLegacyUtilityDescriptor((1u << 21) | (3u << 17)) ==
				  ((1u << 21) | (4u << 17)));
	static_assert(NormalizeLegacyUtilityDescriptor(1u << 20) == (1u << 20));

	void BindLegacyGrassPerGeometryToPixelShader();

	[[nodiscard]] constexpr std::optional<ImageSpaceSelectorTranslation> GetLegacyImageSpaceSelectorTranslation(
		RE::ImageSpaceManager::ImageSpaceEffectEnum a_effect) noexcept
	{
		using enum RE::ImageSpaceManager::ImageSpaceEffectEnum;

		switch (a_effect) {
		case ISDoubleVision:
			return ImageSpaceSelectorTranslation{ 0, ImageSpaceSelectorSource::kAlwaysAdjusted };
		case ISDepthOfField:
		case ISDepthOfFieldFogged:
		case ISDepthOfFieldMaskedFogged:
		case ISDistantBlur:
		case ISDistantBlurFogged:
		case ISDistantBlurMaskedFogged:
			return ImageSpaceSelectorTranslation{ 2, ImageSpaceSelectorSource::kImageSpaceEffectUnk88 };
		case ISRadialBlur:
		case ISRadialBlurMedium:
		case ISRadialBlurHigh:
			return ImageSpaceSelectorTranslation{ 7, ImageSpaceSelectorSource::kImageSpaceEffectUnk88 };
		default:
			return std::nullopt;
		}
	}

	[[nodiscard]] constexpr float ResolveLegacyImageSpaceSelector(
		ImageSpaceSelectorTranslation a_translation,
		bool a_imageSpaceEffectUnk88) noexcept
	{
		if (a_translation.source == ImageSpaceSelectorSource::kAlwaysAdjusted || a_imageSpaceEffectUnk88) {
			return 1.0F;
		}
		return 0.0F;
	}

	[[nodiscard]] constexpr bool TranslateLegacyImageSpaceConstants(
		RE::ImageSpaceManager::ImageSpaceEffectEnum a_effect,
		bool a_imageSpaceEffectUnk88,
		std::span<float> a_constants) noexcept
	{
		const auto translation = GetLegacyImageSpaceSelectorTranslation(a_effect);
		if (!translation || translation->floatIndex >= a_constants.size()) {
			return false;
		}

		a_constants[translation->floatIndex] =
			ResolveLegacyImageSpaceSelector(*translation, a_imageSpaceEffectUnk88);
		return true;
	}

	void Install();
}
