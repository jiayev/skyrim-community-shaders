#include "ReSTIR.h"
#include "Deferred.h"
#include "Features/Raytracing.h"

void ReSTIR::CompileShaders([[maybe_unused]] ID3D12Device5* device) 
{
	// ReSTIR shaders
	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Raytracing\\ReSTIR\\ReSTIRGenerateReservoirCS.hlsl", { { "DX11", "" } }, "cs_5_0")); rawPtr)
		ReSTIRGenerateReservoirCS.attach(rawPtr);

	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Raytracing\\ReSTIR\\ReSTIRSpatialReuseCS.hlsl", { { "DX11", "" } }, "cs_5_0")); rawPtr)
		ReSTIRSpatialReuseCS.attach(rawPtr);
}

void ReSTIR::SetupResources([[maybe_unused]] ID3D12Device5* device)
{
    auto desc = StructuredBufferDesc<Light>(Raytracing::MAX_LIGHTS);
	lightBuffer = eastl::make_unique<StructuredBuffer>(desc, Raytracing::MAX_LIGHTS);
    lightBuffer->CreateSRV();

    restirCB = new ConstantBuffer(ConstantBufferDesc<ReSTIRBuffer>());
}

void ReSTIR::SetupTextureResources(uint2 size, ID3D11Device5* d3d11Device, ID3D12Device5* d3d12Device)
{
	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width = size.x;
	texDesc.Height = size.y;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;


	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = {
			.MostDetailedMip = 0,
			.MipLevels = texDesc.MipLevels }
	};

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};

	reservoirSpatialTexture = eastl::make_unique<WrappedResource>(texDesc, d3d11Device, d3d12Device);
	DX::ThrowIfFailed(reservoirSpatialTexture->resource->SetName(L"ReSTIR DI Spatial Reuse Reservoir Texture"));

	reservoirCurrTexture = eastl::make_unique<Texture2D>(texDesc);
	reservoirCurrTexture->CreateSRV(srvDesc);
	reservoirCurrTexture->CreateUAV(uavDesc);

	reservoirPrevTexture = eastl::make_unique<Texture2D>(texDesc);
	reservoirCurrTexture->CreateSRV(srvDesc);
}

void ReSTIR::CreateSRV(ID3D12Device5* device, CD3DX12_CPU_DESCRIPTOR_HANDLE reservoir) const
{
	device->CreateShaderResourceView(reservoirSpatialTexture->resource.get(), nullptr, reservoir);
}

void ReSTIR::UpdateLightBuffer(const Light* data, uint64_t count) const
{
	lightBuffer->Update(data, count);
}

void ReSTIR::ReSTIRDI(Settings settings, uint lightCount, ID3D11SamplerState* linearSampler)
{
    auto context = globals::d3d::context;
    auto renderer = globals::game::renderer;

    logger::trace("[ReSTIR] ReSTIRDI called with LightCount: {}", lightCount);
    {
        restirCBData.SpatialReuse = settings.SpatialReuse ? 1 : 0;
        restirCBData.TemporalReuse = settings.TemporalReuse ? 1 : 0;
        restirCBData.InitialCandidateCount = static_cast<uint>(settings.InitialCandidateCount);
        restirCBData.MaxCandidateCount = static_cast<uint>(settings.MaxCandidateCount);
        restirCBData.LightCount = lightCount;

        restirCB->Update(&restirCBData);
    }

    ID3D11Buffer* buffer = restirCB->CB();
    context->CSSetConstantBuffers(1, 1, &buffer);
    context->CSSetSamplers(0, 1, &linearSampler);

    auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
    auto normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
    auto motionVectors = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

    ID3D11UnorderedAccessView* uav = nullptr;
	ID3D11ShaderResourceView* srvs[5]{ nullptr };

    auto dispatchCount = Util::GetScreenDispatchCount();

    {
        //TracyD3D11Zone(globals::state->tracyCtx, "ReSTIR - Generate Reservoirs and Temporal Reuse");

		uav = reservoirCurrTexture->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

		srvs[0] = reservoirPrevTexture->srv.get();
		srvs[1] = depth.depthSRV;
		srvs[2] = normal.SRV;
		srvs[3] = lightBuffer->SRV();
		srvs[4] = motionVectors.SRV;

		context->CSSetShaderResources(0, 5, srvs);

		context->CSSetShader(ReSTIRGenerateReservoirCS.get(), nullptr, 0);

        context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
    }
    
    if (settings.SpatialReuse)
    {
        //TracyD3D11Zone(globals::state->tracyCtx, "ReSTIR - Spatial Reuse");

        uav = reservoirSpatialTexture->uav;
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

        srvs[0] = reservoirCurrTexture->srv.get();
        context->CSSetShaderResources(0, 4, srvs);

		context->CSSetShader(ReSTIRSpatialReuseCS.get(), nullptr, 0);

        context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
    } else {
		context->CopyResource(reservoirSpatialTexture->resource11, reservoirCurrTexture->resource.get());
    }

    // Unbind resources
	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	for (auto& srv : srvs)
		srv = nullptr;

	context->CSSetShaderResources(0, 5, srvs);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, 0, 0);

    context->CopyResource(reservoirPrevTexture->resource.get(), reservoirSpatialTexture->resource11);
}