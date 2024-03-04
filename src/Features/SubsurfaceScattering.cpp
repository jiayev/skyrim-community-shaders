#include "SubsurfaceScattering.h"
#include <Util.h>

#include "State.h"
#include <ShaderCache.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SubsurfaceScattering::Settings,
	EnableCharacterLighting,
	UseLinear,
	BlurRadius,
	DepthFalloff,
	BacklightingAmount,
	Strength,
	Falloff)

void SubsurfaceScattering::DrawSettings()
{
	if (ImGui::TreeNodeEx("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Character Lighting", (bool*)&settings.EnableCharacterLighting);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Vanilla feature, not recommended.");
		}

		ImGui::Checkbox("Use Linear Space", (bool*)&settings.UseLinear);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Perform the blur in linear space, different results.");
		}

		ImGui::SliderFloat("Blur Radius", &settings.BlurRadius, 0, 3, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Blur radius.");
		}

		ImGui::SliderFloat("Depth Falloff", &settings.DepthFalloff, 0, 1, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Blur radius relative to depth.");
		}

		ImGui::SliderFloat("Backlighting Amount", &settings.BacklightingAmount, 0, 1, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Ignores depth falloff, more visually interesting.");
		}

		ImGui::ColorEdit3("Strength", (float*)&settings.Strength);
		ImGui::ColorEdit3("Falloff", (float*)&settings.Falloff);

		ImGui::Spacing();
		ImGui::Spacing();

		ImGui::TreePop();
	}
}

float3 SubsurfaceScattering::Gaussian(float variance, float r)
{
	/**
     * We use a falloff to modulate the shape of the profile. Big falloffs
     * spreads the shape making it wider, while small falloffs make it
     * narrower.
     */
	float falloff[3] = { settings.Falloff.x, settings.Falloff.y, settings.Falloff.z };
	float g[3];
	for (int i = 0; i < 3; i++) {
		float rr = r / (0.001f + falloff[i]);
		g[i] = exp((-(rr * rr)) / (2.0f * variance)) / (2.0f * 3.14f * variance);
	}
	return float3(g[0], g[1], g[2]);
}

float3 SubsurfaceScattering::Profile(float r)
{
	/**
     * We used the red channel of the original skin profile defined in
     * [d'Eon07] for all three channels. We noticed it can be used for green
     * and blue channels (scaled using the falloff parameter) without
     * introducing noticeable differences and allowing for total control over
     * the profile. For example, it allows to create blue SSS gradients, which
     * could be useful in case of rendering blue creatures.
     */
	return  // 0.233f * gaussian(0.0064f, r) + /* We consider this one to be directly bounced light, accounted by the strength parameter (see @STRENGTH) */
		0.100f * Gaussian(0.0484f, r) +
		0.118f * Gaussian(0.187f, r) +
		0.113f * Gaussian(0.567f, r) +
		0.358f * Gaussian(1.99f, r) +
		0.078f * Gaussian(7.41f, r);
}

void SubsurfaceScattering::CalculateKernel()
{
	uint nSamples = 33;

	const float RANGE = nSamples > 20 ? 3.0f : 2.0f;
	const float EXPONENT = 2.0f;

	// Calculate the offsets:
	float step = 2.0f * RANGE / (nSamples - 1);
	for (uint i = 0; i < nSamples; i++) {
		float o = -RANGE + float(i) * step;
		float sign = o < 0.0f ? -1.0f : 1.0f;
		kernel[i].w = RANGE * sign * abs(pow(o, EXPONENT)) / pow(RANGE, EXPONENT);
	}

	// Calculate the weights:
	for (uint i = 0; i < nSamples; i++) {
		float w0 = i > 0 ? abs(kernel[i].w - kernel[i - 1].w) : 0.0f;
		float w1 = i < nSamples - 1 ? abs(kernel[i].w - kernel[i + 1].w) : 0.0f;
		float area = (w0 + w1) / 2.0f;
		float3 t = area * Profile(kernel[i].w);
		kernel[i].x = t.x;
		kernel[i].y = t.y;
		kernel[i].z = t.z;
	}

	// We want the offset 0.0 to come first:
	float4 t = kernel[nSamples / 2];
	for (uint i = nSamples / 2; i > 0; i--)
		kernel[i] = kernel[i - 1];
	kernel[0] = t;

	// Calculate the sum of the weights, we will need to normalize them below:
	float3 sum = float3(0.0f, 0.0f, 0.0f);
	for (uint i = 0; i < nSamples; i++)
		sum += float3(kernel[i]);

	// Normalize the weights:
	for (uint i = 0; i < nSamples; i++) {
		kernel[i].x /= sum.x;
		kernel[i].y /= sum.y;
		kernel[i].z /= sum.z;
	}

	// Tweak them using the desired strength. The first one is:
	//     lerp(1.0, kernel[0].rgb, strength)
	kernel[0].x = (1.0f - settings.Strength.x) * 1.0f + settings.Strength.x * kernel[0].x;
	kernel[0].y = (1.0f - settings.Strength.y) * 1.0f + settings.Strength.y * kernel[0].y;
	kernel[0].z = (1.0f - settings.Strength.z) * 1.0f + settings.Strength.z * kernel[0].z;

	// The others:
	//     lerp(0.0, kernel[0].rgb, strength)
	for (uint i = 1; i < nSamples; i++) {
		kernel[i].x *= settings.Strength.x;
		kernel[i].y *= settings.Strength.y;
		kernel[i].z *= settings.Strength.z;
	}
}

void SubsurfaceScattering::DrawSSSWrapper(bool)
{
	if (!SIE::ShaderCache::Instance().IsEnabled())
		return;

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

	ID3D11ShaderResourceView* srvs[8];
	context->PSGetShaderResources(0, 8, srvs);

	ID3D11ShaderResourceView* srvsCS[8];
	context->CSGetShaderResources(0, 8, srvsCS);

	ID3D11UnorderedAccessView* uavsCS[8];
	context->CSGetUnorderedAccessViews(0, 8, uavsCS);

	ID3D11UnorderedAccessView* nullUavs[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 8, nullUavs, nullptr);

	ID3D11ShaderResourceView* nullSrvs[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, 8, nullSrvs);
	context->CSSetShaderResources(0, 8, nullSrvs);

	ID3D11RenderTargetView* views[8];
	ID3D11DepthStencilView* dsv;
	context->OMGetRenderTargets(8, views, &dsv);

	ID3D11RenderTargetView* nullViews[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	ID3D11DepthStencilView* nullDsv = nullptr;
	context->OMSetRenderTargets(8, nullViews, nullDsv);

	DrawSSS();

	context->PSSetShaderResources(0, 8, srvs);
	context->CSSetShaderResources(0, 8, srvsCS);
	context->CSSetUnorderedAccessViews(0, 8, uavsCS, nullptr);
	context->OMSetRenderTargets(8, views, dsv);

	for (int i = 0; i < 8; i++) {
		if (srvs[i])
			srvs[i]->Release();
		if (srvsCS[i])
			srvsCS[i]->Release();
	}

	for (int i = 0; i < 8; i++) {
		if (views[i])
			views[i]->Release();
	}

	if (dsv)
		dsv->Release();
}

void SubsurfaceScattering::DrawSSS()
{
	auto viewport = RE::BSGraphics::State::GetSingleton();

	float resolutionX = blurHorizontalTemp->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
	float resolutionY = blurHorizontalTemp->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

	{
		BlurCB data{};
		std::copy(&kernel[0], &kernel[32], data.Kernel);

		data.BufferDim.x = (float)blurHorizontalTemp->desc.Width;
		data.BufferDim.y = (float)blurHorizontalTemp->desc.Height;

		data.RcpBufferDim.x = 1.0f / (float)blurHorizontalTemp->desc.Width;
		data.RcpBufferDim.y = 1.0f / (float)blurHorizontalTemp->desc.Height;
		const auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		auto bTAA = !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled :
		                                   imageSpaceManager->GetVRRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled;
		data.FrameCount = viewport->uiFrameCount * (bTAA || State::GetSingleton()->upscalerLoaded);

		auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
		auto cameraData = !REL::Module::IsVR() ? shadowState->GetRuntimeData().cameraData.getEye() :
		                                         shadowState->GetVRRuntimeData().cameraData.getEye();

		data.SSSS_FOVY = atan(1.0f / cameraData.projMat.m[0][0]) * 2.0f * (180.0f / 3.14159265359f);

		data.CameraData = Util::GetCameraData();

		data.UseLinear = settings.UseLinear;
		data.BlurRadius = settings.BlurRadius;
		data.DepthFalloff = settings.DepthFalloff;
		data.Backlighting = settings.BacklightingAmount;

		blurCB->Update(data);
	}

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	{
		ID3D11Buffer* buffer[1] = { blurCB->CB() };
		context->CSSetConstantBuffers(0, 1, buffer);

		auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		auto snowSwap = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto normals = normalsMode == 1 ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK] : renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK_SWAP];

		ID3D11ShaderResourceView* views[3];
		views[0] = snowSwap.SRV;
		views[1] = depth.depthSRV;
		views[2] = normals.SRV;

		context->CSSetShaderResources(0, 3, views);

		ID3D11UnorderedAccessView* uav = blurHorizontalTemp->uav.get();

		// Horizontal pass to temporary texture
		{
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

			auto shader = GetComputeShaderHorizontalBlur();
			context->CSSetShader(shader, nullptr, 0);

			context->Dispatch((uint32_t)std::ceil(resolutionX / 32.0f), (uint32_t)std::ceil(resolutionY / 32.0f), 1);
		}

		ID3D11ShaderResourceView* view = nullptr;
		context->CSSetShaderResources(2, 1, &view);

		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

		// Vertical pass to main texture
		{
			views[0] = blurHorizontalTemp->srv.get();
			context->CSSetShaderResources(0, 1, views);

			ID3D11UnorderedAccessView* uavs[2] = { snowSwap.UAV, normals.UAV };
			context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

			auto shader = GetComputeShaderVerticalBlur();
			context->CSSetShader(shader, nullptr, 0);

			context->Dispatch((uint32_t)std::ceil(resolutionX / 32.0f), (uint32_t)std::ceil(resolutionY / 32.0f), 1);
		}
	}

	ID3D11Buffer* buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	ID3D11ShaderResourceView* views[3]{ nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, 3, views);

	ID3D11UnorderedAccessView* uavs[2]{ nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);
}

void SubsurfaceScattering::Draw(const RE::BSShader* a_shader, const uint32_t)
{
	if (a_shader->shaderType.get() == RE::BSShader::Type::Lighting) {
		auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
		GET_INSTANCE_MEMBER(alphaTestEnabled, state)
		if (normalsMode == 0 && alphaTestEnabled) {
			auto renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto context = renderer->GetRuntimeData().context;

			ID3D11RenderTargetView* views[3];
			ID3D11DepthStencilView* dsv;
			context->OMGetRenderTargets(3, views, &dsv);

			auto normals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK];
			auto normalsSwap = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK_SWAP];

			if (views[2] == normals.RTV) {
				normalsMode = 1;
			} else if (views[2] == normalsSwap.RTV) {
				normalsMode = 2;
			}

			for (int i = 0; i < 3; i++) {
				if (views[i])
					views[i]->Release();
			}

			if (dsv)
				dsv->Release();
		}
	}
}

void SubsurfaceScattering::SetupResources()
{
	{
		blurCB = new ConstantBuffer(ConstantBufferDesc<BlurCB>());
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(PerPass);
		sbDesc.ByteWidth = sizeof(PerPass);
		perPass = std::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;
		perPass->CreateSRV(srvDesc);
	}

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	{
		auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);

		blurHorizontalTemp = new Texture2D(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		main.SRV->GetDesc(&srvDesc);
		blurHorizontalTemp->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		main.UAV->GetDesc(&uavDesc);
		blurHorizontalTemp->CreateUAV(uavDesc);
	}

	{
		auto device = renderer->GetRuntimeData().forwarder;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &linearSampler));

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointSampler));
	}
}

void SubsurfaceScattering::Reset()
{
	normalsMode = 0;

	auto& shaderManager = RE::BSShaderManager::State::GetSingleton();
	shaderManager.characterLightEnabled = SIE::ShaderCache::Instance().IsEnabled() ? settings.EnableCharacterLighting : true;

	CalculateKernel();
}

void SubsurfaceScattering::RestoreDefaultSettings()
{
	settings = {};
}

void SubsurfaceScattering::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void SubsurfaceScattering::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void SubsurfaceScattering::ClearShaderCache()
{
	if (horizontalSSBlur) {
		horizontalSSBlur->Release();
		horizontalSSBlur = nullptr;
	}
	if (verticalSSBlur) {
		verticalSSBlur->Release();
		verticalSSBlur = nullptr;
	}
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderHorizontalBlur()
{
	if (!horizontalSSBlur) {
		logger::debug("Compiling horizontalSSBlur");
		horizontalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", { { "HORIZONTAL", "" } }, "cs_5_0");
	}
	return horizontalSSBlur;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderVerticalBlur()
{
	if (!verticalSSBlur) {
		logger::debug("Compiling verticalSSBlur");
		verticalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", {}, "cs_5_0");
	}
	return verticalSSBlur;
}

void SubsurfaceScattering::PostPostLoad()
{
	Hooks::Install();
}

void SubsurfaceScattering::OverrideFirstPersonRenderTargets()
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(renderTargets, state)
	GET_INSTANCE_MEMBER(setRenderTargetMode, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)
	GET_INSTANCE_MEMBER(alphaBlendMode, state)
	GET_INSTANCE_MEMBER(alphaBlendModeExtra, state)
	GET_INSTANCE_MEMBER(alphaBlendWriteMode, state)

	renderTargets[2] = normalsMode == 1 ? RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK : RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK_SWAP;
	setRenderTargetMode[2] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	alphaBlendMode = 0;
	alphaBlendModeExtra = 0;
	alphaBlendWriteMode = 1;
	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
}
