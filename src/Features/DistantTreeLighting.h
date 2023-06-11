#pragma once

#include "Buffer.h"

class DistantTreeLighting
{
public:
	static DistantTreeLighting* GetSingleton()
	{
		static DistantTreeLighting singleton;
		return &singleton;
	}

	bool enabled = false;
	std::string version;

	struct Settings
	{
		std::uint32_t	EnableComplexTreeLOD = 1;
		std::uint32_t	EnableDirLightFix = 1;
		float			SubsurfaceScatteringAmount = 0.5;
		float			FogDimmerAmount = 1.0;
	};

	struct alignas(16) PerPass
	{
		DirectX::XMFLOAT4	EyePosition;
		DirectX::XMFLOAT3X4	DirectionalAmbient;
		DirectX::XMFLOAT4	DirLightColor;
		DirectX::XMFLOAT4	DirLightDirection;
		float				DirLightScale;
		std::uint32_t		ComplexAtlasTexture;
		Settings			Settings;
		float pad0;
		float pad1;
	};

	Settings settings;
	ConstantBuffer* perPass = nullptr;

	RE::TESWorldSpace* lastWorldSpace = nullptr;
	bool complexAtlasTexture = false;

	void SetupResources();

	void DrawSettings();
	void ModifyDistantTree(const RE::BSShader* shader, const uint32_t descriptor);
	void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	void Load(json& o_json);
	void Save(json& o_json);


	bool ValidateCache(CSimpleIniA& a_ini);
	void WriteDiskCacheInfo(CSimpleIniA& a_ini);
};
