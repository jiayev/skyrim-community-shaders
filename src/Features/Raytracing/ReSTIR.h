#pragma once

#include "Buffer.h"
#include "Features/Upscaling/DX12SwapChain.h"
#include "Features/LightLimitFix.h"
#include <d3d12.h>
#include "State.h"

struct ReSTIR
{
    struct ReSTIRSettings
    {
        bool EnableReSTIRDI = true;
        bool SpatialReuse = true;
        bool TemporalReuse = true;
        int InitialCandidateCount = 8;
        int MaxCandidateCount = 20;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ReSTIRSettings, EnableReSTIRDI, SpatialReuse, TemporalReuse, InitialCandidateCount, MaxCandidateCount);
    };

    struct ReSTIRBuffer
    {
        uint SpatialReuse;
        uint TemporalReuse;
        uint InitialCandidateCount;
        uint LightCount;
        uint MaxCandidateCount;
        uint Padding[3];
    };
    STATIC_ASSERT_ALIGNAS_16(ReSTIRBuffer);

    ConstantBuffer* restirCB = nullptr;
    ReSTIRBuffer restirCBData{};

    void ReSTIRDI(ReSTIRSettings settings, uint lightCount, ID3D11SamplerState* linearSampler);

    eastl::unique_ptr<WrappedResource> reservoirSpatialTexture = nullptr;
    eastl::unique_ptr<WrappedResource> reservoirCurrTexture = nullptr;
    eastl::unique_ptr<WrappedResource> reservoirPrevTexture = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> ReSTIRGenerateReservoirCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> ReSTIRSpatialReuseCS = nullptr;
};