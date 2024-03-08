#include "SubsurfaceScattering.h"
#include <Util.h>

#include "State.h"
#include <ShaderCache.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SubsurfaceScattering::DiffusionProfile,
	BlurRadius, Thickness, Strength, Falloff)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SubsurfaceScattering::Settings,
	EnableCharacterLighting,
	BaseProfile,
	HumanProfile)

void SubsurfaceScattering::DrawSettings()
{
	if (ImGui::TreeNodeEx("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Character Lighting", (bool*)&settings.EnableCharacterLighting);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Vanilla feature, not recommended.");
		}

		if (ImGui::TreeNodeEx("Base Profile", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat("Blur Radius", &settings.BaseProfile.BlurRadius, 0, 3, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Blur radius.");
			}

			ImGui::SliderFloat("Thickness", &settings.BaseProfile.Thickness, 0, 3, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Blur radius relative to depth.");
			}

			ImGui::ColorEdit3("Strength", (float*)&settings.BaseProfile.Strength);
			ImGui::ColorEdit3("Falloff", (float*)&settings.BaseProfile.Falloff);

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Human Profile", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat("Blur Radius", &settings.HumanProfile.BlurRadius, 0, 3, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Blur radius.");
			}

			ImGui::SliderFloat("Thickness", &settings.HumanProfile.Thickness, 0, 3, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Blur radius relative to depth.");
			}

			ImGui::ColorEdit3("Strength", (float*)&settings.HumanProfile.Strength);
			ImGui::ColorEdit3("Falloff", (float*)&settings.HumanProfile.Falloff);

			ImGui::TreePop();
		}

		ImGui::Spacing();
		ImGui::Spacing();

		ImGui::TreePop();
	}
}

float3 SubsurfaceScattering::Gaussian(DiffusionProfile& a_profile, float variance, float r)
{
	/**
     * We use a falloff to modulate the shape of the profile. Big falloffs
     * spreads the shape making it wider, while small falloffs make it
     * narrower.
     */
	float falloff[3] = { a_profile.Falloff.x, a_profile.Falloff.y, a_profile.Falloff.z };
	float g[3];
	for (int i = 0; i < 3; i++) {
		float rr = r / (0.001f + falloff[i]);
		g[i] = exp((-(rr * rr)) / (2.0f * variance)) / (2.0f * 3.14f * variance);
	}
	return float3(g[0], g[1], g[2]);
}

float3 SubsurfaceScattering::Profile(DiffusionProfile& a_profile, float r)
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
		0.100f * Gaussian(a_profile, 0.0484f, r) +
		0.118f * Gaussian(a_profile, 0.187f, r) +
		0.113f * Gaussian(a_profile, 0.567f, r) +
		0.358f * Gaussian(a_profile, 1.99f, r) +
		0.078f * Gaussian(a_profile, 7.41f, r);
}

void SubsurfaceScattering::CalculateKernel(DiffusionProfile& a_profile, Kernel& kernel)
{
	uint nSamples = SSSS_N_SAMPLES;

	const float RANGE = nSamples > 20 ? 3.0f : 2.0f;
	const float EXPONENT = 2.0f;

	// Calculate the offsets:
	float step = 2.0f * RANGE / (nSamples - 1);
	for (uint i = 0; i < nSamples; i++) {
		float o = -RANGE + float(i) * step;
		float sign = o < 0.0f ? -1.0f : 1.0f;
		kernel.Sample[i].w = RANGE * sign * abs(pow(o, EXPONENT)) / pow(RANGE, EXPONENT);
	}

	// Calculate the weights:
	for (uint i = 0; i < nSamples; i++) {
		float w0 = i > 0 ? abs(kernel.Sample[i].w - kernel.Sample[i - 1].w) : 0.0f;
		float w1 = i < nSamples - 1 ? abs(kernel.Sample[i].w - kernel.Sample[i + 1].w) : 0.0f;
		float area = (w0 + w1) / 2.0f;
		float3 t = area * Profile(a_profile, kernel.Sample[i].w);
		kernel.Sample[i].x = t.x;
		kernel.Sample[i].y = t.y;
		kernel.Sample[i].z = t.z;
	}

	// We want the offset 0.0 to come first:
	float4 t = kernel.Sample[nSamples / 2];
	for (uint i = nSamples / 2; i > 0; i--)
		kernel.Sample[i] = kernel.Sample[i - 1];
	kernel.Sample[0] = t;

	// Calculate the sum of the weights, we will need to normalize them below:
	float3 sum = float3(0.0f, 0.0f, 0.0f);
	for (uint i = 0; i < nSamples; i++)
		sum += float3(kernel.Sample[i]);

	// Normalize the weights:
	for (uint i = 0; i < nSamples; i++) {
		kernel.Sample[i].x /= sum.x;
		kernel.Sample[i].y /= sum.y;
		kernel.Sample[i].z /= sum.z;
	}

	// Tweak them using the desired strength. The first one is:
	//     lerp(1.0, kernel[0].rgb, strength)
	kernel.Sample[0].x = (1.0f - a_profile.Strength.x) * 1.0f + a_profile.Strength.x * kernel.Sample[0].x;
	kernel.Sample[0].y = (1.0f - a_profile.Strength.y) * 1.0f + a_profile.Strength.y * kernel.Sample[0].y;
	kernel.Sample[0].z = (1.0f - a_profile.Strength.z) * 1.0f + a_profile.Strength.z * kernel.Sample[0].z;

	// The others:
	//     lerp(0.0, kernel[0].rgb, strength)
	for (uint i = 1; i < nSamples; i++) {
		kernel.Sample[i].x *= a_profile.Strength.x;
		kernel.Sample[i].y *= a_profile.Strength.y;
		kernel.Sample[i].z *= a_profile.Strength.z;
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

	validMaterial = false;
}

void SubsurfaceScattering::DrawSSS()
{
	auto viewport = RE::BSGraphics::State::GetSingleton();

	float resolutionX = blurHorizontalTemp->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
	float resolutionY = blurHorizontalTemp->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

	{
		blurCBData.BufferDim.x = (float)blurHorizontalTemp->desc.Width;
		blurCBData.BufferDim.y = (float)blurHorizontalTemp->desc.Height;

		blurCBData.RcpBufferDim.x = 1.0f / (float)blurHorizontalTemp->desc.Width;
		blurCBData.RcpBufferDim.y = 1.0f / (float)blurHorizontalTemp->desc.Height;
		const auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		auto bTAA = !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled :
		                                   imageSpaceManager->GetVRRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled;
		blurCBData.FrameCount = viewport->uiFrameCount * (bTAA || State::GetSingleton()->upscalerLoaded);

		auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
		auto cameraData = !REL::Module::IsVR() ? shadowState->GetRuntimeData().cameraData.getEye() :
		                                         shadowState->GetVRRuntimeData().cameraData.getEye();

		blurCBData.SSSS_FOVY = atan(1.0f / cameraData.projMat.m[0][0]) * 2.0f * (180.0f / 3.14159265359f);

		blurCBData.CameraData = Util::GetCameraData();

		blurCBData.BaseProfile = { settings.BaseProfile.BlurRadius, settings.BaseProfile.Thickness, 0, 0 };
		blurCBData.HumanProfile = { settings.HumanProfile.BlurRadius, settings.HumanProfile.Thickness, 0, 0 };

		blurCB->Update(blurCBData);
	}

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	{
		ID3D11Buffer* buffer[1] = { blurCB->CB() };
		context->CSSetConstantBuffers(0, 1, buffer);

		auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		auto snowSwap = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto normals = renderer->GetRuntimeData().renderTargets[normalsMode];

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
		if (normalsMode == RE::RENDER_TARGET::kNONE) {
			auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
			GET_INSTANCE_MEMBER(renderTargets, state)
			normalsMode = renderTargets[2];
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
}

void SubsurfaceScattering::Reset()
{
	normalsMode = RE::RENDER_TARGET::kNONE;

	auto& shaderManager = RE::BSShaderManager::State::GetSingleton();
	shaderManager.characterLightEnabled = SIE::ShaderCache::Instance().IsEnabled() ? settings.EnableCharacterLighting : true;

	CalculateKernel(settings.BaseProfile, blurCBData.BaseKernel);
	CalculateKernel(settings.HumanProfile, blurCBData.HumanKernel);
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
	if (clearBuffer) {
		clearBuffer->Release();
		clearBuffer = nullptr;
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

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderClearBuffer()
{
	if (!clearBuffer) {
		logger::debug("Compiling clearBuffer");
		clearBuffer = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\ClearBuffer.hlsl", {}, "cs_5_0");
	}
	return clearBuffer;
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

	renderTargets[2] = normalsMode;

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	{
		auto& target = renderer->GetRuntimeData().renderTargets[renderTargets[2]];

		auto uav = target.UAV;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

		auto shader = GetComputeShaderClearBuffer();
		context->CSSetShader(shader, nullptr, 0);

		auto viewport = RE::BSGraphics::State::GetSingleton();

		float resolutionX = blurHorizontalTemp->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		float resolutionY = blurHorizontalTemp->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

		context->Dispatch((uint32_t)std::ceil(resolutionX / 32.0f), (uint32_t)std::ceil(resolutionY / 32.0f), 1);

		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	}

	setRenderTargetMode[2] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	alphaBlendMode = 0;
	alphaBlendModeExtra = 0;
	alphaBlendWriteMode = 1;
	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

	validMaterial = true;
}

void SubsurfaceScattering::BSLightingShader_SetupSkin(RE::BSRenderPass* a_pass)
{
	if (a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kFace, RE::BSShaderProperty::EShaderPropertyFlag::kFaceGenRGBTint)) {
		bool isBeastRace = true;

		auto geometry = a_pass->geometry;
		if (auto userData = geometry->GetUserData()) {
			if (auto actor = userData->As<RE::Actor>()) {
				if (auto race = actor->GetRace()) {
					static auto isBeastRaceForm = RE::TESForm::LookupByEditorID("IsBeastRace")->As<RE::BGSKeyword>();
					isBeastRace = race->HasKeyword(isBeastRaceForm);
				}
			}
		}

		static PerPass perPassData{};

		if (perPassData.ValidMaterial != (uint)validMaterial || perPassData.IsBeastRace != (uint)isBeastRace) {
			perPassData.ValidMaterial = validMaterial;
			perPassData.IsBeastRace = isBeastRace;

			auto renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto context = renderer->GetRuntimeData().context;

			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			size_t bytes = sizeof(PerPass);
			memcpy_s(mapped.pData, bytes, &perPassData, bytes);
			context->Unmap(perPass->resource.get(), 0);
		}
	}
}

void SubsurfaceScattering::Bind()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;
	ID3D11ShaderResourceView* view = perPass->srv.get();
	context->PSSetShaderResources(36, 1, &view);
	validMaterial = true;
}
