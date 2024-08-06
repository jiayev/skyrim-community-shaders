#pragma once
#include "PostProcessFeature.h"

#include "Buffer.h"

struct MathTonemapper : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Math Tonemapper"; }
	virtual inline std::string GetDesc() const override { return "Local tonemapping operators with various maths functions."; }

	virtual bool SupportsVR() { return true; }

	struct Settings
	{
		// 0 Reinhard
		// 1 ReinhardExt
		// 2 HejlBurgessDawsonFilmic
		// 3 AldridgeFilmic
		// 4 AcesHill
		// 5 AcesNarkowicz
		// 6 AcesGuy
		// 7 AgxMinimal
		// 8 LottesFilmic
		// 9 DayFilmic
		// 10 UchimuraFilmic
		int Tonemapper = 7;

		float Exposure = 1.f;
		float WhitePoint = 2.0f;
		float Cutoff = 0.19f;

		// AgX
		float Slope = 1.2f;
		float Power = 1.3f;
		float Offset = 0.f;
		float Saturation = 1.f;

		// Lottes
		float ContrastLottes = 1.6f;
		float Shoulder = 0.977f;
		float HdrMax = 8.f;
		float MidIn = 0.18f;
		float MidOut = 0.267f;

		// Day
		float BlackPoint = 0.f;
		float CrossoverPoint = .3f;
		float WhitePointDay = 2.f;
		float ShoulderStrength = 0.8f;
		float ToeStrength = 0.7f;

		// Uchimura
		float MaxBrightness = 1.f;
		float ContrastUchimura = 1.f;
		float LinearStart = .22f;
		float LinearLen = 0.4f;
		float BlackTightnessShape = 1.33f;
		float BlackTightnessOffset = 0.f;
	} settings;

	// buffers
	struct alignas(16) TonemapCB
	{
		float4 Params0;
		float4 Params1;
	};
	std::unique_ptr<ConstantBuffer> tonemapCB = nullptr;

	std::unique_ptr<Texture2D> texTonemap = nullptr;

	bool recompileFlag = true;
	winrt::com_ptr<ID3D11ComputeShader> tonemapCS = nullptr;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json&) override;
	virtual void SaveSettings(json&) override;

	virtual void DrawSettings() override;

	virtual void Draw(TextureInfo&) override;
};