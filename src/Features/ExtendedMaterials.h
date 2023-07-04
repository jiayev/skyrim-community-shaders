#pragma once

#include "Buffer.h"

class ExtendedMaterials
{
public:
	static ExtendedMaterials* GetSingleton()
	{
		static ExtendedMaterials singleton;
		return &singleton;
	}
	
	bool enabledFeature = false;
	std::string version;

	const char* _name = "Extended Materials";
	const char* _shortName = "ExtendedMaterials";

	struct Settings
	{
		uint32_t EnableComplexMaterial = 1;

		uint32_t EnableParallax = 1;
		uint32_t EnableTerrain = 1;
		uint32_t EnableHighQuality = 0;

		uint32_t MaxDistance = 2048;
		float CRPMRange = 0.5f;
		float BlendRange = 0.05f;
		float Height = 0.1f;

		uint32_t EnableShadows = 1;
		uint32_t ShadowsStartFade = 768;
		uint32_t ShadowsEndFade = 1024;
	};

	struct alignas(16) PerPass
	{
		uint32_t CullingMode = 0;
		Settings settings;
	};

	ID3D11SamplerState* terrainSampler = nullptr;

	Settings settings;

	std::unique_ptr<Buffer> perPass = nullptr;

	bool enabled = true;
	
	void SetupResources();

	void DrawSettings();

	void ModifyLighting(const RE::BSShader* shader, const uint32_t descriptor);
	void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	void Load(json& o_json);
	void Save(json& o_json);

	bool ValidateCache(CSimpleIniA& a_ini);
	void WriteDiskCacheInfo(CSimpleIniA& a_ini);
};

