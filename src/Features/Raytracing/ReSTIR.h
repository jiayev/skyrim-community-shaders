#pragma once

#include "Features/Upscaling/DX12SwapChain.h"
#include "LightLimitFix.h"
#include <d3d12.h>
#include "State.h"

class ReSTIR
{
public:
    struct ReSTIRSettings
    {
        bool EnableReSTIRDI = true;
        bool SpatialReuse = true;
        bool TemporalReuse = true;
        bool BiasedSampling = false;
        int InitialCandidateCount = 4;
        int MaxCandidateCount = 32;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ReSTIRSettings, EnableReSTIRDI, SpatialReuse, TemporalReuse, BiasedSampling, InitialCandidateCount, MaxCandidateCount);
    };

    void DrawReSTIRSettings();

    void SetupReSTIRResources();
    void CompileReSTIRShaders();
    void ClearReSTIRShaderCache();
    void ExecuteReSTIRPass();

    eastl::unique_ptr<WrappedResource> reservoirTexture = nullptr;
    eastl::unique_ptr<WrappedResource> reservoirCandidateTexture = nullptr;
    eastl::unique_ptr<WrappedResource> reservoirPrevTexture = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> csReSTIRDI = nullptr;
};