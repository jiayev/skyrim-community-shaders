#include "LocalExposure.h"

#include "I18n/I18n.h"
#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LocalExposure::Settings,
	Exposure,
	Strength,
	HighlightContrast,
	ShadowContrast,
	DetailStrength,
	BaseBlend,
	BaseMip,
	MiddleGreyBias,
	HighlightThreshold,
	ShadowThreshold,
	HighlightThresholdStrength,
	ShadowThresholdStrength)

void LocalExposure::DrawSettings()
{
	ImGui::SliderFloat(T("feature.post_processing.local_exposure.exposure", "Exposure"), &settings.Exposure, 0.f, 4.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.local_exposure.manual_brightness_normalization_used_when_histogram_auto_exposure", "Manual brightness normalization used when Histogram Auto Exposure is disabled. Higher values make the scene behave brighter."));

	ImGui::SliderFloat(T("feature.post_processing.local_exposure.strength", "Strength"), &settings.Strength, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.local_exposure.strength_tooltip", "Blends the local adjustment with the globally exposed image."));

	ImGui::SliderFloat(T("feature.post_processing.local_exposure.highlight_contrast", "Highlight Contrast"), &settings.HighlightContrast, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.local_exposure.highlight_contrast_tooltip", "Controls base-layer contrast above middle grey. Lower values recover more highlight range."));

	ImGui::SliderFloat(T("feature.post_processing.local_exposure.shadow_contrast", "Shadow Contrast"), &settings.ShadowContrast, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.local_exposure.shadow_contrast_tooltip", "Controls base-layer contrast below middle grey. Lower values lift shadows more strongly."));

	ImGui::SliderFloat(T("feature.post_processing.local_exposure.detail_strength", "Detail Strength"), &settings.DetailStrength, 0.f, 2.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.local_exposure.detail_strength_tooltip", "Preserves or boosts fine luminance detail separated from the base layer. 1.0 keeps the original detail contrast."));

	ImGui::SliderFloat(T("feature.post_processing.local_exposure.base_blend", "Soft Base Blend"), &settings.BaseBlend, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.local_exposure.base_blend_tooltip", "Blends the edge-aware base with a broad smooth base. Higher values reduce halos and keep large highlights natural."));

	const float maxMip = std::max(1.f, static_cast<float>(numMips > 0 ? numMips - 1 : 0));
	ImGui::SliderFloat(T("feature.post_processing.local_exposure.coarse_scale_mip", "Coarse Scale (Mip)"), &settings.BaseMip, 1.f, maxMip, "%.1f mip", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.local_exposure.largest_image_scale_used_by_the_effect_higher", "Largest image scale used by the effect. Higher values affect broader lighting regions."));

	if (ImGui::TreeNodeEx(T("feature.post_processing.local_exposure.advanced", "Advanced"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T("feature.post_processing.local_exposure.middle_grey_bias", "Middle Grey Bias"), &settings.MiddleGreyBias, -4.f, 4.f, "%+.2f EV");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(T("feature.post_processing.local_exposure.middle_grey_bias_tooltip", "Moves the tonal pivot used to separate highlight and shadow adjustments."));

		ImGui::SliderFloat(T("feature.post_processing.local_exposure.highlight_threshold", "Highlight Threshold"), &settings.HighlightThreshold, 0.f, 4.f, "%.2f EV");
		ImGui::SliderFloat(T("feature.post_processing.local_exposure.highlight_threshold_strength", "Highlight Threshold Strength"), &settings.HighlightThresholdStrength, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(T("feature.post_processing.local_exposure.highlight_threshold_tooltip", "Protects tones near middle grey before highlight compression begins."));

		ImGui::SliderFloat(T("feature.post_processing.local_exposure.shadow_threshold", "Shadow Threshold"), &settings.ShadowThreshold, 0.f, 4.f, "%.2f EV");
		ImGui::SliderFloat(T("feature.post_processing.local_exposure.shadow_threshold_strength", "Shadow Threshold Strength"), &settings.ShadowThresholdStrength, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(T("feature.post_processing.local_exposure.shadow_threshold_tooltip", "Protects tones near middle grey before shadow lifting begins."));

		ImGui::TreePop();
	}

	if (ImGui::CollapsingHeader(T("feature.post_processing.local_exposure.debug", "Debug"))) {
		static float debugRescale = .3f;
		ImGui::SliderFloat(T("feature.post_processing.local_exposure.view_resize", "View Resize"), &debugRescale, 0.f, 1.f);
		BUFFER_VIEWER_NODE_TITLE(texBaseLuminance, "Edge-aware Base Log Luminance", debugRescale);
		BUFFER_VIEWER_NODE_TITLE(texLogLuminance, "Scene Log Luminance", debugRescale);
	}
}

void LocalExposure::RestoreDefaultSettings()
{
	settings = {};
}

void LocalExposure::LoadSettings(json& o_json)
{
	settings = o_json;
}

void LocalExposure::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LocalExposure::SetupResources()
{
	auto renderer = globals::game::renderer;

	// Get screen dimensions from game render target
	auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];
	D3D11_TEXTURE2D_DESC mainDesc;
	gameTexMainCopy.texture->GetDesc(&mainDesc);

	uint fullW = mainDesc.Width;
	uint fullH = mainDesc.Height;
	// Calculate mip count for the log-luminance pyramid.
	numMips = 1;
	{
		uint w = fullW, h = fullH;
		while (w > 1 && h > 1 && numMips < s_MaxMips) {
			w = (w + 1) / 2;
			h = (h + 1) / 2;
			numMips++;
		}
	}

	auto createMipViews = [](Texture2D& texture,
							  DXGI_FORMAT format,
							  uint mipCount,
							  std::array<winrt::com_ptr<ID3D11ShaderResourceView>, s_MaxMips>& srvs,
							  std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, s_MaxMips>& uavs) {
		auto device = globals::d3d::device;
		for (uint i = 0; i < mipCount; i++) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = i;
			srvDesc.Texture2D.MipLevels = 1;
			DX::ThrowIfFailed(device->CreateShaderResourceView(texture.resource.get(), &srvDesc, srvs[i].put()));

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = i;
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(texture.resource.get(), &uavDesc, uavs[i].put()));
		}
	};

	// Create the log-luminance pyramid.
	{
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = fullW;
		texDesc.Height = fullH;
		texDesc.MipLevels = numMips;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		texLogLuminance = eastl::make_unique<Texture2D>(texDesc, "LocalExposure Log Luminance");

		D3D11_SHADER_RESOURCE_VIEW_DESC fullSrvDesc = {};
		fullSrvDesc.Format = texDesc.Format;
		fullSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		fullSrvDesc.Texture2D.MostDetailedMip = 0;
		fullSrvDesc.Texture2D.MipLevels = numMips;
		texLogLuminance->CreateSRV(fullSrvDesc);

		createMipViews(*texLogLuminance, DXGI_FORMAT_R16_FLOAT, numMips, logLuminanceMipSRVs, logLuminanceMipUAVs);
	}

	// Create the edge-aware luminance grid.
	{
		D3D11_TEXTURE3D_DESC texDesc = {};
		texDesc.Width = (fullW + s_GridTileSize - 1) / s_GridTileSize;
		texDesc.Height = (fullH + s_GridTileSize - 1) / s_GridTileSize;
		texDesc.Depth = s_GridDepth;
		texDesc.MipLevels = 1;
		texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		texLuminanceGrid = eastl::make_unique<Texture3D>(texDesc, "LocalExposure Luminance Grid");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MostDetailedMip = 0;
		srvDesc.Texture3D.MipLevels = 1;
		texLuminanceGrid->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
		uavDesc.Texture3D.MipSlice = 0;
		uavDesc.Texture3D.FirstWSlice = 0;
		uavDesc.Texture3D.WSize = texDesc.Depth;
		texLuminanceGrid->CreateUAV(uavDesc);
	}

	// Create output base-luminance texture (full resolution, R16F)
	{
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = fullW;
		texDesc.Height = fullH;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		texBaseLuminance = eastl::make_unique<Texture2D>(texDesc, "LocalExposure Base Luminance");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R16_FLOAT;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		texBaseLuminance->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R16_FLOAT;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		texBaseLuminance->CreateUAV(uavDesc);
	}

	// Create linear sampler
	{
		D3D11_SAMPLER_DESC sampDesc = {};
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

		auto device = globals::d3d::device;
		DX::ThrowIfFailed(device->CreateSamplerState(&sampDesc, linearSampler.put()));
	}

	// Create constant buffer
	localExposureCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<LocalExposureCB>());

	CompileComputeShaders();
}

void LocalExposure::ClearShaderCache()
{
	const auto shaderPtrs = std::array{
		&setupCS, &downsampleCS, &gridCS, &resolveCS
	};

	for (auto shader : shaderPtrs)
		if ((*shader)) {
			(*shader)->Release();
			shader->detach();
		}

	CompileComputeShaders();
}

void LocalExposure::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry;
	};

	std::vector<ShaderCompileInfo> shaderInfos = {
		{ &setupCS, "localexposure.cs.hlsl", {}, "CSSetupLogLuminance" },
		{ &downsampleCS, "localexposure.cs.hlsl", {}, "CSDownsampleLogLuminance" },
		{ &gridCS, "localexposure.cs.hlsl", {}, "CSBuildLuminanceGrid" },
		{ &resolveCS, "localexposure.cs.hlsl", {}, "CSResolveBaseLuminance" },
	};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\LocalExposure") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}
}

void LocalExposure::Draw(TextureInfo& inout_tex)
{
	auto context = globals::d3d::context;
	auto state = globals::state;

	state->BeginPerfEvent("Local Exposure");

	// Get dimensions
	D3D11_TEXTURE2D_DESC mainDesc;
	inout_tex.tex->GetDesc(&mainDesc);
	uint fullW = mainDesc.Width;
	uint fullH = mainDesc.Height;
	const uint maxMip = numMips > 0 ? numMips - 1 : 0;
	const float baseMip = std::clamp(settings.BaseMip, 0.f, (float)maxMip);
	const uint activeMip = std::min((uint)std::ceil(baseMip), maxMip);

	// Update constant buffer
	LocalExposureCB cbData = {
		.ManualExposure = settings.Exposure,
		.Strength = std::clamp(settings.Strength, 0.f, 1.f),
		.HighlightContrast = std::clamp(settings.HighlightContrast, 0.f, 1.f),
		.ShadowContrast = std::clamp(settings.ShadowContrast, 0.f, 1.f),
		.DetailStrength = std::clamp(settings.DetailStrength, 0.f, 2.f),
		.BaseBlend = std::clamp(settings.BaseBlend, 0.f, 1.f),
		.BaseMip = baseMip,
		.MiddleGreyBias = settings.MiddleGreyBias,
		.HighlightThreshold = std::max(settings.HighlightThreshold, 0.f),
		.ShadowThreshold = std::max(settings.ShadowThreshold, 0.f),
		.HighlightThresholdStrength = std::clamp(settings.HighlightThresholdStrength, 0.f, 1.f),
		.ShadowThresholdStrength = std::clamp(settings.ShadowThresholdStrength, 0.f, 1.f),
		.InputWidth = fullW,
		.InputHeight = fullH,
		.ActiveMipCount = activeMip + 1,
		.Padding0 = 0,
		.LogLuminanceMin = -13.f,
		.LogLuminanceMax = 18.f,
		.Padding1 = {},
	};
	localExposureCB->Update(cbData);

	ID3D11Buffer* cb = localExposureCB->CB();
	context->CSSetConstantBuffers(1, 1, &cb);

	ID3D11SamplerState* sampler = linearSampler.get();
	context->CSSetSamplers(0, 1, &sampler);

	std::array<ID3D11ShaderResourceView*, 3> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 2> uavs = { nullptr };

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	auto mipDim = [](uint dim, uint mip) {
		return std::max(1u, (dim + ((1u << mip) - 1u)) >> mip);
	};

	// === Pass 1: Set up scene log luminance ===
	{
		globals::profiler->BeginPass("PostProcessing::LocalExposure::Setup");
		state->BeginPerfEvent("Setup Log Luminance");

		srvs[0] = inout_tex.srv;
		uavs[0] = logLuminanceMipUAVs[0].get();
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(setupCS.get(), nullptr, 0);
		context->Dispatch((fullW + 7) >> 3, (fullH + 7) >> 3, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	// === Pass 2: Build the broad-base mip chain ===
	{
		globals::profiler->BeginPass("PostProcessing::LocalExposure::Pyramid");
		state->BeginPerfEvent("Build Luminance Pyramid");

		for (uint i = 1; i <= activeMip; i++) {
			srvs[1] = logLuminanceMipSRVs[i - 1].get();
			uavs[0] = logLuminanceMipUAVs[i].get();
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(downsampleCS.get(), nullptr, 0);

			uint mipW = mipDim(fullW, i);
			uint mipH = mipDim(fullH, i);
			context->Dispatch((mipW + 7) >> 3, (mipH + 7) >> 3, 1);
			resetViews();
		}

		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	// === Pass 3: Build the edge-aware luminance grid ===
	{
		globals::profiler->BeginPass("PostProcessing::LocalExposure::Grid");
		state->BeginPerfEvent("Build Edge-aware Grid");

		srvs[1] = logLuminanceMipSRVs[0].get();
		uavs[1] = texLuminanceGrid->uav.get();
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(gridCS.get(), nullptr, 0);
		context->Dispatch(texLuminanceGrid->desc.Width, texLuminanceGrid->desc.Height, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	// === Pass 4: Resolve the base layer ===
	{
		globals::profiler->BeginPass("PostProcessing::LocalExposure::Resolve");
		state->BeginPerfEvent("Resolve Base Luminance");

		srvs[1] = texLogLuminance->srv.get();
		srvs[2] = texLuminanceGrid->srv.get();
		uavs[0] = texBaseLuminance->uav.get();
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(resolveCS.get(), nullptr, 0);
		context->Dispatch((fullW + 7) >> 3, (fullH + 7) >> 3, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	// Cleanup
	context->CSSetShader(nullptr, nullptr, 0);
	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	// NOTE: We do not modify inout_tex. Composite consumes the base luminance map.
	state->EndPerfEvent();
}
