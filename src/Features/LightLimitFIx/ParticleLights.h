#pragma once

class ParticleLights
{
public:
	static ParticleLights* GetSingleton()
	{
		static ParticleLights singleton;
		return &singleton;
	}

	enum class Flicker
	{
		None = 0,
		Normal = 1
	};

	struct Config
	{
		bool cull = false;
		RE::NiColor colorMult{ 1.0f, 1.0f, 1.0f };
		float radiusMult = 1.0f;
		float saturationMult = 1.0f;
		bool flicker = false;
		float flickerSpeed = 0.0f;
		float flickerIntensity = 0.0f;
		float flickerMovement = 0.0f;
	};

	struct GradientConfig
	{
		RE::NiColor color;
	};

	std::unordered_map<std::string, Config> particleLightConfigs;
	std::unordered_map<std::string, GradientConfig> particleLightGradientConfigs;

	void GetConfigs();
};