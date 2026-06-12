#include "ScreenSpaceGI.h"

#include <DirectXTex.h>

#include "Deferred.h"
#include "DynamicCubemaps.h"
#include "I18n/I18n.h"
#include "NRD.h"
#include "Skylighting.h"
#include "State.h"
#include "Upscaling.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.screen_space_gi."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceGI::Settings,
	Enabled,
	EnableGI,
	EnableVanillaSSAO,
	EnableSH,
	NumSteps,
	HalfRes,
	Thickness,
	AOPower,
	GIStrength,
	UseDynamicCubemapsAsFallback,
	DiffuseCubemapMult,
	EnableREBLUR,
	Reblur)

////////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceGI::RestoreDefaultSettings()
{
	settings = {};
	recompileFlag = true;
}

void ScreenSpaceGI::DrawSettings()
{
	static bool showAdvanced;

	if (!ShadersOK())
		ImGui::TextColored({ 1, 0, 0, 1 }, "%s", T(TKEY("compute_shaders_failed_to_compile"), "Compute shaders failed to compile!"));

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("toggles"), "Toggles"));

	ImGui::Checkbox(T(TKEY("show_advanced_options"), "Show Advanced Options"), &showAdvanced);

	if (ImGui::BeginTable("Toggles", 4)) {
		ImGui::TableNextColumn();
		ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enabled_tooltip"), "Enable Screen Space Global Illumination. When disabled, all other settings are ignored."));
		}

		ImGui::TableNextColumn();
		{
			auto ilToggleGuard = Util::DisableGuard(!settings.Enabled);
			if (ImGui::Checkbox(T(TKEY("indirect_lighting"), "Indirect Lighting (IL)"), &settings.EnableGI)) {
				recompileFlag = true;
				SetupNRDResources();
			}
		}
		ImGui::TableNextColumn();
		{
			auto shGuard = Util::DisableGuard(!settings.Enabled || !settings.EnableGI);
			if (ImGui::Checkbox(T(TKEY("sh_mode"), "SH Mode"), &settings.EnableSH)) {
				recompileFlag = true;
				SetupNRDResources();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("sh_mode_tooltip"), "Use Spherical Harmonics for directional GI. Higher quality but more expensive."));
			}
		}
		ImGui::TableNextColumn();
		{
			ImGui::Checkbox(T(TKEY("vanilla_ssao"), "Vanilla SSAO"), &settings.EnableVanillaSSAO);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("vanilla_ssao_tooltip"), "Enable Skyrim's built-in SSAO. Usually disabled when using SSGI to avoid double-darkening."));
			}
		}

		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("quality_performance"), "Quality/Performance"));

	{
		auto qualityGuard = Util::DisableGuard(!settings.Enabled);

		if (ImGui::Checkbox(T(TKEY("half_resolution_checkerboard"), "Half Resolution (Checkerboard)"), &settings.HalfRes)) {
			recompileFlag = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("half_resolution_checkerboard_tooltip"), "Trace half the columns in a checkerboard pattern. NRD reconstructs the missing pixels."));
		}

		if (showAdvanced) {
			ImGui::SliderInt(T(TKEY("steps_per_slice"), "Steps Per Slice"), (int*)&settings.NumSteps, 1, 32);
		}
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("visual"), "Visual"));

	{
		auto visualGuard = Util::DisableGuard(!settings.Enabled);

		ImGui::SliderFloat(T(TKEY("ao_power"), "AO Power"), &settings.AOPower, 0.f, 6.f, "%.2f");

		{
			auto ilGuard = Util::DisableGuard(!settings.EnableGI);
			ImGui::SliderFloat(T(TKEY("il_source_brightness"), "IL Source Brightness"), &settings.GIStrength, 0.f, 6.f, "%.2f");

			ImGui::Checkbox(T(TKEY("use_dynamic_cubemaps_as_fallback"), "Use Dynamic Cubemaps as Fallback"), &settings.UseDynamicCubemapsAsFallback);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("use_dynamic_cubemaps_as_fallback_tooltip"), "Where indirect rays miss the screen, sample dynamic cubemaps for diffuse fallback."));
			}
			{
				auto cubemapGuard = Util::DisableGuard(!settings.UseDynamicCubemapsAsFallback);
				ImGui::SliderFloat(T(TKEY("diffuse_cubemap_multiplier"), "Diffuse Cubemap Multiplier"), &settings.DiffuseCubemapMult, 0.0f, 5.0f, "%.2f");
			}
		}

		if (showAdvanced) {
			ImGui::Separator();
			ImGui::SliderFloat(T(TKEY("thickness"), "Thickness"), &settings.Thickness, 0.f, 0.2f, "%.3f");
		}
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("reblur_denoiser"), "REBLUR Denoiser"));

	{
		auto denoiseGuard = Util::DisableGuard(!settings.Enabled);

		ImGui::Checkbox(T(TKEY("enable_reblur"), "Enable REBLUR"), &settings.EnableREBLUR);

		if (settings.EnableREBLUR)
			globals::features::nrd.DrawReblurSettings(settings.Reblur, showAdvanced, "ssgi_reblur");
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("debug"), "Debug"));

	if (ImGui::TreeNode(T(TKEY("buffer_viewer"), "Buffer Viewer"))) {
		static float debugRescale = .3f;
		ImGui::SliderFloat(T(TKEY("view_resize"), "View Resize"), &debugRescale, 0.f, 1.f);

		BUFFER_VIEWER_NODE(texNoise, debugRescale)
		BUFFER_VIEWER_NODE(texWorkingDepth, debugRescale)
		BUFFER_VIEWER_NODE(texPrevGeo, debugRescale)
		BUFFER_VIEWER_NODE(texRadiance, debugRescale)
		BUFFER_VIEWER_NODE(texNRDInput, debugRescale)
		BUFFER_VIEWER_NODE(texNRDOutput, debugRescale)
		if (texNRDInputSH1)
			BUFFER_VIEWER_NODE(texNRDInputSH1, debugRescale)
		if (texNRDOutputSH1)
			BUFFER_VIEWER_NODE(texNRDOutputSH1, debugRescale)

		ImGui::TreePop();
	}
}

void ScreenSpaceGI::LoadSettings(json& o_json)
{
	settings = o_json;
	recompileFlag = true;
}

void ScreenSpaceGI::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ScreenSpaceGI::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		ssgiCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSGICB>(), "SSGI::CB");
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 64,
			.Height = 64,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32_UINT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
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

		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;

		{
			texRadiance = eastl::make_unique<Texture2D>(texDesc, "SSGI::Radiance");
			texRadiance->CreateSRV(srvDesc);

			for (uint i = 0; i < 5; ++i) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
					.Format = DXGI_FORMAT_R11G11B10_FLOAT,
					.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MipSlice = i }
				};
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texRadiance->resource.get(), &mipUavDesc, uavRadiance[i].put()));
				Util::SetResourceName(uavRadiance[i].get(), "SSGI::Radiance UAV mip%u", i);
			}
		}

		texDesc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16_FLOAT;

		{
			texWorkingDepth = eastl::make_unique<Texture2D>(texDesc, "SSGI::WorkingDepth");
			texWorkingDepth->CreateSRV(srvDesc);
			for (int i = 0; i < 5; ++i) {
				uavDesc.Texture2D.MipSlice = i;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth[i].put()));
				Util::SetResourceName(uavWorkingDepth[i].get(), "SSGI::WorkingDepth UAV mip%d", i);
			}
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		{
			texNormal = eastl::make_unique<Texture2D>(texDesc, "SSGI::Normal");
			texNormal->CreateSRV(srvDesc);
			for (uint i = 0; i < 5; ++i) {
				uavDesc.Texture2D.MipSlice = i;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texNormal->resource.get(), &uavDesc, uavNormal[i].put()));
				Util::SetResourceName(uavNormal[i].get(), "SSGI::Normal UAV mip%u", i);
			}
		}

		uavDesc.Texture2D.MipSlice = 0;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		{
			texPrevGeo = eastl::make_unique<Texture2D>(texDesc, "SSGI::PrevGeo");
			texPrevGeo->CreateSRV(srvDesc);
			texPrevGeo->CreateUAV(uavDesc);
		}

		SetupNRDResources();
	}

	logger::debug("Loading noise texture...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ "Data\\Shaders\\ScreenSpaceGI\\fast_2uges.dds" };
			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texNoise = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource), "SSGI::Noise");

		D3D11_SHADER_RESOURCE_VIEW_DESC noiseSrvDesc = {
			.Format = texNoise->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texNoise->CreateSRV(noiseSrvDesc);
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));
		Util::SetResourceName(linearClampSampler.get(), "SSGI::LinearClampSampler");

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
		Util::SetResourceName(pointClampSampler.get(), "SSGI::PointClampSampler");
	}

	CompileComputeShaders();
}

void ScreenSpaceGI::SetupNRDResources()
{
	uint32_t fullW, fullH;
	if (texRadiance) {
		fullW = texRadiance->desc.Width;
		fullH = texRadiance->desc.Height;
	} else {
		auto renderer = globals::game::renderer;
		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		D3D11_TEXTURE2D_DESC mainDesc;
		mainTex.texture->GetDesc(&mainDesc);
		fullW = mainDesc.Width;
		fullH = mainDesc.Height;
	}

	D3D11_TEXTURE2D_DESC texDesc{
		.Width = fullW,
		.Height = fullH,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};

	nrdReblur.Shutdown();

	texNRDInput = eastl::make_unique<Texture2D>(texDesc, "SSGI::NRDInput");
	texNRDInput->CreateSRV(srvDesc);
	texNRDInput->CreateUAV(uavDesc);

	texNRDOutput = eastl::make_unique<Texture2D>(texDesc, "SSGI::NRDOutput");
	texNRDOutput->CreateSRV(srvDesc);
	texNRDOutput->CreateUAV(uavDesc);

	if (settings.EnableSH) {
		texNRDInputSH1 = eastl::make_unique<Texture2D>(texDesc, "SSGI::NRDInputSH1");
		texNRDInputSH1->CreateSRV(srvDesc);
		texNRDInputSH1->CreateUAV(uavDesc);

		texNRDOutputSH1 = eastl::make_unique<Texture2D>(texDesc, "SSGI::NRDOutputSH1");
		texNRDOutputSH1->CreateSRV(srvDesc);
		texNRDOutputSH1->CreateUAV(uavDesc);
	} else {
		texNRDInputSH1.reset();
		texNRDOutputSH1.reset();
	}

	auto denoiser = settings.EnableSH ? nrd::Denoiser::REBLUR_DIFFUSE_SH : nrd::Denoiser::REBLUR_DIFFUSE;
	nrdReblur.Init(fullW, fullH, denoiser, 0);

	globals::deferred->ClearShaderCache();
}

void ScreenSpaceGI::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&prefilterDepthsCompute, &prefilterRadianceCompute, &prefilterNormalCompute, &giCompute
	};

	for (auto shader : shaderPtrs)
		*shader = nullptr;

	CompileComputeShaders();
}

void ScreenSpaceGI::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &prefilterDepthsCompute, "prefilterDepths.cs.hlsl", { { "LINEAR_FILTER", "" } } },
			{ &prefilterRadianceCompute, "prefilterRadiance.cs.hlsl", {} },
			{ &prefilterNormalCompute, "prefilterNormal.cs.hlsl", {} },
			{ &giCompute, "diffuseGI.cs.hlsl", {} },
		};

	for (auto& info : shaderInfos) {
		if (settings.EnableGI)
			info.defines.push_back({ "GI", "" });
		if (settings.EnableSH && settings.EnableGI)
			info.defines.push_back({ "SSGI_SH", "" });
		if (settings.HalfRes)
			info.defines.push_back({ "SSGI_HALF", "" });
		if (globals::features::dynamicCubemaps.loaded)
			info.defines.push_back({ "DYNAMIC_CUBEMAPS", "" });
		if (globals::features::skylighting.loaded)
			info.defines.push_back({ "SKYLIGHTING", "" });
	}

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceGI") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
}

bool ScreenSpaceGI::ShadersOK()
{
	return texNoise && prefilterDepthsCompute && prefilterRadianceCompute && prefilterNormalCompute && giCompute;
}

void ScreenSpaceGI::UpdateSB()
{
	float2 res = { (float)texRadiance->desc.Width, (float)texRadiance->desc.Height };
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = { floor(dynres.x), floor(dynres.y) };

	static float4x4 prevInvView = {};

	SSGICB data;
	{
		const auto& projMat = globals::game::frameBufferCached.GetCameraProj();
		const auto& viewMat = globals::game::frameBufferCached.GetCameraView();

		data.PrevInvViewMat = prevInvView;
		data.NDCToViewMul = { 2.0f / projMat(0, 0), -2.0f / projMat(1, 1) };
		data.NDCToViewAdd = { -1.0f / projMat(0, 0), 1.0f / projMat(1, 1) };

		prevInvView = viewMat.Invert();

		data.TexDim = res;
		data.RcpTexDim = float2(1.0f) / res;
		data.FrameDim = dynres;
		data.RcpFrameDim = float2(1.0f) / dynres;
		data.FrameIndex = globals::state->frameCount;

		data.NumSteps = settings.NumSteps;

		data.Thickness = settings.Thickness;
		data.AOPower = settings.AOPower;
		data.GIStrength = settings.GIStrength;
		data.DiffuseCubemapMult = settings.DiffuseCubemapMult;
		data.UseDynamicCubemap = (settings.UseDynamicCubemapsAsFallback && globals::features::dynamicCubemaps.loaded) ? 1u : 0u;
		data.pad0 = 0;
	}

	ssgiCB->Update(data);
}

void ScreenSpaceGI::DrawSSGI()
{
	auto context = globals::d3d::context;

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	auto& BSImagespaceShaderISSAOBlurH = imageSpaceManager->GetRuntimeData().BSImagespaceShaderISSAOBlurH;

	static bool* enableSSAO = reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(BSImagespaceShaderISSAOBlurH.get()) + 0x50LL);
	*enableSSAO = settings.EnableVanillaSSAO;

	if (!(settings.Enabled && ShadersOK())) {
		return;
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "SSGI");

	//////////////////////////////////////////////////////

	if (recompileFlag)
		ClearShaderCache();

	UpdateSB();

	//////////////////////////////////////////////////////

	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;
	auto deferred = globals::deferred;

	float2 size{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	size = Util::ConvertToDynamic(size);
	auto resolution = std::array{ (uint)size.x, (uint)size.y };

	std::array<ID3D11ShaderResourceView*, 11> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 6> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 2> samplers = { pointClampSampler.get(), linearClampSampler.get() };
	auto cb = ssgiCB->CB();

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	//////////////////////////////////////////////////////

	context->CSSetConstantBuffers(1, 1, &cb);
	auto* sharedDataBuf = globals::state->sharedDataCB->CB();
	context->CSSetConstantBuffers(5, 1, &sharedDataBuf);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	// prefilter depths
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Prefilter Depths");

		srvs.at(0) = Util::GetCurrentSceneDepthSRV();
		for (int i = 0; i < 5; ++i)
			uavs.at(i) = uavWorkingDepth[i].get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(prefilterDepthsCompute.get(), nullptr, 0);
		globals::profiler->BeginPass("ScreenSpaceGI::PrefilterDepths");
		context->Dispatch((resolution[0] + 15) >> 4, (resolution[1] + 15) >> 4, 1);
		globals::profiler->EndPass();
	}

	// Prefilter radiance mip chain (reads main RT directly)
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Prefilter Radiance");

		resetViews();
		srvs.at(0) = rts[deferred->forwardRenderTargets[0]].SRV;
		uavs.at(0) = uavRadiance[0].get();
		uavs.at(1) = uavRadiance[1].get();
		uavs.at(2) = uavRadiance[2].get();
		uavs.at(3) = uavRadiance[3].get();
		uavs.at(4) = uavRadiance[4].get();

		context->CSSetShaderResources(0, 1, srvs.data());
		context->CSSetUnorderedAccessViews(0, 5, uavs.data(), nullptr);
		context->CSSetShader(prefilterRadianceCompute.get(), nullptr, 0);
		globals::profiler->BeginPass("ScreenSpaceGI::PrefilterRadiance");
		context->Dispatch((resolution[0] + 15u) >> 4, (resolution[1] + 15u) >> 4, 1);
		globals::profiler->EndPass();
	}

	// Prefilter normals
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Prefilter Normals");

		resetViews();
		srvs.at(0) = rts[NORMALROUGHNESS].SRV;
		uavs.at(0) = uavNormal[0].get();
		uavs.at(1) = uavNormal[1].get();
		uavs.at(2) = uavNormal[2].get();
		uavs.at(3) = uavNormal[3].get();
		uavs.at(4) = uavNormal[4].get();

		context->CSSetShaderResources(0, 1, srvs.data());
		context->CSSetUnorderedAccessViews(0, 5, uavs.data(), nullptr);
		context->CSSetShader(prefilterNormalCompute.get(), nullptr, 0);
		globals::profiler->BeginPass("ScreenSpaceGI::PrefilterNormals");
		context->Dispatch((resolution[0] + 15u) >> 4, (resolution[1] + 15u) >> 4, 1);
		globals::profiler->EndPass();
	}

	// GI → NRD input
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - GI");

		auto& dynamicCubemaps = globals::features::dynamicCubemaps;
		auto& skylighting = globals::features::skylighting;

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(2) = texRadiance->srv.get();
		srvs.at(3) = texNoise->srv.get();
		if (dynamicCubemaps.loaded) {
			srvs.at(4) = dynamicCubemaps.envTexture->srv.get();
			srvs.at(5) = dynamicCubemaps.envReflectionsTexture->srv.get();
		}
		if (dynamicCubemaps.loaded && skylighting.loaded)
			srvs.at(6) = skylighting.texProbeArray->srv.get();
		srvs.at(8) = texNormal->srv.get();

		uavs.at(0) = texNRDInput->uav.get();
		uavs.at(1) = texPrevGeo->uav.get();
		if (settings.EnableSH && texNRDInputSH1)
			uavs.at(2) = texNRDInputSH1->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(giCompute.get(), nullptr, 0);

		uint dispatchX = settings.HalfRes ? (resolution[0] + 1) / 2 : resolution[0];
		uint dispatchY = resolution[1];
		globals::profiler->BeginPass("ScreenSpaceGI::GI");
		context->Dispatch((dispatchX + 7u) >> 3, (dispatchY + 7u) >> 3, 1);
		globals::profiler->EndPass();
	}

	// REBLUR diffuse denoising via core NRD service
	auto& nrdSvc = globals::features::nrd;
	if (settings.EnableREBLUR && nrdReblur.IsValid() && nrdSvc.loaded && nrdSvc.AreGuidesReady()) {
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - REBLUR");

		auto commonSettings = nrdSvc.GetCommonSettings();
		commonSettings.splitScreen = settings.Reblur.SplitScreen;
		nrdReblur.SetCommonSettings(commonSettings);

		nrdSvc.ApplyReblurSettings(reblurSettings, settings.Reblur,
			settings.HalfRes ? nrd::CheckerboardMode::WHITE : nrd::CheckerboardMode::OFF);
		nrdReblur.SetDenoiserSettings(&reblurSettings);

		nrdReblur.SetNamedSRV(nrd::ResourceType::IN_MV, nrdSvc.GetMotionVectorSRV());
		nrdReblur.SetNamedUAV(nrd::ResourceType::IN_MV, nrdSvc.GetMotionVectorUAV());
		nrdReblur.SetNamedSRV(nrd::ResourceType::IN_NORMAL_ROUGHNESS, nrdSvc.GetNormalRoughnessSRV());
		nrdReblur.SetNamedSRV(nrd::ResourceType::IN_VIEWZ, nrdSvc.GetViewZSRV());

		if (settings.EnableSH && texNRDInputSH1) {
			nrdReblur.SetNamedSRV(nrd::ResourceType::IN_DIFF_SH0, texNRDInput->srv.get());
			nrdReblur.SetNamedSRV(nrd::ResourceType::IN_DIFF_SH1, texNRDInputSH1->srv.get());
			nrdReblur.SetNamedSRV(nrd::ResourceType::OUT_DIFF_SH0, texNRDOutput->srv.get());
			nrdReblur.SetNamedUAV(nrd::ResourceType::OUT_DIFF_SH0, texNRDOutput->uav.get());
			nrdReblur.SetNamedSRV(nrd::ResourceType::OUT_DIFF_SH1, texNRDOutputSH1->srv.get());
			nrdReblur.SetNamedUAV(nrd::ResourceType::OUT_DIFF_SH1, texNRDOutputSH1->uav.get());
		} else {
			nrdReblur.SetNamedSRV(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, texNRDInput->srv.get());
			nrdReblur.SetNamedSRV(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, texNRDOutput->srv.get());
			nrdReblur.SetNamedUAV(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, texNRDOutput->uav.get());
		}

		globals::profiler->BeginPass("ScreenSpaceGI::Reblur");
		nrdReblur.Dispatch();
		globals::profiler->EndPass();
	}

	// cleanup
	resetViews();

	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);
}

ID3D11ShaderResourceView* ScreenSpaceGI::GetDiffuseOutputTexture()
{
	if (loaded && settings.Enabled && settings.EnableREBLUR && nrdReblur.IsValid() &&
		globals::features::nrd.loaded && globals::features::nrd.AreGuidesReady())
		return texNRDOutput->srv.get();
	else if (loaded && settings.Enabled)
		return texNRDInput->srv.get();
	return nullptr;
}

ID3D11ShaderResourceView* ScreenSpaceGI::GetDiffuseSH1Texture()
{
	if (!loaded || !settings.Enabled || !settings.EnableSH)
		return nullptr;
	if (settings.EnableREBLUR && nrdReblur.IsValid() && globals::features::nrd.loaded && globals::features::nrd.AreGuidesReady() && texNRDOutputSH1)
		return texNRDOutputSH1->srv.get();
	else if (texNRDInputSH1)
		return texNRDInputSH1->srv.get();
	return nullptr;
}

ScreenSpaceGI::SharedData ScreenSpaceGI::GetCommonBufferData()
{
	SharedData data;
	data.DiffuseMult = (settings.Enabled && settings.EnableGI) ? settings.GIStrength : 0.0f;
	data.DebugMode = 0;
	data.pad0 = 0;
	data.pad1 = 0;
	return data;
}

#undef I18N_KEY_PREFIX
