#include "ScreenSpaceReflections.h"

#include "Deferred.h"
#include "DynamicCubemaps.h"
#include "I18n/I18n.h"
#include "IBL.h"
#include "NRD.h"
#include "ShaderCache.h"
#include "Skylighting.h"
#include "State.h"
#include "Upscaling.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.screen_space_reflections."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceReflections::Settings,
	Enabled,
	ReplaceWaterSSR,
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
	EnableREBLUR,
	Reblur,
	SpecularPrepassBlurRadius,
	UsePrepassOnlyForSpecularMotionEstimation)

////////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceReflections::RestoreDefaultSettings()
{
	settings = {};
	recompileFlag = true;
	resetReblurHistory = true;
	outputReady = false;
}

void ScreenSpaceReflections::DrawSettings()
{
	static bool showAdvanced;

	if (!ShadersOK())
		ImGui::TextColored({ 1, 0, 0, 1 }, "%s", T(TKEY("compute_shaders_failed_to_compile"), "Compute shaders failed to compile!"));

	ImGui::SeparatorText(T(TKEY("toggles"), "Toggles"));

	ImGui::Checkbox(T(TKEY("show_advanced_options"), "Show Advanced Options"), &showAdvanced);

	if (ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled))
		globals::deferred->ClearShaderCache();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enabled_tooltip"), "Enable screen-space reflections. When disabled, the deferred composite falls back to cubemap-only reflections."));
	}
	{
		auto waterSSRGuard = Util::DisableGuard(!settings.Enabled);
		ImGui::Checkbox(T(TKEY("replace_water_ssr"), "Use Hi-Z SSR for Water"), &settings.ReplaceWaterSSR);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("replace_water_ssr_tooltip"), "Write the final water surface normal to a dedicated target, trace a sharp Hi-Z reflection in compute, then composite it after water refraction with Fresnel. The original water SSR, blur, and WaterBlend chain is skipped."));
		}
	}

	{
		auto enabledGuard = Util::DisableGuard(!settings.Enabled);

		{
			auto halfResGuard = Util::DisableGuard(!settings.EnableREBLUR);
			if (ImGui::Checkbox(T(TKEY("half_resolution_checkerboard"), "Half Resolution (Checkerboard)"), &settings.HalfRes)) {
				recompileFlag = true;
				resetReblurHistory = true;
			}
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("half_resolution_checkerboard_tooltip"), "Trace half the columns in a checkerboard pattern. NRD reconstructs the missing pixels."));
		}
	}

	ImGui::SeparatorText(T(TKEY("visual"), "Visual"));

	{
		auto visualGuard = Util::DisableGuard(!settings.Enabled);
		ImGui::SliderFloat(T(TKEY("specular_multiplier"), "Specular Multiplier"), &settings.SpecularMult, 0.0f, 5.0f, "%.2f");
		ImGui::Checkbox(T(TKEY("use_dynamic_cubemaps_as_fallback"), "Use Dynamic Cubemaps as Fallback"), &settings.UseDynamicCubemapsAsFallback);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("use_dynamic_cubemaps_as_fallback_tooltip"), "When ray marching misses, fall back to dynamic cubemaps for reflections."));
		}
		{
			auto cubemapGuard = Util::DisableGuard(!settings.UseDynamicCubemapsAsFallback);
			ImGui::SliderFloat(T(TKEY("specular_cubemap_multiplier"), "Specular Cubemap Multiplier"), &settings.SpecCubemapMult, 0.0f, 5.0f, "%.2f");
		}

		if (showAdvanced) {
			ImGui::Separator();
			ImGui::SliderInt(T(TKEY("max_steps"), "Max Steps"), (int*)&settings.SpecMaxSteps, 1, 256);
			ImGui::SliderFloat(T(TKEY("thickness"), "Thickness"), &settings.SpecThickness, 0.0f, 50.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("normal_bias"), "Normal Bias"), &settings.NormalBias, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("normal_bias_tooltip"), "Push ray origin along surface normal to avoid self-intersection artifacts."));
			}
			ImGui::SliderFloat(T(TKEY("brdf_bias"), "BRDF Bias"), &settings.BRDFBias, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("brdf_bias_tooltip"), "Higher values reduce noise but make reflections more glossy."));
			}
			ImGui::SliderFloat(T(TKEY("occlusion_strength"), "Occlusion Strength"), &settings.OcclusionStrength, 0.0f, 1.0f, "%.2f");

			ImGui::SeparatorText(T(TKEY("nrd_hit_distance_parameters"), "NRD Hit Distance Parameters"));
			bool hitDistanceChanged = false;
			hitDistanceChanged |= ImGui::SliderFloat(T(TKEY("hit_dist_a"), "Hit Dist A"), &settings.HitDistA, 1.0f, 1000.0f, "%.1f");
			hitDistanceChanged |= ImGui::SliderFloat(T(TKEY("hit_dist_b"), "Hit Dist B"), &settings.HitDistB, 0.0f, 1.0f, "%.3f");
			hitDistanceChanged |= ImGui::SliderFloat(T(TKEY("hit_dist_c"), "Hit Dist C"), &settings.HitDistC, 1.0f, 200.0f, "%.1f");
			resetReblurHistory |= hitDistanceChanged;
		}
	}

	ImGui::SeparatorText(T(TKEY("reblur_denoiser"), "REBLUR Denoiser"));

	{
		auto denoiseGuard = Util::DisableGuard(!settings.Enabled);

		if (ImGui::Checkbox(T(TKEY("enable_reblur"), "Enable REBLUR"), &settings.EnableREBLUR)) {
			if (!settings.EnableREBLUR && settings.HalfRes) {
				settings.HalfRes = false;
				recompileFlag = true;
			}
			resetReblurHistory = true;
		}

		if (settings.EnableREBLUR) {
			resetReblurHistory |= globals::features::nrd.DrawReblurSettings(settings.Reblur, showAdvanced, "ssr_reblur");
			if (showAdvanced) {
				resetReblurHistory |= ImGui::SliderFloat(T(TKEY("specular_prepass_blur_radius"), "Specular Pre-pass Radius"), &settings.SpecularPrepassBlurRadius, 0.0f, 75.0f, "%.1f px");
				resetReblurHistory |= ImGui::Checkbox(T(TKEY("specular_prepass_motion_only"), "Use Pre-pass Only for Motion Estimation"), &settings.UsePrepassOnlyForSpecularMotionEstimation);
			}
		}
	}

	ImGui::SeparatorText(T(TKEY("debug"), "Debug"));

	if (ImGui::TreeNode(T(TKEY("buffer_viewer"), "Buffer Viewer"))) {
		static float debugRescale = .3f;
		ImGui::SliderFloat(T(TKEY("view_resize"), "View Resize"), &debugRescale, 0.f, 1.f);

		if (texHiZDepth)
			BUFFER_VIEWER_NODE(texHiZDepth, debugRescale)
		if (texNRDSpecInput)
			BUFFER_VIEWER_NODE(texNRDSpecInput, debugRescale)
		if (texNRDSpecOutput)
			BUFFER_VIEWER_NODE(texNRDSpecOutput, debugRescale)
		if (texWaterNormal)
			BUFFER_VIEWER_NODE(texWaterNormal, debugRescale)
		if (auto validation = settings.Reblur.EnableValidation ? nrdReblurSpecular.GetValidationSRV() : nullptr) {
			if (ImGui::TreeNode("NRD Validation")) {
				ImGui::Image(validation, { nrdReblurSpecular.GetWidth() * debugRescale, nrdReblurSpecular.GetHeight() * debugRescale });
				ImGui::TreePop();
			}
		}

		ImGui::TreePop();
	}
}

#undef I18N_KEY_PREFIX

void ScreenSpaceReflections::LoadSettings(json& o_json)
{
	settings = o_json;
	if (!settings.EnableREBLUR)
		settings.HalfRes = false;
	recompileFlag = true;
	resetReblurHistory = true;
	outputReady = false;
}

void ScreenSpaceReflections::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ScreenSpaceReflections::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;
	resetReblurHistory = true;
	outputReady = false;

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

			numHiZMips = std::max(numHiZMips, 7u);

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

		// Full-resolution water normal + coverage. Water.hlsl writes the final
		// perturbed world-space normal to MRT2 only for non-additive surface passes.
		{
			D3D11_TEXTURE2D_DESC normalDesc{
				.Width = fullW,
				.Height = fullH,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
			};
			D3D11_SHADER_RESOURCE_VIEW_DESC normalSrvDesc{
				.Format = normalDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 },
			};
			D3D11_RENDER_TARGET_VIEW_DESC normalRtvDesc{
				.Format = normalDesc.Format,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 },
			};

			texWaterNormal = eastl::make_unique<Texture2D>(normalDesc, "SSR::WaterNormal");
			texWaterNormal->CreateSRV(normalSrvDesc);
			texWaterNormal->CreateRTV(normalRtvDesc);
			waterNormalClearFrame = static_cast<std::uint32_t>(-1);
		}

		// Snapshot of kMAIN after water has sampled refraction and finished shading.
		// The compute pass reads this SRV while writing kMAIN through its UAV.
		{
			D3D11_TEXTURE2D_DESC copyDesc{};
			mainTex.texture->GetDesc(&copyDesc);
			copyDesc.Usage = D3D11_USAGE_DEFAULT;
			copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			copyDesc.CPUAccessFlags = 0;

			D3D11_SHADER_RESOURCE_VIEW_DESC copySrvDesc{};
			mainTex.SRV->GetDesc(&copySrvDesc);

			texWaterColorCopy = eastl::make_unique<Texture2D>(copyDesc, "SSR::WaterColorCopy");
			texWaterColorCopy->CreateSRV(copySrvDesc);
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
	outputReady = false;
	prefilterHiZDepthCompute = nullptr;
	prefilterHiZMipsCompute = nullptr;
	depthDownsampleCompute = nullptr;
	specularGICompute = nullptr;
	waterSSRCompute = nullptr;
	CompileComputeShaders();
}

void ScreenSpaceReflections::CompileComputeShaders()
{
	{
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceReflections") / "prefilterHiZDepth.cs.hlsl";
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0")))
			prefilterHiZDepthCompute.attach(rawPtr);
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), { { "FUSED_HIZ_MIPS", "" } }, "cs_5_0")))
			prefilterHiZMipsCompute.attach(rawPtr);
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
		if (globals::features::ibl.loaded)
			defines.push_back({ "IBL", "" });
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceReflections") / "specularGI.cs.hlsl";
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), defines, "cs_5_0")))
			specularGICompute.attach(rawPtr);
	}
	{
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceReflections") / "waterSSR.cs.hlsl";
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0")))
			waterSSRCompute.attach(rawPtr);
	}

	recompileFlag = false;
}

bool ScreenSpaceReflections::ShadersOK()
{
	return prefilterHiZDepthCompute && prefilterHiZMipsCompute && depthDownsampleCompute && specularGICompute;
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

	ssrCB->Update(data);
}

void ScreenSpaceReflections::DrawSSR()
{
	outputReady = false;

	if (recompileFlag)
		ClearShaderCache();

	UpdateWaterBlendOverride();

	if (!(settings.Enabled && ShadersOK() && texHiZDepth && texNRDSpecInput))
		return;

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "SSR");

	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("SSR");

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

		uint firstDownsampleMip = 0;
		const bool canFuseFirstFiveMips = numHiZMips >= 5 &&
		                                  (texHiZDepth->desc.Width & 15u) == 0 &&
		                                  (texHiZDepth->desc.Height & 15u) == 0;
		if (canFuseFirstFiveMips) {
			// Build mip 0-4 in one pass. Each group covers a 16x16 source tile,
			// then performs the same min-Z reduction in group-shared memory.
			resetViews();
			auto srv = depth.depthSRV;
			context->CSSetShaderResources(0, 1, &srv);
			std::array<ID3D11UnorderedAccessView*, 5> uavs;
			for (uint i = 0; i < (uint)uavs.size(); ++i)
				uavs[i] = hiZDepthUAVs[i].get();
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(prefilterHiZMipsCompute.get(), nullptr, 0);
			context->Dispatch(((uint)dynres.x + 15) / 16, ((uint)dynres.y + 15) / 16, 1);
			firstDownsampleMip = 4;
		} else {
			// Small or non-16-aligned render targets keep the generic path so its
			// NPOT edge-reduction behavior remains unchanged.
			resetViews();
			auto srv = depth.depthSRV;
			context->CSSetShaderResources(0, 1, &srv);
			ID3D11UnorderedAccessView* uav = hiZDepthUAVs[0].get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(prefilterHiZDepthCompute.get(), nullptr, 0);
			context->Dispatch(((uint)dynres.x + 7) / 8, ((uint)dynres.y + 7) / 8, 1);
		}

		// Min-Z downsample mip chain
		for (uint i = firstDownsampleMip; i < numHiZMips - 1; ++i) {
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
		auto& ibl = globals::features::ibl;
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
		ID3D11ShaderResourceView* envIBLSrv = ibl.loaded && ibl.envIBLTexture ? ibl.envIBLTexture->srv.get() : nullptr;

		ID3D11UnorderedAccessView* specUAV = texNRDSpecInput->uav.get();

		context->CSSetShaderResources(0, (uint)specSRVs.size(), specSRVs.data());
		context->CSSetShaderResources(76, 1, &envIBLSrv);
		context->CSSetUnorderedAccessViews(0, 1, &specUAV, nullptr);
		context->CSSetShader(specularGICompute.get(), nullptr, 0);

		uint specDispatchX = settings.HalfRes ? (resolution[0] + 1) / 2 : resolution[0];
		uint specDispatchY = resolution[1];
		globals::profiler->BeginPass("ScreenSpaceReflections::SpecularGI");
		context->Dispatch((specDispatchX + 7u) >> 3, (specDispatchY + 7u) >> 3, 1);
		globals::profiler->EndPass();

		envIBLSrv = nullptr;
		context->CSSetShaderResources(76, 1, &envIBLSrv);
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

		auto commonSettings = nrdSvc.GetCommonSettings();
		commonSettings.splitScreen = settings.Reblur.SplitScreen;
		commonSettings.enableValidation = settings.Reblur.EnableValidation;
		if (resetReblurHistory)
			commonSettings.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
		nrdReblurSpecular.SetCommonSettings(commonSettings);

		nrdSvc.ApplyReblurSettings(reblurSpecularSettings, settings.Reblur,
			settings.HalfRes ? nrd::CheckerboardMode::BLACK : nrd::CheckerboardMode::OFF);
		reblurSpecularSettings.hitDistanceParameters.A = settings.HitDistA;
		reblurSpecularSettings.hitDistanceParameters.B = settings.HitDistB;
		reblurSpecularSettings.hitDistanceParameters.C = settings.HitDistC;
		reblurSpecularSettings.specularPrepassBlurRadius = std::max(settings.SpecularPrepassBlurRadius, 0.0f);
		reblurSpecularSettings.usePrepassOnlyForSpecularMotionEstimation = settings.UsePrepassOnlyForSpecularMotionEstimation;
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
		resetReblurHistory = false;
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
	outputReady = true;

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void ScreenSpaceReflections::BindWaterNormalTarget(bool a_isCompute)
{
	if (a_isCompute || !IsWaterSSRReplacementActive() || !texWaterNormal || !texWaterNormal->rtv)
		return;

	auto* state = globals::state;
	if (!state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Water || state->IsRenderingReflections())
		return;

	// Water techniques 0..7 are the base pass followed by additive specular-light
	// permutations. Only technique 0, LOD (9), and Simple (11) own a complete
	// surface normal and therefore declare SV_Target2 in Water.hlsl.
	const std::uint32_t technique = (state->currentPixelDescriptor >> 11) & 0xFu;
	if (technique != 0 &&
		technique != static_cast<std::uint32_t>(SIE::ShaderCache::WaterShaderTechniques::Lod) &&
		technique != static_cast<std::uint32_t>(SIE::ShaderCache::WaterShaderTechniques::Simple))
		return;

	auto* context = globals::d3d::context;
	if (!context)
		return;

	std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> currentRTVs{};
	ID3D11DepthStencilView* currentDSV = nullptr;
	context->OMGetRenderTargets(static_cast<UINT>(currentRTVs.size()), currentRTVs.data(), &currentDSV);

	bool canAppend = currentRTVs[0] != nullptr && currentRTVs[2] == nullptr;
	for (std::size_t i = 3; i < currentRTVs.size(); ++i)
		canAppend &= currentRTVs[i] == nullptr;

	if (canAppend) {
		if (waterNormalClearFrame != state->frameCount) {
			constexpr float clear[4] = { 0.f, 0.f, 0.f, 0.f };
			context->ClearRenderTargetView(texWaterNormal->rtv.get(), clear);
			waterNormalClearFrame = state->frameCount;
		}

		ID3D11RenderTargetView* waterRTVs[3] = { currentRTVs[0], currentRTVs[1], texWaterNormal->rtv.get() };
		context->OMSetRenderTargets(3, waterRTVs, currentDSV);
	}

	for (auto* rtv : currentRTVs) {
		if (rtv)
			rtv->Release();
	}
	if (currentDSV)
		currentDSV->Release();
}

void ScreenSpaceReflections::DrawWaterSSR()
{
	if (!IsWaterSSRReplacementActive() || !outputReady ||
		!texWaterNormal || !texWaterColorCopy || !waterSSRCompute || globals::state->IsRenderingReflections())
		return;

	const auto frameCount = globals::state->frameCount;
	if (waterNormalClearFrame != frameCount || waterSSRDrawFrame == frameCount)
		return;

	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	if (!context || !renderer)
		return;

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& waterDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	if (!main.texture || !main.UAV || !waterDepth.depthSRV)
		return;

	std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> previousRTVs{};
	ID3D11DepthStencilView* previousDSV = nullptr;
	context->OMGetRenderTargets(static_cast<UINT>(previousRTVs.size()), previousRTVs.data(), &previousDSV);
	if (previousRTVs[2] == texWaterNormal->rtv.get()) {
		previousRTVs[2]->Release();
		previousRTVs[2] = nullptr;
	}
	UINT previousRTVCount = 0;
	for (UINT i = 0; i < static_cast<UINT>(previousRTVs.size()); ++i) {
		if (previousRTVs[i])
			previousRTVCount = i + 1;
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "SSR - Water");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("SSR - Water");

	// Snapshot kMAIN without an output binding, then composite directly back into
	// its UAV. Restore the game-owned output-merger state before returning.
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->CopyResource(texWaterColorCopy->resource.get(), main.texture);

	std::array<ID3D11ShaderResourceView*, 4> srvs{
		waterDepth.depthSRV,
		texWaterNormal->srv.get(),
		texWaterColorCopy->srv.get(),
		texHiZDepth->srv.get(),
	};
	ID3D11UnorderedAccessView* uav = main.UAV;
	std::array<ID3D11SamplerState*, 2> samplers{ pointClampSampler.get(), linearClampSampler.get() };
	ID3D11Buffer* cb = ssrCB->CB();
	ID3D11Buffer* sharedData = globals::state->sharedDataCB->CB();
	ID3D11Buffer* frameBuffer = *globals::game::perFrame.get();

	ID3D11ComputeShader* previousShader = nullptr;
	std::array<ID3D11ShaderResourceView*, 4> previousSRVs{};
	ID3D11UnorderedAccessView* previousUAV = nullptr;
	std::array<ID3D11SamplerState*, 2> previousSamplers{};
	ID3D11Buffer* previousCB1 = nullptr;
	ID3D11Buffer* previousCB5 = nullptr;
	ID3D11Buffer* previousCB12 = nullptr;

	context->CSGetShader(&previousShader, nullptr, nullptr);
	context->CSGetShaderResources(0, static_cast<UINT>(previousSRVs.size()), previousSRVs.data());
	context->CSGetUnorderedAccessViews(0, 1, &previousUAV);
	context->CSGetSamplers(0, static_cast<UINT>(previousSamplers.size()), previousSamplers.data());
	context->CSGetConstantBuffers(1, 1, &previousCB1);
	context->CSGetConstantBuffers(5, 1, &previousCB5);
	context->CSGetConstantBuffers(12, 1, &previousCB12);

	context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetConstantBuffers(5, 1, &sharedData);
	context->CSSetConstantBuffers(12, 1, &frameBuffer);
	context->CSSetShader(waterSSRCompute.get(), nullptr, 0);

	float2 size{ static_cast<float>(globals::game::graphicsState->screenWidth), static_cast<float>(globals::game::graphicsState->screenHeight) };
	size = Util::ConvertToDynamic(size);
	globals::profiler->BeginPass("ScreenSpaceReflections::WaterSSR");
	context->Dispatch((static_cast<std::uint32_t>(size.x) + 7u) >> 3, (static_cast<std::uint32_t>(size.y) + 7u) >> 3, 1);
	waterSSRDrawFrame = frameCount;
	globals::profiler->EndPass();

	std::array<ID3D11ShaderResourceView*, 4> nullSRVs{};
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetShaderResources(0, static_cast<UINT>(nullSRVs.size()), nullSRVs.data());
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

	context->CSSetShaderResources(0, static_cast<UINT>(previousSRVs.size()), previousSRVs.data());
	context->CSSetUnorderedAccessViews(0, 1, &previousUAV, nullptr);
	context->CSSetSamplers(0, static_cast<UINT>(previousSamplers.size()), previousSamplers.data());
	context->CSSetConstantBuffers(1, 1, &previousCB1);
	context->CSSetConstantBuffers(5, 1, &previousCB5);
	context->CSSetConstantBuffers(12, 1, &previousCB12);
	context->CSSetShader(previousShader, nullptr, 0);
	context->OMSetRenderTargets(previousRTVCount, previousRTVs.data(), previousDSV);

	for (auto* srv : previousSRVs) {
		if (srv)
			srv->Release();
	}
	if (previousUAV)
		previousUAV->Release();
	for (auto* sampler : previousSamplers) {
		if (sampler)
			sampler->Release();
	}
	if (previousCB1)
		previousCB1->Release();
	if (previousCB5)
		previousCB5->Release();
	if (previousCB12)
		previousCB12->Release();
	if (previousShader)
		previousShader->Release();
	for (auto* rtv : previousRTVs) {
		if (rtv)
			rtv->Release();
	}
	if (previousDSV)
		previousDSV->Release();

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

ID3D11ShaderResourceView* ScreenSpaceReflections::GetOutputTexture()
{
	if (!outputReady || !loaded || !settings.Enabled)
		return nullptr;
	if (settings.EnableREBLUR && nrdReblurSpecular.IsValid() && globals::features::nrd.loaded && globals::features::nrd.AreGuidesReady())
		return texNRDSpecOutput->srv.get();
	return texNRDSpecInput->srv.get();
}

ID3D11ShaderResourceView* ScreenSpaceReflections::GetHiZDepthTexture()
{
	if (!loaded || !settings.Enabled || recompileFlag || !ShadersOK() || !texHiZDepth)
		return nullptr;
	return texHiZDepth->srv.get();
}

bool ScreenSpaceReflections::IsWaterSSRReplacementActive()
{
	return settings.ReplaceWaterSSR && GetHiZDepthTexture() != nullptr &&
	       texWaterNormal && texWaterNormal->rtv && texWaterNormal->srv &&
	       texWaterColorCopy && texWaterColorCopy->srv && waterSSRCompute;
}

void ScreenSpaceReflections::UpdateWaterBlendOverride()
{
	// Skyrim AE skips the explicit ISWaterBlend pass while this flag is set.
	// Skyrim SE 1.5.97 does not read the field, so its pass is skipped by the
	// BSImagespaceShaderISWaterBlend hook instead.
	if (!REL::Module::IsAE())
		return;

	auto* imageSpaceManager = globals::game::imageSpaceManager;
	auto* taa = imageSpaceManager ? imageSpaceManager->GetRuntimeData().BSImagespaceShaderISTemporalAA : nullptr;
	auto* taaWaterBlendingEnabled = taa ? reinterpret_cast<bool*>(reinterpret_cast<std::uintptr_t>(taa) + 0x38) : nullptr;

	if (!IsWaterSSRReplacementActive()) {
		if (!waterBlendOverrideApplied || !taaWaterBlendingEnabled)
			return;

		if (taa == waterBlendOverrideTarget)
			*taaWaterBlendingEnabled = originalTAAWaterBlendingState;

		waterBlendOverrideTarget = nullptr;
		waterBlendOverrideApplied = false;
		return;
	}

	if (!taa)
		return;

	if (!waterBlendOverrideApplied || taa != waterBlendOverrideTarget) {
		waterBlendOverrideTarget = taa;
		originalTAAWaterBlendingState = *taaWaterBlendingEnabled;
		waterBlendOverrideApplied = true;
	}

	*taaWaterBlendingEnabled = true;
}

ScreenSpaceReflections::SharedData ScreenSpaceReflections::GetCommonBufferData()
{
	SharedData data;
	data.Enabled = (loaded && settings.Enabled && !recompileFlag && ShadersOK() && texHiZDepth && texNRDSpecInput) ? 1u : 0u;
	data.SpecularMult = settings.SpecularMult;
	data.SpecCubemapMult = settings.UseDynamicCubemapsAsFallback && globals::features::dynamicCubemaps.loaded ? settings.SpecCubemapMult : 0.0f;
	data.UseHiZForWater = IsWaterSSRReplacementActive() ? 1u : 0u;
	data.MaxSteps = settings.SpecMaxSteps;
	data.MaxMips = std::min(settings.SpecMaxSteps, numHiZMips);
	data.Thickness = settings.SpecThickness;
	data.NormalBias = settings.NormalBias;
	return data;
}
