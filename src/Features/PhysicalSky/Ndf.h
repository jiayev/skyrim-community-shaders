#pragma once

#include "Buffer.h"

struct TextureManager
{
	std::string name;
	ankerl::unordered_dense::map<std::string, winrt::com_ptr<ID3D11ShaderResourceView>> texList;

	bool LoadTexture(std::filesystem::path path);

	inline ID3D11ShaderResourceView* Query(const std::string& path) const
	{
		if (texList.contains(path))
			return texList.at(path).get();
		return nullptr;
	}

	inline std::vector<std::string> ListPaths()
	{
		std::vector<std::string> retval;
		std::ranges::transform(texList, std::back_inserter(retval), [](auto& pair) { return pair.first; });
		return retval;
	}

	std::string uiPath;
	void DrawUI();
};

namespace nlohmann
{
	void to_json(json&, const TextureManager&);
	void from_json(const json&, TextureManager&);
}

struct TexNdfSettings
{
	std::string texPath;
};

struct CumuliformNdfSettings
{
	DirectX::XMUINT2 scale0 = { 10, 10 };
	float2 offset0 = { 3.f, 3.f };
	DirectX::XMUINT2 scale1 = { 20, 20 };
	float2 offset1 = { 6.f, 6.f };
	DirectX::XMUINT2 scale2 = { 40, 40 };
	float2 offset2 = { 24.f, 24.f };
	float2 clipRange = { 0.4f, 1.f };
	float power = 0.7f;
	float wispiness = 0.1f;
	float rot0 = 1.f;
	float rot1 = 2.f;
	float rot2 = 3.f;
	float _pad = 0.f;
};

using NdfSettings = std::variant<TexNdfSettings, CumuliformNdfSettings>;

struct NdfManager
{
	constexpr static uint16_t kNdfDim = 256;

	eastl::unique_ptr<Texture2D> texNdfOutput = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> cumuliformProgram = nullptr;
	eastl::unique_ptr<ConstantBuffer> cumuliformCb = {};

	void SetupResources();
	void CompileShaders();

	static const char* GetSettingsTypeName(const NdfSettings& ndfSettings);
	static const char* GetSettingsHint(const NdfSettings& ndfSettings);
	static void DrawNdfSettings(NdfSettings& ndfSettings, TextureManager& texManager);
	void UpdateNdf(const NdfSettings& ndfSettings);
	ID3D11ShaderResourceView* GetNdf(const NdfSettings& ndfSettings, TextureManager& texManager);
};

struct CloudLayer
{
	// placement
	float bottom = 0.2f;
	float thickness = 0.3f;
	// ndf
	float2 ndfScale = { 16.f, 16.f };  // km
	// noise
	float noiseScale = 0.2f;                   // km
	float3 noiseSpeed = { 0.f, -4.8f, 5.7f };  // m/s

	float power = 1.0f;

	// density
	float3 scatter = { 85.f, 90.f, 95.f };
	float3 absorption = { 15.f, 10.f, 5.f };

	// visuals
	float averageDensity = 0.02f;

	float msMult = 10.0f;
	float msTransmittancePower = 0.15f;
	float msHeightPower = 0.7f;

	float ambientMult = 1.0f;
};
