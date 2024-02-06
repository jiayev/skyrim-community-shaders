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
		float MaxRainWetness = 1.0f;
		float MaxShoreWetness = 0.5f;
		uint ShoreRange = 32;
		float PuddleRadius = 1.0f;
		float PuddleMaxAngle = 0.95f;
		float PuddleMinWetness = 0.85f;
		float MinRainWetness = 0.65f;
		float SkinWetness = 0.825f;
		float WeatherTransitionSpeed = 3.0f;
	};

	struct alignas(16) PerPass
	{
		float Wetness;
		float PuddleWetness;
		DirectX::XMFLOAT3X4 DirectionalAmbientWS;
		Settings settings;
	};

	Settings settings;

	std::unique_ptr<Buffer> perPass = nullptr;

	bool requiresUpdate = true;
	float wetnessDepth = 0.0f;
	float puddleDepth = 0.0f;
	float lastGameTimeValue = 0.0f;
	uint32_t currentWeatherID = 0;
	uint32_t lastWeatherID = 0;

	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void RestoreDefaultSettings();
	float CalculateWeatherTransitionPercentage(float skyCurrentWeatherPct, float beginFade, bool fadeIn);
	void CalculateWetness(RE::TESWeather* weather, RE::Sky* sky, float seconds, float& wetness, float& puddleWetness);
};
