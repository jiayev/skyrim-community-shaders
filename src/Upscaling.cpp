#include "Upscaling.h"

#include "Hooks.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Upscaling::Settings,
	upscaleMode,
	upscaleModeNoDLSS,
	sharpness,
	dlssPreset);

void Upscaling::DrawSettings()
{
	if (ImGui::CollapsingHeader("Upscaling", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick)) {
		const char* upscaleModes[] = { "Temporal Anti-Aliasing", "AMD FSR 3.1", "NVIDIA DLAA" };

		if (Streamline::GetSingleton()->featureDLSS) {
			ImGui::SliderInt("Mode", (int*)&settings.upscaleMode, 0, 2, std::format("{}", upscaleModes[(uint)settings.upscaleMode]).c_str());
			settings.upscaleMode = std::min(2u, (uint)settings.upscaleMode);
		} else {
			ImGui::SliderInt("Mode", (int*)&settings.upscaleModeNoDLSS, 0, 1, std::format("{}", upscaleModes[(uint)settings.upscaleModeNoDLSS]).c_str());
			settings.upscaleModeNoDLSS = std::min(1u, (uint)settings.upscaleModeNoDLSS);
		}

		auto upscaleMode = GetUpscaleMode();

		if (upscaleMode != UpscaleMode::kTAA) {
			ImGui::SliderFloat("Sharpness", &settings.sharpness, 0.0f, 1.0f, "%.1f");
			settings.sharpness = std::clamp(settings.sharpness, 0.0f, 1.0f);
		}

		if (upscaleMode == UpscaleMode::kDLSS) {
			const char* dlssPresets[] = { "Default", "Preset A", "Preset B", "Preset C", "Preset D", "Preset E", "Preset F" };
			ImGui::SliderInt("DLSS Preset", (int*)&settings.dlssPreset, 0, 6, std::format("{}", dlssPresets[(uint)settings.dlssPreset]).c_str());
			settings.dlssPreset = std::min(6u, (uint)settings.dlssPreset);
		}
	}
}

void Upscaling::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Upscaling::LoadSettings(json& o_json)
{
	settings = o_json;
}

Upscaling::UpscaleMode Upscaling::GetUpscaleMode()
{
	auto streamline = Streamline::GetSingleton();
	return streamline->featureDLSS ? (Upscaling::UpscaleMode)settings.upscaleMode : (Upscaling::UpscaleMode)settings.upscaleModeNoDLSS;
}

void Upscaling::CheckResources()
{
	static auto previousUpscaleMode = UpscaleMode::kTAA;
	auto currentUpscaleMode = GetUpscaleMode();

	auto streamline = Streamline::GetSingleton();
	auto fidelityFX = FidelityFX::GetSingleton();

	if (previousUpscaleMode != currentUpscaleMode) {
		if (previousUpscaleMode == UpscaleMode::kTAA)
			CreateUpscalingResources();
		else if (previousUpscaleMode == UpscaleMode::kDLSS)
			streamline->DestroyDLSSResources();
		else if (previousUpscaleMode == UpscaleMode::kFSR)
			fidelityFX->DestroyFSRResources();

		if (currentUpscaleMode == UpscaleMode::kTAA)
			DestroyUpscalingResources();
		else if (previousUpscaleMode == UpscaleMode::kFSR)
			fidelityFX->DestroyFSRResources();
		else if (currentUpscaleMode == UpscaleMode::kFSR)
			fidelityFX->CreateFSRResources();

		previousUpscaleMode = currentUpscaleMode;
	}
}

ID3D11ComputeShader* Upscaling::GetRCASComputeShader()
{
	static auto previousSharpness = settings.sharpness;
	auto currentSharpness = settings.sharpness;

	if (previousSharpness != currentSharpness) {
		previousSharpness = currentSharpness;

		if (rcasCS) {
			rcasCS->Release();
			rcasCS = nullptr;
		}
	}

	if (!rcasCS) {
		logger::debug("Compiling Utility.hlsl");
		rcasCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\RCAS\\RCAS.hlsl", { { "SHARPNESS", std::format("{}", currentSharpness).c_str() } }, "cs_5_0");
	}

	return rcasCS;
}

void Upscaling::Upscale()
{
	CheckResources();

	Hooks::BSGraphics_SetDirtyStates::func(false);

	auto state = State::GetSingleton();
	state->BeginPerfEvent("Upscaling");

	auto& context = state->context;

	ID3D11ShaderResourceView* inputTextureSRV;
	context->PSGetShaderResources(0, 1, &inputTextureSRV);

	inputTextureSRV->Release();

	ID3D11RenderTargetView* outputTextureRTV;
	ID3D11DepthStencilView* dsv;
	context->OMGetRenderTargets(1, &outputTextureRTV, &dsv);
	context->OMSetRenderTargets(0, nullptr, nullptr);

	outputTextureRTV->Release();

	if (dsv)
		dsv->Release();

	ID3D11Resource* inputTextureResource;
	inputTextureSRV->GetResource(&inputTextureResource);

	ID3D11Resource* outputTextureResource;
	outputTextureRTV->GetResource(&outputTextureResource);

	context->CopyResource(upscalingTempTexture->resource.get(), inputTextureResource);

	auto upscaleMode = GetUpscaleMode();

	if (upscaleMode == UpscaleMode::kDLSS)
		Streamline::GetSingleton()->Upscale(upscalingTempTexture);
	else
		FidelityFX::GetSingleton()->Upscale(upscalingTempTexture);

	context->CopyResource(inputTextureResource, upscalingTempTexture->resource.get());

	state->EndPerfEvent();

	state->BeginPerfEvent("Sharpening");

	{
		auto dispatchCount = Util::GetScreenDispatchCount(false);

		{
			ID3D11ShaderResourceView* views[1] = { inputTextureSRV };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			ID3D11UnorderedAccessView* uavs[1] = { upscalingTempTexture->uav.get() };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetShader(GetRCASComputeShader(), nullptr, 0);

			context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
		}

		ID3D11ShaderResourceView* views[1] = { nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);
	}

	context->CopyResource(outputTextureResource, upscalingTempTexture->resource.get());

	state->EndPerfEvent();
}

void Upscaling::CreateUpscalingResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.RTV->GetDesc(&rtvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Format = texDesc.Format;
	rtvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	upscalingTempTexture = new Texture2D(texDesc);
	upscalingTempTexture->CreateSRV(srvDesc);
	upscalingTempTexture->CreateRTV(rtvDesc);
	upscalingTempTexture->CreateUAV(uavDesc);
}

void Upscaling::DestroyUpscalingResources()
{
	upscalingTempTexture->srv = nullptr;
	upscalingTempTexture->uav = nullptr;
	upscalingTempTexture->rtv = nullptr;
	upscalingTempTexture->dsv = nullptr;
	upscalingTempTexture->resource = nullptr;
	delete upscalingTempTexture;
}
