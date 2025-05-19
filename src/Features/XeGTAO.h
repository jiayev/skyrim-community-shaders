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

	ID3D11ComputeShader* CSPrefilterDepths16x16;
	ID3D11ComputeShader* CSGTAOUltra;
	ID3D11ComputeShader* CSDenoisePass;
	ID3D11ComputeShader* CSDenoiseLastPass;

	Texture2D* workingDepths;
	Texture2D* workingEdges;
	Texture2D* workingAOTerm;
	Texture2D* workingAOTermPong;
	Texture2D* outputAO;

	ID3D11SamplerState* samplerPointClamp = nullptr;

	ConstantBuffer* constantBuffer;

	XeGTAO::GTAOSettings settings;

	ID3D11UnorderedAccessView* workingDepthsMIPViews[XE_GTAO_DEPTH_MIP_LEVELS];

	virtual inline std::string GetName() override { return "XeGTAO"; }
	virtual inline std::string GetShortName() override { return "XeGTAO"; }
	virtual inline std::string_view GetShaderDefineName() override { return "XeGTAO"; }

	virtual void SetupResources() override;

	virtual bool SupportsVR() override { return false; };

	void GTAO();
};
