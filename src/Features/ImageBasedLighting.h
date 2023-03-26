#pragma once

#include "RE/BSGraphicsTypes.h"

#include "Buffer.h"

class ImageBasedLighting
{
public:
	static ImageBasedLighting* GetSingleton()
	{
		static ImageBasedLighting singleton;
		return &singleton;
	}

	bool enabled = false;
	std::string version;

	struct Settings
	{
		std::uint32_t IBLMetalOnly = false;
		float IBLBalance = 1.0f;
		float IBLIntensity = 1.5f;
	};

	struct alignas(16) PerPass
	{
		std::uint32_t EnableIBL;
		std::uint32_t GeneratingIBL;
		Settings			Settings;
		float pad0;
		float pad1;
		float pad2;
	};

	bool enableIBL = false;
	bool renderedScreenCamera = false;

	Settings settings;
	ConstantBuffer* perPass = nullptr;

	Texture2D* cubemapIBL = nullptr;

	PerPass lastPerPassData{};

	void SetupResources();

	void DrawSettings();
	void ModifyGrass(const RE::BSShader* shader, const uint32_t descriptor);
	void UpdateCubemap();
	void ModifyLighting(const RE::BSShader* shader, const uint32_t descriptor);
	void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	void Load(json& o_json);
	void Save(json& o_json);


	bool ValidateCache(CSimpleIniA& a_ini);
	void WriteDiskCacheInfo(CSimpleIniA& a_ini);
	void Reset();
};
