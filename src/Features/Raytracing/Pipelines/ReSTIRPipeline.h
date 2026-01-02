#pragma once

#include "Buffer.h"
#include "Features/Upscaling/DX12SwapChain.h"
#include "Features/Raytracing/Pipeline.h"
#include "Features/Raytracing/Utils.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include "State.h"
#include "Features/Raytracing/Types.h"
#include "Raytracing/Includes/Types/Light.hlsli"

struct ReSTIRPipeline : IPipeline
{
    struct Settings
    {
        bool EnableReSTIRDI = true;
		bool TemporalReuse = true;
        bool SpatialReuse = true;
        int InitialCandidateCount = 8;
        int MaxCandidateCount = 20;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Settings, EnableReSTIRDI, TemporalReuse, SpatialReuse, InitialCandidateCount, MaxCandidateCount);
    };

    struct alignas(16) ReSTIRBuffer
    {
        uint SpatialReuse;
        uint TemporalReuse;
        uint InitialCandidateCount;
        uint LightCount;
        uint MaxCandidateCount;
		float4 NDCToView;
        uint3 Padding;
    };
    STATIC_ASSERT_ALIGNAS_16(ReSTIRBuffer);

    ConstantBuffer* restirCB = nullptr;
    ReSTIRBuffer restirCBData{};

	void CompileShaders(ID3D12Device5* device) override;
	void SetupResources(ID3D12Device5* device) override;
	void ReSTIRDI(Settings settings, uint lightCount, ID3D11SamplerState* linearSampler, ID3D11ShaderResourceView* normalRoughness);
	void SetupTextureResources(uint2 size, ID3D11Device5* d3d11Device, ID3D12Device5* d3d12Device);
	void UpdateLightBuffer(const Light* data, uint64_t count) const;
	void CreateSRV(ID3D12Device5* device, CD3DX12_CPU_DESCRIPTOR_HANDLE reservoir) const;

    eastl::unique_ptr<WrappedResource> reservoirSpatialTexture = nullptr;
	eastl::unique_ptr<Texture2D> reservoirCurrTexture = nullptr;
	eastl::unique_ptr<Texture2D> reservoirPrevTexture = nullptr;

    eastl::unique_ptr<StructuredBuffer> lightBuffer = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> ReSTIRGenerateReservoirCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> ReSTIRSpatialReuseCS = nullptr;
};