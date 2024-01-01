#pragma once

#include "Buffer.h"
#include "Feature.h"

struct WetnessEffects : Feature
{
public:
	static WetnessEffects* GetSingleton()
	{
		static WetnessEffects singleton;
		return &singleton;
	}

	virtual inline std::string GetName() { return "Wetness Effects"; }
	virtual inline std::string GetShortName() { return "WetnessEffects"; }
	inline std::string_view GetShaderDefineName() override { return "WETNESS_EFFECTS"; }

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	struct Settings
	{
		uint EnableWetnessEffects = true;
		float MaxRainWetness = 0.8f;
		float MaxShoreWetness = 0.5f;
		float MaxOcclusion = 0.1f;
		uint ShoreRange = 32;
		float PuddleMinWetness = 0.5f;
		float PuddleRadius = 1.0f;
		float PuddleMaxAngle = 0.9f;
		float PuddleFlatness = 0.95f;
	};

	struct PerPass
	{
		float Wetness;
		DirectX::XMFLOAT3X4 DirectionalAmbientWS;
		Settings settings;
	};

	Settings settings;

	std::unique_ptr<Buffer> perPass = nullptr;

	bool requiresUpdate = true;

	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);
};
