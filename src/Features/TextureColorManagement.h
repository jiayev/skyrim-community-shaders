#pragma once

#include "ColorManagement.h"

namespace TextureColorManagement
{
	enum class Content : std::uint8_t
	{
		Data,
		Color
	};

	enum class EncodingRule : std::uint8_t
	{
		Infer,
		SRGB,
		Linear,
		GameGamma
	};

	enum class AlphaMode : std::uint8_t
	{
		Data,
		Opacity,
		Coverage
	};

	class TextureContract
	{
	public:
		constexpr TextureContract(const TextureContract&) = default;
		constexpr TextureContract& operator=(const TextureContract&) = default;

		static constexpr TextureContract Data() { return {}; }
		static constexpr TextureContract Color(EncodingRule encodingRule = EncodingRule::Infer, AlphaMode alpha = AlphaMode::Data)
		{
			return TextureContract(Content::Color, encodingRule, alpha);
		}

		constexpr bool IsColor() const { return content == Content::Color; }
		constexpr EncodingRule GetEncodingRule() const { return encoding; }
		constexpr AlphaMode GetAlphaMode() const { return alphaMode; }

	private:
		constexpr TextureContract() = default;
		constexpr TextureContract(Content a_content, EncodingRule a_encoding, AlphaMode a_alphaMode) :
			content(a_content), encoding(a_encoding), alphaMode(a_alphaMode) {}

		Content content = Content::Data;
		EncodingRule encoding = EncodingRule::Infer;
		AlphaMode alphaMode = AlphaMode::Data;
	};

	void InstallHooks();
	void ApplyBindings(ID3D11DeviceContext* context);
	void ResetBindingContracts();
	void SetBindingContract(std::uint32_t slot, ID3D11ShaderResourceView* sourceView, TextureContract contract);
	void SetAssetOverride(std::string_view path, ColorManagement::Encoding sourceEncoding);
	void ClearAssetOverride(std::string_view path);
}
