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
		bool cull;
		RE::NiColor colorMult;
		float radiusMult;
		bool flicker;
		float flickerSpeed;
		float flickerIntensity;
		float flickerMovement;
	};

	std::unordered_map<std::string, Config> particleLightConfigs;

	void GetConfigs();
};