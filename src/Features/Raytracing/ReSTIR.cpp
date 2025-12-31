#include "ReSTIR.h"
#include "Deferred.h"

void ReSTIR::ReSTIRDI(ReSTIRSettings settings, uint lightCount, ID3D11SamplerState* linearSampler)
{
    if (!settings.EnableReSTIRDI)
        return;

    auto context = globals::d3d::context;
    auto renderer = globals::game::renderer;

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

    ID3D11UnorderedAccessView* uav = reservoirCurrTexture->uav;
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

    ID3D11ShaderResourceView* srvs[5];
    srvs[0] = reservoirPrevTexture->srv;
    srvs[1] = depth.depthSRV;
    srvs[2] = normal.SRV;
    srvs[3] = lightBuffer->srv.get();
    srvs[4] = motionVectors.SRV;

    context->CSSetShaderResources(0, 5, srvs);
    auto dispatchCount = Util::GetScreenDispatchCount();

    {
        TracyD3D11Zone(globals::state->tracyCtx, "ReSTIR - Generate Reservoirs and Temporal Reuse");

        auto shader = ReSTIRGenerateReservoirCS.get();
        context->CSSetShader(shader, nullptr, 0);

        context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
    }
    
    if (settings.SpatialReuse)
    {
        TracyD3D11Zone(globals::state->tracyCtx, "ReSTIR - Spatial Reuse");

        uav = reservoirSpatialTexture->uav;
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

        srvs[0] = reservoirCurrTexture->srv;
        context->CSSetShaderResources(0, 4, srvs);

        auto shader = ReSTIRSpatialReuseCS.get();
        context->CSSetShader(shader, nullptr, 0);

        context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
    } else {
        std::swap(reservoirSpatialTexture, reservoirCurrTexture);
    }

    context->CopyResource(reservoirPrevTexture->resource11, reservoirSpatialTexture->resource11);

    // Unbind resources
    uav = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

    for (auto& srv : srvs)
        srv = nullptr;
    
    context->CSSetShaderResources(0, 5, srvs);

    ID3D11ComputeShader* shader = nullptr;
    context->CSSetShader(shader, 0, 0);
}