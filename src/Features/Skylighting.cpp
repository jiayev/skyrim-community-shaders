#include "Skylighting.h"
#include <DDSTextureLoader.h>
#include <Deferred.h>
#include <Features/TerrainBlending.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skylighting::Settings,
	EnableSkylighting,
	HeightSkylighting,
	MinimumBound,
	RenderTrees)

void Skylighting::DrawSettings()
{
	ImGui::Checkbox("Enable Skylighting", &settings.EnableSkylighting);
	ImGui::Checkbox("Enable Height Map Rendering", &settings.HeightSkylighting);
	ImGui::SliderFloat("Minimum Bound", &settings.MinimumBound, 1, 256, "%.0f");
	ImGui::Checkbox("Render Trees", &settings.RenderTrees);
}

void Skylighting::Draw(const RE::BSShader*, const uint32_t)
{
}

void Skylighting::SetupResources()
{
	GetSkylightingCS();
	GetSkylightingShadowMapCS();

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	{
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.UAV->GetDesc(&uavDesc);

		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		skylightingTexture = new Texture2D(texDesc);
		skylightingTexture->CreateSRV(srvDesc);
		skylightingTexture->CreateUAV(uavDesc);

		skylightingTempTexture = new Texture2D(texDesc);
		skylightingTempTexture->CreateSRV(srvDesc);
		skylightingTempTexture->CreateUAV(uavDesc);
	}

	{
		auto& precipitationOcclusion = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};

		precipitationOcclusion.texture->GetDesc(&texDesc);
		precipitationOcclusion.depthSRV->GetDesc(&srvDesc);
		precipitationOcclusion.views[0]->GetDesc(&dsvDesc);

		occlusionTexture = new Texture2D(texDesc);
		occlusionTexture->CreateSRV(srvDesc);
		occlusionTexture->CreateDSV(dsvDesc);
	}

	{
		perFrameCB = new ConstantBuffer(ConstantBufferDesc<PerFrameCB>());
	}

	{
		auto& device = State::GetSingleton()->device;
		auto& context = State::GetSingleton()->context;

		DirectX::CreateDDSTextureFromFile(device, context, L"Data\\Shaders\\Skylighting\\bluenoise.dds", nullptr, &noiseView);
	}

	{
		auto& device = State::GetSingleton()->device;

		D3D11_SAMPLER_DESC sampDesc;

		sampDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MipLODBias = 0.0f;
		sampDesc.MaxAnisotropy = 1;
		sampDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		sampDesc.BorderColor[0] = sampDesc.BorderColor[1] = sampDesc.BorderColor[2] = sampDesc.BorderColor[3] = 0;
		sampDesc.MinLOD = 0;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

		DX::ThrowIfFailed(device->CreateSamplerState(&sampDesc, &comparisonSampler));
	}
}

void Skylighting::Reset()
{
}

void Skylighting::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void Skylighting::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void Skylighting::RestoreDefaultSettings()
{
	settings = {};
}

ID3D11ComputeShader* Skylighting::GetSkylightingCS()
{
	if (!skylightingCS) {
		logger::debug("Compiling SkylightingCS");
		skylightingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\SkylightingCS.hlsl", {}, "cs_5_0");
	}
	return skylightingCS;
}

ID3D11ComputeShader* Skylighting::GetSkylightingShadowMapCS()
{
	if (!skylightingShadowMapCS) {
		logger::debug("Compiling SkylightingCS SHADOWMAP");
		skylightingShadowMapCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\SkylightingCS.hlsl", { { "SHADOWMAP", nullptr } }, "cs_5_0");
	}
	return skylightingShadowMapCS;
}

ID3D11ComputeShader* Skylighting::GetSkylightingBlurHorizontalCS()
{
	if (!skylightingBlurHorizontalCS) {
		logger::debug("Compiling SkylightingBlurCS HORIZONTAL");
		skylightingBlurHorizontalCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\SkylightingBlurCS.hlsl", { { "HORIZONTAL", nullptr } }, "cs_5_0");
	}
	return skylightingBlurHorizontalCS;
}

ID3D11ComputeShader* Skylighting::GetSkylightingBlurVerticalCS()
{
	if (!skylightingBlurVerticalCS) {
		logger::debug("Compiling SkylightingVerticalCS");
		skylightingBlurVerticalCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\SkylightingBlurCS.hlsl", {}, "cs_5_0");
	}
	return skylightingBlurVerticalCS;
}

ID3D11PixelShader* Skylighting::GetFoliagePS()
{
	if (!foliagePixelShader) {
		logger::debug("Compiling Utility.hlsl");
		foliagePixelShader = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", { { "RENDER_DEPTH", "" }, { "FOLIAGE", "" } }, "ps_5_0");
	}
	return foliagePixelShader;
}

void Skylighting::UpdateFoliage(RE::BSRenderPass* a_pass)
{
	if (inOcclusion) {
		foliage = a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim);
	}
}

void Skylighting::SkylightingShaderHacks()
{
	if (inOcclusion) {
		auto& context = State::GetSingleton()->context;

		if (foliage) {
			context->PSSetShader(GetFoliagePS(), NULL, NULL);
		} else {
			context->PSSetShader(nullptr, NULL, NULL);
		}
	}
}

void Skylighting::ClearShaderCache()
{
	if (skylightingCS) {
		skylightingCS->Release();
		skylightingCS = nullptr;
	}
	if (skylightingShadowMapCS) {
		skylightingShadowMapCS->Release();
		skylightingShadowMapCS = nullptr;
	}
	if (skylightingBlurHorizontalCS) {
		skylightingBlurHorizontalCS->Release();
		skylightingBlurHorizontalCS = nullptr;
	}
	if (skylightingBlurVerticalCS) {
		skylightingBlurVerticalCS->Release();
		skylightingBlurVerticalCS = nullptr;
	}
	if (foliagePixelShader) {
		foliagePixelShader->Release();
		foliagePixelShader = nullptr;
	}
}

void Skylighting::Compute()
{
	auto state = State::GetSingleton();
	auto& context = state->context;

	if (!settings.EnableSkylighting) {
		float clear[4] = { 1, 1, 1, 1 };
		context->ClearUnorderedAccessViewFloat(GetSingleton()->skylightingTexture->uav.get(), clear);
		return;
	}

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto dispatchCount = Util::GetScreenDispatchCount();

	bool temporal = Util::GetTemporal();

	{
		PerFrameCB data{};
		data.OcclusionViewProj = viewProjMat;
		data.EyePosition = { eyePosition.x, eyePosition.y, eyePosition.z, 0 };

		auto shadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto shadowDirLight = (RE::BSShadowDirectionalLight*)shadowSceneNode->GetRuntimeData().shadowDirLight;
		bool dirShadow = shadowDirLight && shadowDirLight->shadowLightIndex == 0;

		if (dirShadow) {
			data.ShadowDirection = float4(shadowDirLight->lightDirection.x, shadowDirLight->lightDirection.y, shadowDirLight->lightDirection.z, 0);
		}

		data.BufferDim.x = state->screenWidth;
		data.BufferDim.y = state->screenHeight;
		data.BufferDim.z = 1.0f / data.BufferDim.x;
		data.BufferDim.w = 1.0f / data.BufferDim.y;

		data.CameraData = Util::GetCameraData();

		data.FrameCount = temporal ? RE::BSGraphics::State::GetSingleton()->uiFrameCount : 0;

		perFrameCB->Update(data);
	}

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto terrainBlending = TerrainBlending::GetSingleton();

	ID3D11ShaderResourceView* srvs[5]{
		terrainBlending->loaded ? terrainBlending->blendedDepthTexture16->srv.get() : depth.depthSRV,
		Deferred::GetSingleton()->shadowView,
		Deferred::GetSingleton()->perShadow->srv.get(),
		noiseView,
		occlusionTexture->srv.get()
	};

	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	ID3D11UnorderedAccessView* uavs[1]{ skylightingTexture->uav.get() };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	auto buffer = perFrameCB->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	ID3D11SamplerState* samplers[2] = { Deferred::GetSingleton()->linearSampler, comparisonSampler };
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	{
		REL::Relocation<ID3D11Buffer**> perFrame{ REL::RelocationID(524768, 411384) };
		ID3D11Buffer* buffers[1] = { *perFrame.get() };

		context->CSSetConstantBuffers(12, ARRAYSIZE(buffers), buffers);
	}

	context->CSSetShader(settings.HeightSkylighting ? GetSkylightingCS() : GetSkylightingShadowMapCS(), nullptr, 0);

	context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

	srvs[0] = nullptr;
	srvs[1] = nullptr;
	srvs[2] = nullptr;
	srvs[3] = nullptr;
	srvs[4] = nullptr;
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	uavs[0] = nullptr;
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	samplers[0] = nullptr;
	samplers[1] = nullptr;
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	context->CSSetShader(nullptr, nullptr, 0);
}

void Skylighting::ComputeBlur(bool a_horizontal)
{
	auto state = State::GetSingleton();
	auto& context = state->context;

	if (!settings.EnableSkylighting) {
		return;
	}

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto dispatchCount = Util::GetScreenDispatchCount();

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	auto terrainBlending = TerrainBlending::GetSingleton();

	ID3D11ShaderResourceView* srvs[2]{
		terrainBlending->loaded ? terrainBlending->blendedDepthTexture16->srv.get() : depth.depthSRV,
		(a_horizontal ? skylightingTexture : skylightingTempTexture)->srv.get()
	};

	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	ID3D11UnorderedAccessView* uavs[1]{
		(!a_horizontal ? skylightingTexture : skylightingTempTexture)->uav.get()
	};

	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	auto buffer = perFrameCB->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	ID3D11SamplerState* samplers[1] = { Deferred::GetSingleton()->linearSampler };
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	{
		REL::Relocation<ID3D11Buffer**> perFrame{ REL::RelocationID(524768, 411384) };
		ID3D11Buffer* buffers[1] = { *perFrame.get() };

		context->CSSetConstantBuffers(12, ARRAYSIZE(buffers), buffers);
	}

	context->CSSetShader(a_horizontal ? GetSkylightingBlurHorizontalCS() : GetSkylightingBlurVerticalCS(), nullptr, 0);

	context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

	srvs[0] = nullptr;
	srvs[1] = nullptr;
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	uavs[0] = nullptr;
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	samplers[0] = nullptr;
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	context->CSSetShader(nullptr, nullptr, 0);
}

void Skylighting::Prepass()
{
	Compute();
	ComputeBlur(true);
	ComputeBlur(false);
	Bind();
}

void Skylighting::Bind()
{
	auto state = State::GetSingleton();
	auto& context = state->context;

	auto buffer = perFrameCB->CB();
	context->PSSetConstantBuffers(8, 1, &buffer);

	ID3D11ShaderResourceView* srvs[2]{
		occlusionTexture->srv.get(),
		skylightingTexture->srv.get(),
	};

	context->PSSetShaderResources(29, ARRAYSIZE(srvs), srvs);
}
