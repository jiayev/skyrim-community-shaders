#include "ScreenSpaceShadows.h"

#include "Deferred.h"
#include "State.h"
#include "Util.h"

#pragma warning(push)
#pragma warning(disable: 4838 4244)
#include "ScreenSpaceShadows/bend_sss_cpu.h"
#pragma warning(pop)

using RE::RENDER_TARGETS;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::BendSettings,
	Enable,
	EnableNormalMappingShadows,
	SampleCount,
	SurfaceThickness,
	BilinearThreshold,
	ShadowContrast)

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable", (bool*)&bendSettings.Enable);
		ImGui::Checkbox("Enable Normal Mapping Shadows", (bool*)&bendSettings.EnableNormalMappingShadows);
		ImGui::SliderInt("Sample Count", (int*)&bendSettings.SampleCount, 1, 4);

		ImGui::SliderFloat("SurfaceThickness", &bendSettings.SurfaceThickness, 0.005f, 0.05f);
		ImGui::SliderFloat("BilinearThreshold", &bendSettings.BilinearThreshold, 0.02f, 1.0f);
		ImGui::SliderFloat("ShadowContrast", &bendSettings.ShadowContrast, 0.0f, 4.0f);

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void ScreenSpaceShadows::ClearShaderCache()
{
	if (raymarchCS) {
		raymarchCS->Release();
		raymarchCS = nullptr;
	}
	if (raymarchRightCS) {
		raymarchRightCS->Release();
		raymarchRightCS = nullptr;
	}
	if (normalMappingShadowsCS) {
		normalMappingShadowsCS->Release();
		normalMappingShadowsCS = nullptr;
	}
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarch()
{
	static uint sampleCount = bendSettings.SampleCount;

	if (sampleCount != bendSettings.SampleCount) {
		sampleCount = bendSettings.SampleCount;
		if (raymarchCS) {
			raymarchCS->Release();
			raymarchCS = nullptr;
		}
	}

	if (!raymarchCS) {
		logger::debug("Compiling RaymarchCS");
		raymarchCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", { { "SAMPLE_COUNT", std::format("{}", sampleCount * 64).c_str() } }, "cs_5_0");
	}
	return raymarchCS;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarchRight()
{
	static uint sampleCount = bendSettings.SampleCount;

	if (sampleCount != bendSettings.SampleCount) {
		sampleCount = bendSettings.SampleCount;
		if (raymarchRightCS) {
			raymarchRightCS->Release();
			raymarchRightCS = nullptr;
		}
	}

	if (!raymarchRightCS) {
		logger::debug("Compiling RaymarchCS RIGHT");
		raymarchRightCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", { { "SAMPLE_COUNT", std::format("{}", sampleCount * 64).c_str() }, { "RIGHT", "" } }, "cs_5_0");
	}
	return raymarchRightCS;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeNormalMappingShadows()
{
	if (!normalMappingShadowsCS) {
		logger::debug("Compiling NormalMappingShadowsCS");
		normalMappingShadowsCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\NormalMappingShadowsCS.hlsl", {}, "cs_5_0");
	}
	return normalMappingShadowsCS;
}

void ScreenSpaceShadows::DrawShadows()
{
	if (!bendSettings.Enable)
		return;

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& context = State::GetSingleton()->context;

	auto shadowState = State::GetSingleton()->shadowState;
	auto viewport = RE::BSGraphics::State::GetSingleton();

	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto state = State::GetSingleton();

	auto& directionNi = dirLight->GetWorldDirection();
	float3 light = { directionNi.x, directionNi.y, directionNi.z };
	light.Normalize();
	float4 lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);

	Matrix viewProjMat = !REL::Module::IsVR() ?
	                         shadowState->GetRuntimeData().cameraData.getEye().viewProjMat :
	                         shadowState->GetVRRuntimeData().cameraData.getEye().viewProjMat;

	lightProjection = DirectX::SimpleMath::Vector4::Transform(lightProjection, viewProjMat);
	float lightProjectionF[4] = { lightProjection.x, lightProjection.y, lightProjection.z, lightProjection.w };

	int viewportSize[2] = { (int)state->screenWidth, (int)state->screenHeight };

	if (REL::Module::IsVR())
		viewportSize[0] /= 2;

	int minRenderBounds[2] = { 0, 0 };
	int maxRenderBounds[2] = {
		(int)((float)viewportSize[0] * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale),
		(int)((float)viewportSize[1] * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale)
	};

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	context->CSSetShaderResources(0, 1, &depth.depthSRV);

	auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	context->CSSetUnorderedAccessViews(0, 1, &shadowMask.UAV, nullptr);

	context->CSSetSamplers(0, 1, &pointBorderSampler);

	auto buffer = raymarchCB->CB();
	context->CSSetConstantBuffers(1, 1, &buffer);

	context->CSSetShader(GetComputeRaymarch(), nullptr, 0);

	auto dispatchList = Bend::BuildDispatchList(lightProjectionF, viewportSize, minRenderBounds, maxRenderBounds);

	for (int i = 0; i < dispatchList.DispatchCount; i++) {
		auto dispatchData = dispatchList.Dispatch[i];

		RaymarchCB data{};
		data.LightCoordinate[0] = dispatchList.LightCoordinate_Shader[0];
		data.LightCoordinate[1] = dispatchList.LightCoordinate_Shader[1];
		data.LightCoordinate[2] = dispatchList.LightCoordinate_Shader[2];
		data.LightCoordinate[3] = dispatchList.LightCoordinate_Shader[3];

		data.WaveOffset[0] = dispatchData.WaveOffset_Shader[0];
		data.WaveOffset[1] = dispatchData.WaveOffset_Shader[1];

		data.FarDepthValue = 1.0f;
		data.NearDepthValue = 0.0f;

		data.InvDepthTextureSize[0] = 1.0f / (float)viewportSize[0];
		data.InvDepthTextureSize[1] = 1.0f / (float)viewportSize[1];

		data.settings = bendSettings;

		raymarchCB->Update(data);

		context->Dispatch(dispatchData.WaveCount[0], dispatchData.WaveCount[1], dispatchData.WaveCount[2]);
	}

	if (REL::Module::IsVR()) {
		lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);

		viewProjMat = shadowState->GetVRRuntimeData().cameraData.getEye(1).viewProjMat;

		lightProjection = DirectX::SimpleMath::Vector4::Transform(lightProjection, viewProjMat);

		float lightProjectionRightF[4] = { lightProjection.x, lightProjection.y, lightProjection.z, lightProjection.w };

		context->CSSetShader(GetComputeRaymarchRight(), nullptr, 0);

		dispatchList = Bend::BuildDispatchList(lightProjectionRightF, viewportSize, minRenderBounds, maxRenderBounds);

		for (int i = 0; i < dispatchList.DispatchCount; i++) {
			auto dispatchData = dispatchList.Dispatch[i];

			RaymarchCB data{};
			data.LightCoordinate[0] = dispatchList.LightCoordinate_Shader[0];
			data.LightCoordinate[1] = dispatchList.LightCoordinate_Shader[1];
			data.LightCoordinate[2] = dispatchList.LightCoordinate_Shader[2];
			data.LightCoordinate[3] = dispatchList.LightCoordinate_Shader[3];

			data.WaveOffset[0] = dispatchData.WaveOffset_Shader[0];
			data.WaveOffset[1] = dispatchData.WaveOffset_Shader[1];

			data.FarDepthValue = 1.0f;
			data.NearDepthValue = 0.0f;

			data.InvDepthTextureSize[0] = 1.0f / (float)viewportSize[0];
			data.InvDepthTextureSize[1] = 1.0f / (float)viewportSize[1];

			data.settings = bendSettings;

			raymarchCB->Update(data);

			context->Dispatch(dispatchData.WaveCount[0], dispatchData.WaveCount[1], dispatchData.WaveCount[2]);
		}
	}

	ID3D11ShaderResourceView* views[1]{ nullptr };
	context->CSSetShaderResources(0, 1, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11SamplerState* sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);

	if (bendSettings.EnableNormalMappingShadows)
		DrawNormalMappingShadows();
}

void ScreenSpaceShadows::DrawNormalMappingShadows()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& context = State::GetSingleton()->context;

	{
		auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
		auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		auto masks = renderer->GetRuntimeData().renderTargets[MASKS];

		ID3D11ShaderResourceView* srvs[3]{ normalRoughness.SRV, depth.depthSRV, masks.SRV };
		context->CSSetShaderResources(0, 3, srvs);

		auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
		ID3D11UnorderedAccessView* uavs[1]{ shadowMask.UAV };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		auto shader = GetComputeNormalMappingShadows();
		context->CSSetShader(shader, nullptr, 0);

		auto state = State::GetSingleton();
		auto viewport = RE::BSGraphics::State::GetSingleton();

		float resolutionX = state->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		float resolutionY = state->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

		uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 32.0f);
		uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 32.0f);

		context->Dispatch(dispatchX, dispatchY, 1);
	}

	ID3D11ShaderResourceView* views[3]{ nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, 3, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	ID3D11SamplerState* sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	context->CSSetShader(nullptr, nullptr, 0);
}

void ScreenSpaceShadows::Draw(const RE::BSShader*, const uint32_t)
{
}

void ScreenSpaceShadows::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		bendSettings = o_json[GetName()];

	Feature::Load(o_json);
}

void ScreenSpaceShadows::Save(json& o_json)
{
	o_json[GetName()] = bendSettings;
}

void ScreenSpaceShadows::RestoreDefaultSettings()
{
	bendSettings = {};
}

void ScreenSpaceShadows::SetupResources()
{
	raymarchCB = new ConstantBuffer(ConstantBufferDesc<RaymarchCB>());

	{
		auto& device = State::GetSingleton()->device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		samplerDesc.BorderColor[0] = 1.0f;
		samplerDesc.BorderColor[1] = 1.0f;
		samplerDesc.BorderColor[2] = 1.0f;
		samplerDesc.BorderColor[3] = 1.0f;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointBorderSampler));
	}
}

void ScreenSpaceShadows::Reset()
{
}
