#pragma once

#include "Buffer.h"
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
        int InitialCandidateCount = 4;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ReSTIRSettings, EnableReSTIRDI, SpatialReuse, TemporalReuse, BiasedSampling, InitialCandidateCount, MaxCandidateCount);
    };

    ReSTIRSettings restirSettings;

    struct ReSTIRBuffer
    {
        uint SpatialReuse;
        uint TemporalReuse;
        uint InitialCandidateCount;
        uint LightCount;
    };
    STATIC_ASSERT_ALIGNAS_16(ReSTIRBuffer);

    void DrawReSTIRSettings();
    void ExecuteReSTIRPass();

    eastl::unique_ptr<WrappedResource> reservoirSpatialTexture = nullptr;
    eastl::unique_ptr<WrappedResource> reservoirCurrTexture = nullptr;
    eastl::unique_ptr<WrappedResource> reservoirPrevTexture = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> ReSTIRGenerateReservoirCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> ReSTIRSpatialReuseCS = nullptr;
};