#pragma once

#include "Buffer.h"

class WaterBlending
{
public:
	static WaterBlending* GetSingleton()
	{
		static WaterBlending singleton;
		return &singleton;
	}

	struct Settings
	{
		uint32_t EnableWaterBlending = 1;
		uint32_t EnableWaterBlendingSSR = 1;
		float WaterBlendRange = 1;
		float SSRBlendRange = 2;
	};

	struct alignas(16) PerPass
	{
		float waterHeight;
		Settings settings;
		float pad[3];
	};

	Settings settings;

	std::unique_ptr<Buffer> perPass = nullptr;

	void DrawSettings();

	void Draw(const RE::BSShader* shader, const uint32_t descriptor);
	void SetupResources();
};
