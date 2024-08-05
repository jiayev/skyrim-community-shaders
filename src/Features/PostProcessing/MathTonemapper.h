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
		// 3 AcesHill
		// 4 AcesNarkowicz
		// 5 AcesGuy
		// 6 AgxMinimal
		int Tonemapper = 6;

		float KeyValue = 2.f;
		float WhitePoint = 2.0f;

		float Slope = 1.2f;
		float Power = 1.3f;
		float Offset = 0.f;
		float Saturation = 1.f;
	} settings;

	// buffers
	struct alignas(16) TonemapCB
	{
		float4 Params;
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