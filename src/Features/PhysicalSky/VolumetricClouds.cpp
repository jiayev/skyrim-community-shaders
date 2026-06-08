#include "Features/PhysicalSky.h"

#include "Deferred.h"
#include "Features/TerrainShadows.h"
#include "Features/VolumetricShadows.h"
#include "State.h"
#include "Util.h"

#include <DDSTextureLoader.h>
#include <DirectXTex.h>

void PhysicalSky::LoadCloudTextures()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	auto loadDDS = [&](const wchar_t* path, winrt::com_ptr<ID3D11ShaderResourceView>& srv) {
		srv = nullptr;
		HRESULT hr = DirectX::CreateDDSTextureFromFile(device, context, path, nullptr, srv.put());
		if (FAILED(hr))
			logger::warn("Failed to load DDS texture: {}", std::filesystem::path(path).string());
	};

	loadDDS(L"Data\\Textures\\PhysicalSky\\top_lut.dds", cloudTopLutSrv);
	loadDDS(L"Data\\Textures\\PhysicalSky\\bottom_lut.dds", cloudBottomLutSrv);
	loadDDS(L"Data\\Textures\\PhysicalSky\\nubis.dds", nubisNoiseSrv);
}

void PhysicalSky::SetupVolumetricResources()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	logger::debug("Setting up volumetric cloud resources...");

	// Tileable sampler (wrap all axes)
	{
		D3D11_SAMPLER_DESC desc = {};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		desc.MaxAnisotropy = 1;
		desc.MinLOD = 0;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&desc, sampTileable.put()));
	}

	// Volumetric cloud StructuredBuffer
	{
		volCloudSb = eastl::make_unique<StructuredBuffer>(StructuredBufferDesc<VolumetricCloudSB>(), 1);
		volCloudSb->CreateSRV();
		volCubeHistoryCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VolumetricCloudCubeHistoryCB>(), "PhysicalSky::VolumetricCubeHistoryCB");
	}

	// Get main render target dimensions for TR/Lum textures
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC mainDesc;
	mainTex.texture->GetDesc(&mainDesc);

	auto createCloudTexture = [](uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format, const char* a_name) {
		D3D11_TEXTURE2D_DESC tex_desc = {
			.Width = a_width,
			.Height = a_height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = a_format,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		auto texture = eastl::make_unique<Texture2D>(tex_desc, a_name);
		texture->CreateSRV(srv_desc);
		texture->CreateUAV(uav_desc);
		return texture;
	};

	// Nubis main-view cloud textures: 1/4-resolution raymarch, full-resolution resample, then full-resolution blur.
	{
		const uint32_t lowW = std::max(1u, (mainDesc.Width + kVolCloudDownsample - 1u) / kVolCloudDownsample);
		const uint32_t lowH = std::max(1u, (mainDesc.Height + kVolCloudDownsample - 1u) / kVolCloudDownsample);

		texVolLowTr = createCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricLowTr");
		texVolLowLum = createCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricLowLum");
		texVolLowAux = createCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricLowAux");
		texVolUpscaleTr = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricUpscaleTr");
		texVolUpscaleLum = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricUpscaleLum");
		texVolUpscaleAux = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricUpscaleAux");
		texVolTr = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricTr");
		texVolLum = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricLum");
		texVolAux = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricAux");
		texVolHistoryTr = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricHistoryTr");
		texVolHistoryLum = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricHistoryLum");
		texVolHistoryAux = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricHistoryAux");
	}

	// Low-resolution cubemap volumetric cloud textures for Dynamic Cubemaps inference.
	{
		D3D11_TEXTURE2D_DESC tex_desc = {
			.Width = kVolCubeSize,
			.Height = kVolCubeSize,
			.MipLevels = 1,
			.ArraySize = 6,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray = { .MipSlice = 0, .FirstArraySlice = 0, .ArraySize = tex_desc.ArraySize }
		};

		texVolCubeTr = eastl::make_unique<Texture2D>(tex_desc, "PhysicalSky::VolumetricCubeTr");
		texVolCubeTr->CreateSRV(srv_desc);
		texVolCubeTr->CreateUAV(uav_desc);

		texVolCubeLum = eastl::make_unique<Texture2D>(tex_desc, "PhysicalSky::VolumetricCubeLum");
		texVolCubeLum->CreateSRV(srv_desc);
		texVolCubeLum->CreateUAV(uav_desc);

		texVolCubeTrHistory = eastl::make_unique<Texture2D>(tex_desc, "PhysicalSky::VolumetricCubeTrHistory");
		texVolCubeTrHistory->CreateSRV(srv_desc);
		texVolCubeTrHistory->CreateUAV(uav_desc);

		texVolCubeLumHistory = eastl::make_unique<Texture2D>(tex_desc, "PhysicalSky::VolumetricCubeLumHistory");
		texVolCubeLumHistory->CreateSRV(srv_desc);
		texVolCubeLumHistory->CreateUAV(uav_desc);

		FLOAT trClr[4] = { 1.f, 1.f, 1.f, 1.f };
		FLOAT lumClr[4] = { 0.f, 0.f, 0.f, 0.f };
		context->ClearUnorderedAccessViewFloat(texVolCubeTr->uav.get(), trClr);
		context->ClearUnorderedAccessViewFloat(texVolCubeLum->uav.get(), lumClr);
		context->ClearUnorderedAccessViewFloat(texVolCubeTrHistory->uav.get(), trClr);
		context->ClearUnorderedAccessViewFloat(texVolCubeLumHistory->uav.get(), lumClr);
		volCubeHistoryValid = false;
	}

	// Shadow volume 3D texture
	{
		D3D11_TEXTURE3D_DESC tex3d_desc = {
			.Width = kShadowVolW,
			.Height = kShadowVolH,
			.Depth = kShadowVolD,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
			.Format = tex3d_desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
			.Format = tex3d_desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = kShadowVolD }
		};

		texShadowVolume = eastl::make_unique<Texture3D>(tex3d_desc);
		texShadowVolume->CreateSRV(srv_desc);
		texShadowVolume->CreateUAV(uav_desc);
	}

	// Load textures and NDF
	LoadCloudTextures();
	ndfManager.SetupResources();

	CompileVolumetricShaders();
}

void PhysicalSky::CompileVolumetricShaders()
{
	logger::debug("Compiling volumetric cloud shaders...");

	struct ShaderInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* csPtr;
		const char* filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry = "main";
	};

	std::array shaderInfos = {
		ShaderInfo{ &csVolMainView, "Volumetrics.cs.hlsl" },
		ShaderInfo{ &csVolResample, "Volumetrics.cs.hlsl", {}, "resample" },
		ShaderInfo{ &csVolBlur, "Volumetrics.cs.hlsl", {}, "blur" },
		ShaderInfo{ &csVolShadowVolume, "Volumetrics.cs.hlsl", {}, "renderShadowVolume" },
		ShaderInfo{ &csVolCubemap, "Volumetrics.cs.hlsl", {}, "renderCubemap" },
		ShaderInfo{ &csVolCubemapHistory, "Volumetrics.cs.hlsl", {}, "accumulateCubemap" }
	};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.data())))
			info.csPtr->attach(rawPtr);
	}

	ndfManager.CompileShaders();
}

void PhysicalSky::RenderVolumetricClouds(VolumetricCloudPass a_pass)
{
	if (!csVolMainView || !csVolResample || !csVolBlur || !csVolShadowVolume || !csVolCubemap)
		return;
	if (!nubisNoiseSrv || !cloudTopLutSrv || !cloudBottomLutSrv)
		return;

	auto state = globals::state;
	auto context = globals::d3d::context;
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	const uint32_t resW = (uint32_t)cbData.frameDim.x;
	const uint32_t resH = (uint32_t)cbData.frameDim.y;
	if (volHistoryWidth != resW || volHistoryHeight != resH) {
		volMainHistoryValid = false;
		volHistoryWidth = resW;
		volHistoryHeight = resH;
	}

	// Convert cloud layer settings to noise offset (speed * time)
	float3 noiseOffset = settings.cloudLayer.noiseSpeed * (state->timer * 1e-3f);

	// Aerial perspective max distance (same as LUT generation)
	float apMaxDist = 40.f / 1.428e-5f;  // 40km in game units

	uint32_t lowW = texVolLowTr ? texVolLowTr->desc.Width : std::max(1u, (resW + kVolCloudDownsample - 1u) / kVolCloudDownsample);
	uint32_t lowH = texVolLowTr ? texVolLowTr->desc.Height : std::max(1u, (resH + kVolCloudDownsample - 1u) / kVolCloudDownsample);

	// Update StructuredBuffer
	VolumetricCloudSB sbData = {
		.rayMarchRange = settings.rayMarchRange / 1.428e-5f,  // km to game units
		.shadowVolumeRange = settings.shadowVolumeRange / 1.428e-5f,
		.cloudMaxStep = settings.cloudMaxStep,
		._pad0 = 0,
		.frameDim = { cbData.frameDim.x, cbData.frameDim.y },
		.rcpFrameDim = { cbData.rcpFrameDim.x, cbData.rcpFrameDim.y },
		.dirlightDir = { cbData.sunDir.x, cbData.sunDir.y, cbData.sunDir.z },
		._pad1 = 0,
		.dirlightColor = { cbData.sunlightColor.x, cbData.sunlightColor.y, cbData.sunlightColor.z },
		._pad2 = 0,
		.bottomZ = cbData.zBottom,
		.planetRadius = cbData.rPlanet,
		.atmosThickness = cbData.rAtmosphere - cbData.rPlanet,
		.aerialPerspectiveMaxDist = apMaxDist,
		.cloudBottom = settings.cloudLayer.bottom / 1.428e-5f,
		.cloudThickness = settings.cloudLayer.thickness / 1.428e-5f,
		.ndfFreq = { 1.428e-5f / settings.cloudLayer.ndfScale.x, 1.428e-5f / settings.cloudLayer.ndfScale.y },
		.noiseFreq = 1.428e-5f / settings.cloudLayer.noiseScale,
		.noiseOffset = { -noiseOffset.x / 1.428e-5f, -noiseOffset.y / 1.428e-5f, -noiseOffset.z / 1.428e-5f },
		.power = settings.cloudLayer.power,
		.cloudScatter = settings.cloudLayer.scatter * 1.428e-5f,
		.cloudAbsorption = settings.cloudLayer.absorption * 1.428e-5f,
		.averageDensity = settings.cloudLayer.averageDensity,
		.msMult = settings.cloudLayer.msMult,
		.msTransmittancePower = settings.cloudLayer.msTransmittancePower,
		.msHeightPower = settings.cloudLayer.msHeightPower,
		.ambientMult = settings.cloudLayer.ambientMult,
		.lowFrameDim = { static_cast<float>(lowW), static_cast<float>(lowH) },
		.rcpLowFrameDim = { 1.0f / static_cast<float>(lowW), 1.0f / static_cast<float>(lowH) },
		.historyValid = volMainHistoryValid ? 1u : 0u,
		.padding = { 0u, 0u, 0u },
	};
	volCloudSb->Update(&sbData, sizeof(sbData));

	// Shared SRVs for both passes
	auto* ndfSrv = ndfManager.GetNdf(ndfSettings, ndfTexManager);
	if (!ndfSrv)
		return;

	std::array<ID3D11ShaderResourceView*, 11> srvs = {
		volCloudSb->SRV(),                                                                                             // t0
		texTrLut->srv.get(),                                                                                           // t1
		texMsLut->srv.get(),                                                                                           // t2
		texApLut->srv.get(),                                                                                           // t3
		renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV,  // t4
		nubisNoiseSrv.get(),                                                                                           // t5
		ndfSrv,                                                                                                        // t6
		cloudTopLutSrv.get(),                                                                                          // t7
		cloudBottomLutSrv.get(),                                                                                       // t8
		texApShadow ? texApShadow->srv.get() : nullptr,                                                                // t9
		texSvLut->srv.get(),                                                                                           // t10
	};

	// Shadow-related SRVs (t20-t23)
	auto& volumetricShadows = globals::features::volumetricShadows;
	auto& terrainShadows = globals::features::terrainShadows;
	ID3D11ShaderResourceView* directionalShadowLights = nullptr;
	if (auto* directionalShadowBuffer = Deferred::GetSingleton()->directionalShadowLights)
		directionalShadowLights = directionalShadowBuffer->srv.get();
	std::array<ID3D11ShaderResourceView*, 4> shadowSrvs = {
		volumetricShadows.shadowView,                                                             // t20
		directionalShadowLights,                                                                  // t21
		terrainShadows.IsHeightMapReady() ? terrainShadows.texShadowHeight->srv.get() : nullptr,  // t22
		nullptr,                                                                                  // t23 - shadow volume (set per pass)
	};

	std::array<ID3D11SamplerState*, 3> samplers = { sampTileable.get(), sampTr.get(), sampSv.get() };
	std::array<ID3D11ShaderResourceView*, 3> outputSrvs = { texVolTr->srv.get(), texVolLum->srv.get(), texShadowVolume->srv.get() };

	context->CSSetSamplers(2, (uint)samplers.size(), samplers.data());

	if (a_pass == VolumetricCloudPass::kShadowVolume) {
		// ===== Pass 1: Shadow Volume =====
		state->BeginPerfEvent("Volumetric Clouds: Shadow Volume");
		std::array<ID3D11UnorderedAccessView*, 2> uavs = { texShadowVolume->uav.get(), nullptr };

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(20, (uint)shadowSrvs.size(), shadowSrvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csVolShadowVolume.get(), nullptr, 0);

		// Dispatch based on dominant sun direction component
		float3 ray_px_dir = { -cbData.sunDir.x, -cbData.sunDir.y, -cbData.sunDir.z };
		ray_px_dir.x *= kShadowVolW / (settings.shadowVolumeRange / 1.428e-5f);
		ray_px_dir.y *= kShadowVolH / (settings.shadowVolumeRange / 1.428e-5f);
		ray_px_dir.z *= kShadowVolD / (settings.cloudLayer.thickness / 1.428e-5f);
		float dir_max_component = std::max(std::max(abs(ray_px_dir.x), abs(ray_px_dir.y)), abs(ray_px_dir.z));
		uint32_t dispatch_size[2];
		if (abs(ray_px_dir.x) == dir_max_component || abs(ray_px_dir.y) == dir_max_component) {
			dispatch_size[0] = kShadowVolW;
			dispatch_size[1] = kShadowVolD;
		} else {
			dispatch_size[0] = dispatch_size[1] = kShadowVolW;
		}

		globals::profiler->BeginPass("PhysicalSky::VolumetricShadowVolume");
		context->Dispatch(dispatch_size[0], dispatch_size[1], 1);
		globals::profiler->EndPass();
		state->EndPerfEvent();
	}

	if (a_pass == VolumetricCloudPass::kMainViewAndCubemap) {
		// ===== Pass 2: Nubis 1/4-resolution Main View Ray March =====
		state->BeginPerfEvent("Volumetric Clouds: Main View");
		std::array<ID3D11UnorderedAccessView*, 3> uavs = { texVolLowTr->uav.get(), texVolLowLum->uav.get(), texVolLowAux->uav.get() };
		shadowSrvs[3] = texShadowVolume->srv.get();

		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(20, (uint)shadowSrvs.size(), shadowSrvs.data());
		context->CSSetShader(csVolMainView.get(), nullptr, 0);

		globals::profiler->BeginPass("PhysicalSky::VolumetricMainView");
		context->Dispatch((lowW + 7u) >> 3, (lowH + 7u) >> 3, 1);
		globals::profiler->EndPass();
		state->EndPerfEvent();

		// ===== Pass 3: Nubis Resample/Upscale =====
		state->BeginPerfEvent("Volumetric Clouds: Resample");
		{
			ID3D11UnorderedAccessView* nullUavs[3] = {};
			context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);

			std::array<ID3D11ShaderResourceView*, 9> passSrvs = {
				texVolHistoryTr->srv.get(),
				texVolHistoryLum->srv.get(),
				texVolHistoryAux->srv.get(),
				texVolLowTr->srv.get(),
				texVolLowLum->srv.get(),
				texVolLowAux->srv.get(),
				nullptr,
				nullptr,
				nullptr,
			};
			uavs = { texVolUpscaleTr->uav.get(), texVolUpscaleLum->uav.get(), texVolUpscaleAux->uav.get() };
			context->CSSetShaderResources(26, (uint)passSrvs.size(), passSrvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(csVolResample.get(), nullptr, 0);
			globals::profiler->BeginPass("PhysicalSky::VolumetricResample");
			context->Dispatch((resW + 7u) >> 3, (resH + 7u) >> 3, 1);
			globals::profiler->EndPass();
		}
		state->EndPerfEvent();

		// ===== Pass 4: Nubis Blur =====
		state->BeginPerfEvent("Volumetric Clouds: Blur");
		{
			ID3D11UnorderedAccessView* nullUavs[3] = {};
			ID3D11ShaderResourceView* nullSrvs[9] = {};
			context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
			context->CSSetShaderResources(26, 9, nullSrvs);

			std::array<ID3D11ShaderResourceView*, 9> passSrvs = {
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				texVolUpscaleTr->srv.get(),
				texVolUpscaleLum->srv.get(),
				texVolUpscaleAux->srv.get(),
			};
			uavs = { texVolTr->uav.get(), texVolLum->uav.get(), texVolAux->uav.get() };
			context->CSSetShaderResources(26, (uint)passSrvs.size(), passSrvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(csVolBlur.get(), nullptr, 0);
			globals::profiler->BeginPass("PhysicalSky::VolumetricBlur");
			context->Dispatch((resW + 7u) >> 3, (resH + 7u) >> 3, 1);
			globals::profiler->EndPass();
		}
		state->EndPerfEvent();

		{
			ID3D11UnorderedAccessView* nullUavs[3] = {};
			ID3D11ShaderResourceView* nullSrvs[9] = {};
			context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
			context->CSSetShaderResources(26, 9, nullSrvs);
			context->CopyResource(texVolHistoryTr->resource.get(), texVolTr->resource.get());
			context->CopyResource(texVolHistoryLum->resource.get(), texVolLum->resource.get());
			context->CopyResource(texVolHistoryAux->resource.get(), texVolAux->resource.get());
			volMainHistoryValid = true;
		}

		// ===== Cubemap Ray March =====
		if (texVolCubeTr && texVolCubeLum) {
			state->BeginPerfEvent("Volumetric Clouds: Cubemap");
			uavs = { texVolCubeTr->uav.get(), texVolCubeLum->uav.get(), nullptr };
			shadowSrvs[3] = texShadowVolume->srv.get();

			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetShaderResources(20, (uint)shadowSrvs.size(), shadowSrvs.data());
			context->CSSetShader(csVolCubemap.get(), nullptr, 0);

			globals::profiler->BeginPass("PhysicalSky::VolumetricCubemap");
			context->Dispatch((kVolCubeSize + 7u) >> 3, (kVolCubeSize + 7u) >> 3, 6);
			globals::profiler->EndPass();
			state->EndPerfEvent();

			if (csVolCubemapHistory && volCubeHistoryCb && texVolCubeTrHistory && texVolCubeLumHistory) {
				state->BeginPerfEvent("Volumetric Clouds: Cubemap History");

				const VolumetricCloudCubeHistoryCB historyCbData = {
					.historyWeight = volCubeHistoryValid ? 0.875f : 0.0f,
					._pad0 = { 0.0f, 0.0f, 0.0f }
				};
				volCubeHistoryCb->Update(historyCbData);

				std::array<ID3D11ShaderResourceView*, 2> historySrvs = {
					texVolCubeTrHistory->srv.get(),
					texVolCubeLumHistory->srv.get()
				};
				auto* historyCb = volCubeHistoryCb->CB();

				context->CSSetShaderResources(24, (uint)historySrvs.size(), historySrvs.data());
				context->CSSetConstantBuffers(1, 1, &historyCb);
				context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
				context->CSSetShader(csVolCubemapHistory.get(), nullptr, 0);

				globals::profiler->BeginPass("PhysicalSky::VolumetricCubemapHistory");
				context->Dispatch((kVolCubeSize + 7u) >> 3, (kVolCubeSize + 7u) >> 3, 6);
				globals::profiler->EndPass();

				ID3D11UnorderedAccessView* nullUavs[2] = {};
				ID3D11ShaderResourceView* nullHistorySrvs[2] = {};
				context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
				context->CSSetShaderResources(24, 2, nullHistorySrvs);
				context->CopyResource(texVolCubeTrHistory->resource.get(), texVolCubeTr->resource.get());
				context->CopyResource(texVolCubeLumHistory->resource.get(), texVolCubeLum->resource.get());
				volCubeHistoryValid = true;

				state->EndPerfEvent();
			}
		}
	}

	// Cleanup
	{
		ID3D11ShaderResourceView* nullSrvs[11] = {};
		ID3D11ShaderResourceView* nullShadowSrvs[4] = {};
		ID3D11ShaderResourceView* nullHistorySrvs[11] = {};
		ID3D11UnorderedAccessView* nullUavs[3] = {};
		ID3D11Buffer* nullCb[1] = {};
		ID3D11SamplerState* nullSamplers[3] = {};
		context->CSSetShaderResources(0, 11, nullSrvs);
		context->CSSetShaderResources(20, 4, nullShadowSrvs);
		context->CSSetShaderResources(24, 11, nullHistorySrvs);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetConstantBuffers(1, 1, nullCb);
		context->CSSetSamplers(2, 3, nullSamplers);
		context->CSSetShader(nullptr, nullptr, 0);

		// Make results available to any later pixel-shader passes that sample PhysicalSky/Common.hlsli.
		context->PSSetShaderResources(110, (uint)outputSrvs.size(), outputSrvs.data());
	}
}
