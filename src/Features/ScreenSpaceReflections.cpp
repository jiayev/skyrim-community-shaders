#include "ScreenSpaceReflections.h"

#include "Deferred.h"
#include "DynamicCubemaps.h"
#include "NRD.h"
#include "Skylighting.h"
#include "State.h"
#include "Upscaling.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceReflections::Settings,
	Enabled,
	HalfRes,
	SpecularMult,
	SpecMaxSteps,
	SpecThickness,
	NormalBias,
	BRDFBias,
	OcclusionStrength,
	UseDynamicCubemapsAsFallback,
	SpecCubemapMult,
	HitDistA,
	HitDistB,
	HitDistC,
	HitDistD,
	EnableREBLUR,
	Reblur)

////////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceReflections::RestoreDefaultSettings()
{
	settings = {};
	recompileFlag = true;
}

void ScreenSpaceReflections::DrawSettings()
{
	static bool showAdvanced;

	if (!ShadersOK())
		ImGui::TextColored({ 1, 0, 0, 1 }, "Compute shaders failed to compile!");

	ImGui::SeparatorText("Toggles");

	ImGui::Checkbox("Show Advanced Options", &showAdvanced);

	if (ImGui::Checkbox("Enabled", &settings.Enabled))
		globals::deferred->ClearShaderCache();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Enable screen-space reflections. When disabled, the deferred composite falls back to cubemap-only reflections.");
	}

	{
		auto enabledGuard = Util::DisableGuard(!settings.Enabled);

		if (ImGui::Checkbox("Half Resolution (Checkerboard)", &settings.HalfRes)) {
			recompileFlag = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Trace half the columns in a checkerboard pattern. NRD reconstructs the missing pixels.");
		}
	}

	ImGui::SeparatorText("Visual");

	{
		auto visualGuard = Util::DisableGuard(!settings.Enabled);
		ImGui::SliderFloat("Specular Multiplier", &settings.SpecularMult, 0.0f, 5.0f, "%.2f");
		ImGui::Checkbox("Use Dynamic Cubemaps as Fallback", &settings.UseDynamicCubemapsAsFallback);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("When ray marching misses, fall back to dynamic cubemaps for reflections.");
		}
		{
			auto cubemapGuard = Util::DisableGuard(!settings.UseDynamicCubemapsAsFallback);
			ImGui::SliderFloat("Specular Cubemap Multiplier", &settings.SpecCubemapMult, 0.0f, 5.0f, "%.2f");
		}

		if (showAdvanced) {
			ImGui::Separator();
			ImGui::SliderInt("Max Steps", (int*)&settings.SpecMaxSteps, 1, 256);
			ImGui::SliderFloat("Thickness", &settings.SpecThickness, 0.0f, 50.0f, "%.2f");
			ImGui::SliderFloat("Normal Bias", &settings.NormalBias, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Push ray origin along surface normal to avoid self-intersection artifacts.");
			}
			ImGui::SliderFloat("BRDF Bias", &settings.BRDFBias, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Higher values reduce noise but make reflections more glossy.");
			}
			ImGui::SliderFloat("Occlusion Strength", &settings.OcclusionStrength, 0.0f, 1.0f, "%.2f");

			ImGui::SeparatorText("NRD Hit Distance Parameters");
			ImGui::SliderFloat("Hit Dist A", &settings.HitDistA, 1.0f, 1000.0f, "%.1f");
			ImGui::SliderFloat("Hit Dist B", &settings.HitDistB, 0.0f, 1.0f, "%.3f");
			ImGui::SliderFloat("Hit Dist C", &settings.HitDistC, 0.0f, 200.0f, "%.1f");
			ImGui::SliderFloat("Hit Dist D", &settings.HitDistD, -200.0f, 0.0f, "%.1f");
		}
	}

	ImGui::SeparatorText("REBLUR Denoiser");

	{
		auto denoiseGuard = Util::DisableGuard(!settings.Enabled);

		ImGui::Checkbox("Enable REBLUR", &settings.EnableREBLUR);

		if (settings.EnableREBLUR)
			globals::features::nrd.DrawReblurSettings(settings.Reblur, showAdvanced, "ssr_reblur");
	}

	ImGui::SeparatorText("Debug");

	if (ImGui::TreeNode("Buffer Viewer")) {
		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

		if (texHiZDepth)
			BUFFER_VIEWER_NODE(texHiZDepth, debugRescale)
		if (texNRDSpecInput)
			BUFFER_VIEWER_NODE(texNRDSpecInput, debugRescale)
		if (texNRDSpecOutput)
			BUFFER_VIEWER_NODE(texNRDSpecOutput, debugRescale)

		ImGui::TreePop();
	}
}

void ScreenSpaceReflections::LoadSettings(json& o_json)
{
	settings = o_json;
	recompileFlag = true;
}

void ScreenSpaceReflections::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ScreenSpaceReflections::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("ScreenSpaceReflections: creating buffers");
	{
		ssrCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSRCB>(), "SSR::CB");
	}

	logger::debug("ScreenSpaceReflections: creating textures");
	{
		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		D3D11_TEXTURE2D_DESC texDesc;
		mainTex.texture->GetDesc(&texDesc);

		uint32_t fullW = texDesc.Width;
		uint32_t fullH = texDesc.Height;

		// Hi-Z depth pyramid (R32F, raw NDC depth with min-Z mip chain)
		{
			uint minDim = std::min(fullW, fullH);
			numHiZMips = 1;
			while ((minDim >> numHiZMips) >= 16 && numHiZMips < kMaxHiZMips)
				++numHiZMips;

			D3D11_TEXTURE2D_DESC hizDesc{
				.Width = fullW,
				.Height = fullH,
				.MipLevels = numHiZMips,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R32_FLOAT,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			};
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
				.Format = hizDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = 0, .MipLevels = numHiZMips }
			};
			texHiZDepth = eastl::make_unique<Texture2D>(hizDesc, "SSR::HiZDepth");
			texHiZDepth->CreateSRV(srvDesc);

			for (uint i = 0; i < numHiZMips; i++) {
				D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {
					.Format = hizDesc.Format,
					.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MostDetailedMip = i, .MipLevels = 1 }
				};
				DX::ThrowIfFailed(device->CreateShaderResourceView(texHiZDepth->resource.get(), &mipSrvDesc, hiZDepthSRVs[i].put()));
				Util::SetResourceName(hiZDepthSRVs[i].get(), "SSR::HiZDepth SRV mip%u", i);

				D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
					.Format = hizDesc.Format,
					.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MipSlice = i }
				};
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texHiZDepth->resource.get(), &mipUavDesc, hiZDepthUAVs[i].put()));
				Util::SetResourceName(hiZDepthUAVs[i].get(), "SSR::HiZDepth UAV mip%u", i);
			}
		}

		// NRD input/output (RGBA16F, full-res)
		{
			D3D11_TEXTURE2D_DESC specDesc{
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
				.Format = specDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
			};
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
				.Format = specDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};

			texNRDSpecInput = eastl::make_unique<Texture2D>(specDesc, "SSR::NRDInput");
			texNRDSpecInput->CreateSRV(srvDesc);
			texNRDSpecInput->CreateUAV(uavDesc);

			texNRDSpecOutput = eastl::make_unique<Texture2D>(specDesc, "SSR::NRDOutput");
			texNRDSpecOutput->CreateSRV(srvDesc);
			texNRDSpecOutput->CreateUAV(uavDesc);

			nrdReblurSpecular.Shutdown();
			nrdReblurSpecular.Init(fullW, fullH, nrd::Denoiser::REBLUR_SPECULAR, 0);
		}
	}

	logger::debug("ScreenSpaceReflections: creating samplers");
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
		Util::SetResourceName(linearClampSampler.get(), "SSR::LinearClampSampler");

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
		Util::SetResourceName(pointClampSampler.get(), "SSR::PointClampSampler");
	}

	CompileComputeShaders();
}

void ScreenSpaceReflections::ClearShaderCache()
{
	prefilterHiZDepthCompute = nullptr;
	depthDownsampleCompute = nullptr;
	specularGICompute = nullptr;
	CompileComputeShaders();
}

void ScreenSpaceReflections::CompileComputeShaders()
{
	{
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceReflections") / "prefilterHiZDepth.cs.hlsl";
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0")))
			prefilterHiZDepthCompute.attach(rawPtr);
	}
	{
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceReflections") / "depthDownsample.cs.hlsl";
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0")))
			depthDownsampleCompute.attach(rawPtr);
	}
	{
		std::vector<std::pair<const char*, const char*>> defines;
		if (settings.HalfRes)
			defines.push_back({ "SSR_HALF", "" });
		if (globals::features::dynamicCubemaps.loaded)
			defines.push_back({ "DYNAMIC_CUBEMAPS", "" });
		if (globals::features::skylighting.loaded)
			defines.push_back({ "SKYLIGHTING", "" });
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceReflections") / "specularGI.cs.hlsl";
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), defines, "cs_5_0")))
			specularGICompute.attach(rawPtr);
	}

	recompileFlag = false;
}

bool ScreenSpaceReflections::ShadersOK()
{
	return prefilterHiZDepthCompute && depthDownsampleCompute && specularGICompute;
}

void ScreenSpaceReflections::UpdateSB()
{
	float2 res = { (float)texHiZDepth->desc.Width, (float)texHiZDepth->desc.Height };
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = { floor(dynres.x), floor(dynres.y) };

	SSRCB data;
	data.TexDim = res;
	data.RcpTexDim = float2(1.0f) / res;
	data.FrameDim = dynres;
	data.RcpFrameDim = float2(1.0f) / dynres;
	data.FrameIndex = globals::state->frameCount;

	data.SpecMaxSteps = settings.SpecMaxSteps;
	data.SpecMaxMips = std::min((uint)settings.SpecMaxSteps, numHiZMips);
	data.SpecThickness = settings.SpecThickness;
	data.NormalBias = settings.NormalBias;
	data.BRDFBias = settings.BRDFBias;
	data.OcclusionStrength = settings.OcclusionStrength;
	data.SpecUseDynamicCubemap = (settings.UseDynamicCubemapsAsFallback && globals::features::dynamicCubemaps.loaded) ? 1u : 0u;
	data.SpecCubemapMult = settings.SpecCubemapMult;
	data.HitDistA = settings.HitDistA;
	data.HitDistB = settings.HitDistB;
	data.HitDistC = settings.HitDistC;
	data.HitDistD = settings.HitDistD;

	ssrCB->Update(data);
}

void ScreenSpaceReflections::DrawSSR()
{
	if (!(settings.Enabled && ShadersOK() && texHiZDepth && texNRDSpecInput))
		return;

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "SSR");

	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("SSR");

	if (recompileFlag)
		ClearShaderCache();

	UpdateSB();

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;

	float2 size{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	size = Util::ConvertToDynamic(size);
	auto resolution = std::array{ (uint)size.x, (uint)size.y };
	float2 dynres = { floor(size.x), floor(size.y) };

	std::array<ID3D11SamplerState*, 2> samplers = { pointClampSampler.get(), linearClampSampler.get() };
	auto cb = ssrCB->CB();

	context->CSSetConstantBuffers(1, 1, &cb);
	auto* sharedDataBuf = globals::state->sharedDataCB->CB();
	context->CSSetConstantBuffers(5, 1, &sharedDataBuf);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	auto resetViews = [&]() {
		ID3D11ShaderResourceView* nullSRVs[8] = {};
		ID3D11UnorderedAccessView* nullUAVs[8] = {};
		context->CSSetShaderResources(0, 8, nullSRVs);
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	};

	// Hi-Z depth pyramid
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSR - Hi-Z Depth");
		if (globals::state->frameAnnotations)
			globals::state->BeginPerfEvent("SSR - Hi-Z Depth");

		globals::profiler->BeginPass("ScreenSpaceReflections::HiZDepth");

		auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

		// Copy NDC depth → Hi-Z mip 0
		{
			resetViews();
			auto srv = depth.depthSRV;
			context->CSSetShaderResources(0, 1, &srv);
			ID3D11UnorderedAccessView* uav = hiZDepthUAVs[0].get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(prefilterHiZDepthCompute.get(), nullptr, 0);
			context->Dispatch(((uint)dynres.x + 7) / 8, ((uint)dynres.y + 7) / 8, 1);
		}

		// Min-Z downsample mip chain
		for (uint i = 0; i < numHiZMips - 1; ++i) {
			uint outW = std::max(1u, (uint)size.x >> (i + 1));
			uint outH = std::max(1u, (uint)size.y >> (i + 1));

			resetViews();
			ID3D11ShaderResourceView* inSrv = hiZDepthSRVs[i].get();
			ID3D11UnorderedAccessView* outUav = hiZDepthUAVs[i + 1].get();
			context->CSSetShaderResources(0, 1, &inSrv);
			context->CSSetUnorderedAccessViews(0, 1, &outUav, nullptr);
			context->CSSetShader(depthDownsampleCompute.get(), nullptr, 0);
			context->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
		}

		resetViews();

		globals::profiler->EndPass();

		if (globals::state->frameAnnotations)
			globals::state->EndPerfEvent();
	}

	// Specular GI ray march
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSR - Specular GI");
		if (globals::state->frameAnnotations)
			globals::state->BeginPerfEvent("SSR - Specular GI");

		auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		auto normal = rts[NORMALROUGHNESS];
		auto main = rts[globals::deferred->forwardRenderTargets[0]];

		auto& dynamicCubemaps = globals::features::dynamicCubemaps;
		auto& skylighting = globals::features::skylighting;

		resetViews();
		std::array<ID3D11ShaderResourceView*, 7> specSRVs = { nullptr };
		specSRVs[0] = depth.depthSRV;
		specSRVs[1] = normal.SRV;
		specSRVs[2] = main.SRV;
		specSRVs[3] = texHiZDepth->srv.get();
		specSRVs[4] = dynamicCubemaps.loaded ? dynamicCubemaps.envTexture->srv.get() : nullptr;
		specSRVs[5] = dynamicCubemaps.loaded ? dynamicCubemaps.envReflectionsTexture->srv.get() : nullptr;
		specSRVs[6] = dynamicCubemaps.loaded && skylighting.loaded ? skylighting.texProbeArray->srv.get() : nullptr;

		ID3D11UnorderedAccessView* specUAV = texNRDSpecInput->uav.get();

		context->CSSetShaderResources(0, (uint)specSRVs.size(), specSRVs.data());
		context->CSSetUnorderedAccessViews(0, 1, &specUAV, nullptr);
		context->CSSetShader(specularGICompute.get(), nullptr, 0);

		uint specDispatchX = settings.HalfRes ? (resolution[0] + 1) / 2 : resolution[0];
		uint specDispatchY = resolution[1];
		globals::profiler->BeginPass("ScreenSpaceReflections::SpecularGI");
		context->Dispatch((specDispatchX + 7u) >> 3, (specDispatchY + 7u) >> 3, 1);
		globals::profiler->EndPass();

		resetViews();

		if (globals::state->frameAnnotations)
			globals::state->EndPerfEvent();
	}

	// REBLUR denoise (specular)
	auto& nrdSvc = globals::features::nrd;
	if (settings.EnableREBLUR && nrdReblurSpecular.IsValid() && nrdSvc.loaded && nrdSvc.AreGuidesReady()) {
		TracyD3D11Zone(globals::state->tracyCtx, "SSR - REBLUR Specular");
		if (globals::state->frameAnnotations)
			globals::state->BeginPerfEvent("SSR - REBLUR Specular");

		nrdReblurSpecular.SetCommonSettings(nrdSvc.GetCommonSettings());

		nrdSvc.ApplyReblurSettings(reblurSpecularSettings, settings.Reblur,
			settings.HalfRes ? nrd::CheckerboardMode::BLACK : nrd::CheckerboardMode::OFF);
		reblurSpecularSettings.hitDistanceParameters.A = settings.HitDistA;
		reblurSpecularSettings.hitDistanceParameters.B = settings.HitDistB;
		reblurSpecularSettings.hitDistanceParameters.C = settings.HitDistC;
		nrdReblurSpecular.SetDenoiserSettings(&reblurSpecularSettings);

		nrdReblurSpecular.SetNamedSRV(nrd::ResourceType::IN_MV, nrdSvc.GetMotionVectorSRV());
		nrdReblurSpecular.SetNamedUAV(nrd::ResourceType::IN_MV, nrdSvc.GetMotionVectorUAV());
		nrdReblurSpecular.SetNamedSRV(nrd::ResourceType::IN_NORMAL_ROUGHNESS, nrdSvc.GetNormalRoughnessSRV());
		nrdReblurSpecular.SetNamedSRV(nrd::ResourceType::IN_VIEWZ, nrdSvc.GetViewZSRV());
		nrdReblurSpecular.SetNamedSRV(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, texNRDSpecInput->srv.get());
		nrdReblurSpecular.SetNamedSRV(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, texNRDSpecOutput->srv.get());
		nrdReblurSpecular.SetNamedUAV(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, texNRDSpecOutput->uav.get());

		globals::profiler->BeginPass("ScreenSpaceReflections::ReblurSpecular");
		nrdReblurSpecular.Dispatch();
		globals::profiler->EndPass();

		if (globals::state->frameAnnotations)
			globals::state->EndPerfEvent();
	}

	// cleanup
	resetViews();
	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

ID3D11ShaderResourceView* ScreenSpaceReflections::GetOutputTexture()
{
	if (!loaded || !settings.Enabled)
		return nullptr;
	if (settings.EnableREBLUR && nrdReblurSpecular.IsValid() && globals::features::nrd.loaded && globals::features::nrd.AreGuidesReady())
		return texNRDSpecOutput->srv.get();
	return texNRDSpecInput->srv.get();
}

ScreenSpaceReflections::SharedData ScreenSpaceReflections::GetCommonBufferData()
{
	SharedData data;
	data.Enabled = (loaded && settings.Enabled) ? 1u : 0u;
	data.SpecularMult = settings.SpecularMult;
	data.pad0 = 0;
	data.pad1 = 0;
	return data;
}
