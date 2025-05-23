#pragma once

#include "Features/XeGTAO/XeGTAO.h"

struct XeGTAOFeature : Feature
{
public:
	static XeGTAOFeature* GetSingleton()
	{
		static XeGTAOFeature singleton;
		return &singleton;
	}

	struct Settings
	{
		bool Enabled = true;
		int QualityLevel = 2;
		bool Denoise = false;
		float Radius = 50.0f;
		float MixStrength = 1.0f;
		bool UseSecondPass = false;
		int SecondPassQualityLevel = 2;
		float SecondPassRadius = 50.0f;
		bool BentNormals = true;
		bool BlurGeneratedNormals = true;
	} menusettings;

	struct alignas(16) PerFrame
	{
		uint Enabled;
		uint BentNormals;
		float MixStrength;
		float pad;
	};

	PerFrame GetCommonBufferData();

	ID3D11ComputeShader* CSPrefilterDepths16x16;
	ID3D11ComputeShader* CSGTAOLow[2];
	ID3D11ComputeShader* CSGTAOMedium[2];
	ID3D11ComputeShader* CSGTAOHigh[2];
	ID3D11ComputeShader* CSGTAOUltra[2];
	ID3D11ComputeShader* CSDenoisePass[2];
	ID3D11ComputeShader* CSDenoiseLastPass[2];
	ID3D11ComputeShader* CSGenerateNormals;
	ID3D11ComputeShader* CSBlur;

	Texture2D* workingDepths;
	Texture2D* workingEdges;
	Texture2D* workingAOTerm;
	Texture2D* workingAOTermPong;
	Texture2D* outputAO;
	Texture2D* generatedNormals;
	Texture2D* blurredNormals;

	ID3D11SamplerState* samplerPointClamp = nullptr;

	ConstantBuffer* constantBuffer;

	XeGTAO::GTAOSettings settings;

	ID3D11UnorderedAccessView* workingDepthsMIPViews[XE_GTAO_DEPTH_MIP_LEVELS];

	virtual inline std::string GetName() override { return "XeGTAO"; }
	virtual inline std::string GetShortName() override { return "XeGTAO"; }
	virtual inline std::string_view GetShaderDefineName() override { return "XeGTAO"; }
	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual inline void RestoreDefaultSettings() override { menusettings = {}; }

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();

	virtual void Prepass() override;

	virtual bool SupportsVR() override { return false; };

	void GTAOGenerateNormals();
	void GTAO(bool b_isFirstPass);
};
