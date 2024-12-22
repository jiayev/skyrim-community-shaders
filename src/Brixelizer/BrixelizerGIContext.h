#pragma once

#include <shared_mutex>
#include <unordered_set>

#include <d3d12.h>
#include <eastl/set.h>

#include "Brixelizer.h"

class BrixelizerGIContext
{
public:
	static BrixelizerGIContext* GetSingleton()
	{
		static BrixelizerGIContext singleton;
		return &singleton;
	}

	FfxBrixelizerGIContextDescription giInitializationParameters = {};
	FfxBrixelizerGIDispatchDescription giDispatchDesc = {};
	FfxBrixelizerGIContext brixelizerGIContext = {};

	Brixelizer::WrappedResource diffuseGi;
	Brixelizer::WrappedResource specularGi;

	Brixelizer::WrappedResource depth;
	Brixelizer::WrappedResource normal;

	Brixelizer::WrappedResource historyDepth;
	Brixelizer::WrappedResource historyNormal;
	Brixelizer::WrappedResource prevLitOutput;
	Brixelizer::WrappedResource roughness;

	winrt::com_ptr<ID3D12Resource> noiseTextures[16];

	ID3D11ComputeShader* copyToSharedBufferCS;
	ID3D11ComputeShader* GetCopyToSharedBufferCS();

	void ClearShaderCache();

	void CopyResourcesToSharedBuffers();

	void CreateMiscTextures();
	void CreateNoiseTextures();

	void InitBrixelizerGIContext();

	void UpdateBrixelizerGIContext();

	void CopyHistoryResources();
};
