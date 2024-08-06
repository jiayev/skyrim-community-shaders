#pragma once
#include "PostProcessFeature.h"

#include "Buffer.h"

struct ColourTransforms : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Colour Transforms"; }
	virtual inline std::string GetDesc() const override { return "Single pixel operators for tonemapping, colour grading and more."; }

	virtual bool SupportsVR() { return true; }

	int transformType = 1;

	// buffers
	struct TonemapCB
	{
		float4 Params0 = { 1.2f, 1.3f, 0.f, 1.f };
		float4 Params1 = { 0.f, 0.f, 0.f, 0.f };
		float4 Params2 = { 0.f, 0.f, 0.f, 0.f };
		float4 Params3 = { 0.f, 0.f, 0.f, 0.f };
	} settings;
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