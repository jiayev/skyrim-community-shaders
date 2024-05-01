#pragma once

#include "Buffer.h"
#include <nvapi.h>

class VariableRateShading
{
public:
	static VariableRateShading* GetSingleton()
	{
		static VariableRateShading singleton;
		return &singleton;
	}

	bool enableVRS = true;

	bool nvapiLoaded = false;
	bool vrsActive = false;
	bool vrsPass = false;
	ID3D11Texture2D* singleEyeVRSTex[2];
	ID3D11UnorderedAccessView* singleEyeVRSUAV[2];
	ID3D11NvShadingRateResourceView* singleEyeVRSView[2];

	std::unordered_set<uint> screenTargets;

	Texture2D* reductionData = nullptr;
	ID3D11ComputeShader* computeNASDataCS = nullptr;
	ID3D11ComputeShader* computeShadingRateCS = nullptr;

	void ClearShaderCache();
	ID3D11ComputeShader* GetComputeNASData();
	ID3D11ComputeShader* GetComputeShadingRate();

	void DrawSettings();

	void UpdateVRS();
	void ComputeShadingRate();

	void Setup();
	void SetupSingleEyeVRS(int eye, int width, int height);
	void UpdateViews(bool a_enable);
};