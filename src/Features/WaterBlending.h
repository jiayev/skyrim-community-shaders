#pragma once

#include "Buffer.h"
#include "Feature.h"

struct WaterBlending : Feature
{
public:
	static WaterBlending* GetSingleton()
	{
		static WaterBlending singleton;
		return &singleton;
	}

	virtual inline std::string GetName() { return "Water Blending"; }
	virtual inline std::string GetShortName() { return "WaterBlending"; }

	struct Settings
	{
		uint32_t EnableWaterBlending = 1;
		uint32_t EnableWaterBlendingSSR = 1;
		float WaterBlendRange = 1;
		float SSRBlendRange = 1;
	};

	struct alignas(16) PerPass
	{
		float waterHeight;
		Settings settings;
		float pad[3];
	};

	Settings settings;

	std::unique_ptr<Buffer> perPass = nullptr;

	virtual void SetupResources();
	virtual inline void Reset() {}

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);
};
