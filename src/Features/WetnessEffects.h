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
		float MaxPuddleWetness = 2.667f;
		float MaxShoreWetness = 0.5f;
		uint ShoreRange = 32;
		float MaxPointLightSpecular = 0.4f;
		float MaxDALCSpecular = 0.01f;
		float MaxAmbientSpecular = 1.0f;
		float PuddleRadius = 1.0f;
		float PuddleMaxAngle = 0.95f;
		float PuddleMinWetness = 0.85f;
		float MinRainWetness = 0.65f;
		float SkinWetness = 0.95f;
		float WeatherTransitionSpeed = 3.0f;

		// Raindrop fx settings
		uint EnableRaindropFx = true;
		uint EnableSplashes = true;
		uint EnableRipples = true;
		uint EnableChaoticRipples = true;
		float RaindropFxRange = 1000.f;
		float RaindropGridSize = 4.f;
		float RaindropInterval = .5f;
		float RaindropChance = .3f;
		float SplashesLifetime = 10.0f;
		float SplashesStrength = 1.2f;
		float SplashesMinRadius = .3f;
		float SplashesMaxRadius = .5f;
		float RippleStrength = 1.f;
		float RippleRadius = 1.f;
		float RippleBreadth = .5f;
		float RippleLifetime = .15f;
		float ChaoticRippleStrength = .1f;
		float ChaoticRippleScale = 1.f;
		float ChaoticRippleSpeed = 20.f;
	};

	struct alignas(16) PerPass
	{
		float Time;
		float Raining;
		float Wetness;
		float PuddleWetness;
		DirectX::XMFLOAT3X4 DirectionalAmbientWS;
		RE::DirectX::XMFLOAT4X4 PrecipProj;
		Settings settings;

		float pad[4 - (sizeof(Settings) / 4 + 16) % 4];
	};

	Settings settings;

	std::unique_ptr<Buffer> perPass = nullptr;

	std::unique_ptr<Texture2D> precipOcclusionTex = nullptr;

	bool requiresUpdate = true;
	float wetnessDepth = 0.0f;
	float puddleDepth = 0.0f;
	float lastGameTimeValue = 0.0f;
	uint32_t currentWeatherID = 0;
	uint32_t lastWeatherID = 0;
	float previousWeatherTransitionPercentage = 0.0f;
	RE::DirectX::XMFLOAT4X4 precipProj;

	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void RestoreDefaultSettings();
	float CalculateWeatherTransitionPercentage(float skyCurrentWeatherPct, float beginFade, bool fadeIn);
	void CalculateWetness(RE::TESWeather* weather, RE::Sky* sky, float seconds, float& wetness, float& puddleWetness);

	virtual inline void PostPostLoad() override { Hooks::Install(); }

	struct Hooks
	{
		struct BSParticleShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSParticleShader_SetupGeometry>(RE::VTABLE_BSParticleShader[0]);
		}
	};
};
