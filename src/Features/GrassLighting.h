#pragma once

#include "Buffer.h"
#include "Feature.h"

struct GrassLighting : Feature
{
	static GrassLighting* GetSingleton()
	{
		static GrassLighting singleton;
		return &singleton;
	}

	virtual inline std::string GetName() { return "Grass Lighting"; }
	virtual inline std::string GetShortName() { return "GrassLighting"; }

	struct Settings
	{
		float Glossiness = 20;
		float SpecularStrength = 0.5;
		float SubsurfaceScatteringAmount = 1.0;
		std::uint32_t EnableDirLightFix = 1;
		std::uint32_t EnablePointLights = 1;
	};

	struct alignas(16) PerFrame
	{
		DirectX::XMFLOAT4 EyePosition;
		DirectX::XMFLOAT3X4 DirectionalAmbient;
		float SunlightScale;
		Settings Settings;
		float pad[2];
	};

	struct alignas(16) PerFrameVR
	{
		DirectX::XMFLOAT4 EyePosition;
		DirectX::XMFLOAT4 EyePosition2;
		DirectX::XMFLOAT3X4 DirectionalAmbient;
		float SunlightScale;
		Settings Settings;
		float pad[2];
	};

	Settings settings;

	bool updatePerFrame = false;
	ConstantBuffer* perFrame = nullptr;
	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();
	void ModifyGrass(const RE::BSShader* shader, const uint32_t descriptor);
	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);
};
