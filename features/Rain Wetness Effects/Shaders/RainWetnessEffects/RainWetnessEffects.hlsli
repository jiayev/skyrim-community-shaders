

struct PerPassRainWetnessEffects
{
	bool EnableRainWetnessEffects;
	float RainShininessMultiplierDay;
	float RainSpecularMultiplierDay;
	float RainDiffuseMultiplierDay;
	float RainShininessMultiplierNight;
	float RainSpecularMultiplierNight;
	float RainDiffuseMultiplierNight;
	float RainShininessMultiplier;
	float RainSpecularMultiplier;
	float RainDiffuseMultiplier;
	float pad[2];
};

StructuredBuffer<PerPassRainWetnessEffects> perPassRainWetnessEffects : register(t22);