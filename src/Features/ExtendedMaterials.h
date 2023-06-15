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

	struct Settings
	{
		uint32_t EnableParallax = 1;
		uint32_t EnableTerrainParallax = 1;
		uint32_t EnableExtendedParallax = 1;
		uint32_t EnableComplexMaterial = 1;
		uint32_t LODLevel = 8;
		uint32_t MinSamples = 4;
		uint32_t MaxSamples = 16;
		float Intensity = 1;
	};

	struct alignas(16) PerPass
	{
		Settings settings;
	};

	Settings settings;

	std::unique_ptr<Buffer> perPass = nullptr;

	bool enabled = true;
	
	void SetupResources();

	void DrawSettings();

	void ModifyGrass(const RE::BSShader* shader, const uint32_t descriptor);
	void ModifyDistantTree(const RE::BSShader*, const uint32_t descriptor);
	void ModifyLighting(const RE::BSShader* shader, const uint32_t descriptor);
	void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	void Load(json& o_json);
	void Save(json& o_json);

	bool ValidateCache(CSimpleIniA& a_ini);
	void WriteDiskCacheInfo(CSimpleIniA& a_ini);
	void Reset();

};
