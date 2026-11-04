#include "RayReconstruction.h"

#include "Deferred.h"
#include "Features/LinearLighting.h"
#include "Features/ScreenSpaceReflections.h"
#include "State.h"
#include "Util.h"

namespace
{
	struct GuideConstants
	{
		uint32_t specularEnabled;
		uint32_t specularHalf;
		uint32_t padding[2];
	};
}

bool RayReconstruction::BeginFrame()
{
	readyFrame = UINT32_MAX;
	if (!prepareGuides)
		prepareGuides.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\Upscaling\\PrepareRayReconstruction.cs.hlsl", {}, "cs_5_0")));
	if (!prepareGuides)
		return false;
	if (!globals::features::linearLighting.IsLinearLightingActive()) {
		if (!decodeColor)
			decodeColor.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
				L"Data\\Shaders\\Upscaling\\RayReconstructionColor.cs.hlsl", {}, "cs_5_0")));
		if (!encodeColor)
			encodeColor.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
				L"Data\\Shaders\\Upscaling\\RayReconstructionColor.cs.hlsl", { { "TO_SCENE", "" } }, "cs_5_0")));
		if (!decodeColor || !encodeColor)
			return false;
	}
	auto main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC desc;
	main.texture->GetDesc(&desc);
	if (!beforeSSS || beforeSSS->desc.Width != desc.Width || beforeSSS->desc.Height != desc.Height || beforeSSS->desc.Format != desc.Format) {
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.MiscFlags = 0;
		const auto create = [&](eastl::unique_ptr<Texture2D>& a_texture, const char* a_name) {
			a_texture = eastl::make_unique<Texture2D>(desc, a_name);
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{ .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { 0, 1 } };
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{ .Format = desc.Format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { 0 } };
			a_texture->CreateSRV(srv);
			a_texture->CreateUAV(uav);
		};
		create(beforeSSS, "DLSSRR::BeforeSSS");
		create(colorBeforeTransparency, "DLSSRR::ColorBeforeTransparency");
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		create(diffuseAlbedo, "DLSSRR::DiffuseAlbedo");
		create(specularAlbedo, "DLSSRR::SpecularAlbedo");
		create(normalRoughness, "DLSSRR::NormalRoughness");
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		create(tracedSpecularHitDistance, "DLSSRR::TracedSpecularHitDistance");
		create(specularHitDistance, "DLSSRR::SpecularHitDistance");
		desc.Format = DXGI_FORMAT_R16_FLOAT;
		create(sssGuide, "DLSSRR::SSSGuide");
	}
	if (!constants)
		constants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<GuideConstants>(), "DLSSRR::GuideConstants");
	return true;
}

void RayReconstruction::CaptureBeforeSSS(ID3D11Resource* a_color)
{
	globals::d3d::context->CopyResource(beforeSSS->resource.get(), a_color);
}

void RayReconstruction::PrepareGuides(ID3D11ShaderResourceView* a_colorAfterSSS)
{
	auto context = globals::d3d::context;
	auto targets = globals::game::renderer->GetRuntimeData().renderTargets;
	auto& ssr = globals::features::screenSpaceReflections;
	GuideConstants data{ ssr.GetOutputTexture() != nullptr, ssr.settings.HalfRes, {} };
	constants->Update(data);
	auto cb = constants->CB();
	context->CSSetConstantBuffers(1, 1, &cb);
	cb = globals::state->sharedDataCB->CB();
	context->CSSetConstantBuffers(5, 1, &cb);
	ID3D11ShaderResourceView* srvs[] = {
		targets[ALBEDO].SRV, targets[REFLECTANCE].SRV, targets[NORMALROUGHNESS].SRV,
		Util::GetCurrentSceneDepthSRV(true), tracedSpecularHitDistance->srv.get(),
		beforeSSS->srv.get(), a_colorAfterSSS, targets[MASKS].SRV
	};
	ID3D11UnorderedAccessView* uavs[] = {
		diffuseAlbedo->uav.get(), specularAlbedo->uav.get(), normalRoughness->uav.get(),
		specularHitDistance->uav.get(), sssGuide->uav.get()
	};
	context->CSSetShaderResources(0, 8, srvs);
	context->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
	context->CSSetShader(prepareGuides.get(), nullptr, 0);
	auto count = Util::GetScreenDispatchCount(true);
	context->Dispatch(count.x, count.y, 1);
	std::fill(std::begin(srvs), std::end(srvs), nullptr);
	std::fill(std::begin(uavs), std::end(uavs), nullptr);
	context->CSSetShaderResources(0, 8, srvs);
	context->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
}

void RayReconstruction::CaptureBeforeTransparency(ID3D11Resource* a_color)
{
	globals::d3d::context->CopyResource(colorBeforeTransparency->resource.get(), a_color);
	ConvertColor(colorBeforeTransparency->uav.get(), false);
	readyFrame = globals::state->frameCount;
}

void RayReconstruction::ConvertColor(ID3D11UnorderedAccessView* a_color, bool a_toScene)
{
	if (globals::features::linearLighting.IsLinearLightingActive())
		return;
	auto context = globals::d3d::context;
	auto cb = globals::state->sharedDataCB->CB();
	context->CSSetConstantBuffers(5, 1, &cb);
	context->CSSetUnorderedAccessViews(0, 1, &a_color, nullptr);
	context->CSSetShader(a_toScene ? encodeColor.get() : decodeColor.get(), nullptr, 0);
	auto count = Util::GetScreenDispatchCount(false);
	context->Dispatch(count.x, count.y, 1);
	a_color = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &a_color, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
}

void RayReconstruction::ClearShaderCache()
{
	prepareGuides = nullptr;
	decodeColor = nullptr;
	encodeColor = nullptr;
	readyFrame = UINT32_MAX;
}

bool RayReconstruction::GuidesReady() const
{
	return readyFrame == globals::state->frameCount;
}
