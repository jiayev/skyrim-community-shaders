#pragma once

#include "Buffer.h"

class GrassLighting
{
public:
	static GrassLighting* GetSingleton()
	{
		static GrassLighting singleton;
		return &singleton;
	}

	bool enabled = false;
	std::string version;

	struct Settings
	{
		float Glossiness = 20;
		float SpecularStrength = 0.5;
		float SubsurfaceScatteringAmount = 0.5;
		std::uint32_t EnableDirLightFix = 1;
		std::uint32_t EnablePointLights = 1;
	};

	struct alignas(16) PerFrame
	{
		DirectX::XMFLOAT4 EyePosition;
		DirectX::XMFLOAT3X4 DirectionalAmbient;
		float SunlightScale;
		Settings Settings;
		float pad0;
		float pad1;
	};

	Settings settings;

	bool updatePerFrame = false;
	ConstantBuffer* perFrame = nullptr;
	void SetupResources();
	void Reset();

	void DrawSettings();
	void ModifyGrass(const RE::BSShader* shader, const uint32_t descriptor);
	void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	void Load(json& o_json);
	void Save(json& o_json);

	bool ValidateCache(CSimpleIniA& a_ini);
	void WriteDiskCacheInfo(CSimpleIniA& a_ini);
};
