#include "Features/PhysicalSky.h"

#include "Deferred.h"
#include "Features/TerrainShadows.h"
#include "Features/VolumetricShadows.h"
#include "State.h"
#include "Util.h"

#include <DDSTextureLoader.h>
#include <DirectXTex.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
	constexpr float KilometersToGameUnits(float value) { return value / Util::Units::GAME_UNIT_TO_KM; }
	constexpr float MetersToGameUnits(float value) { return value / Util::Units::GAME_UNIT_TO_M; }

	bool IsVolumeTexture(ID3D11ShaderResourceView* srv)
	{
		if (!srv)
			return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		srv->GetDesc(&desc);
		return desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D;
	}
}

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

	loadDDS(L"Data\\Textures\\PhysicalSky\\nubis.dds", baseShapeNoiseSrv);
	if (baseShapeNoiseSrv && !IsVolumeTexture(baseShapeNoiseSrv.get())) {
		logger::warn("Ignoring Nubis noise composite because it is not a 3D texture.");
		baseShapeNoiseSrv = nullptr;
	}

	loadDDS(L"Data\\Textures\\PhysicalSky\\top_lut.dds", cloudTopLutSrv);
	loadDDS(L"Data\\Textures\\PhysicalSky\\bottom_lut.dds", cloudBottomLutSrv);
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
	}

	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 3,
			.Height = 1,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
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

		texVolCloudAmbientSH = eastl::make_unique<Texture2D>(texDesc, "PhysicalSky::VolumetricCloudAmbientSH");
		texVolCloudAmbientSH->CreateSRV(srvDesc);
		texVolCloudAmbientSH->CreateUAV(uavDesc);
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

	// Realtime layout: quarter-resolution trace, half-resolution temporal
	// reprojection/history, then depth-aware full-resolution upscale.
	{
		const uint32_t lowW = std::max(1u, (mainDesc.Width + kVolCloudDownsample - 1u) / kVolCloudDownsample);
		const uint32_t lowH = std::max(1u, (mainDesc.Height + kVolCloudDownsample - 1u) / kVolCloudDownsample);
		const uint32_t intermediateW = std::max(1u, (mainDesc.Width + 1u) / 2u);
		const uint32_t intermediateH = std::max(1u, (mainDesc.Height + 1u) / 2u);

		texVolLowTr = createCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricLowTr");
		texVolLowLum = createCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricLowLum");
		texVolLowAux = createCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricLowAux");
		texVolUpscaleTr = createCloudTexture(intermediateW, intermediateH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricIntermediateTr");
		texVolUpscaleLum = createCloudTexture(intermediateW, intermediateH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricIntermediateLum");
		texVolUpscaleAux = createCloudTexture(intermediateW, intermediateH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricIntermediateAux");
		texVolTr = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricTr");
		texVolLum = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricLum");
		texVolAux = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricAux");
		texVolHistoryTr = createCloudTexture(intermediateW, intermediateH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricHistoryTr");
		texVolHistoryLum = createCloudTexture(intermediateW, intermediateH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricHistoryLum");
		texVolHistoryAux = createCloudTexture(intermediateW, intermediateH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolumetricHistoryAux");
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

		FLOAT trClr[4] = { 1.f, 1.f, 1.f, 1.f };
		FLOAT lumClr[4] = { 0.f, 0.f, 0.f, 0.f };
		context->ClearUnorderedAccessViewFloat(texVolCubeTr->uav.get(), trClr);
		context->ClearUnorderedAccessViewFloat(texVolCubeLum->uav.get(), lumClr);
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

		texShadowVolume = eastl::make_unique<Texture3D>(tex3d_desc, "PhysicalSky::VolumetricCloudShadowVolume");
		texShadowVolume->CreateSRV(srv_desc);
		texShadowVolume->CreateUAV(uav_desc);

		FLOAT shadowVolumeClr[4] = { 0.f, 0.f, 0.f, 0.f };
		context->ClearUnorderedAccessViewFloat(texShadowVolume->uav.get(), shadowVolumeClr);
	}

	// Load textures and NDF
	LoadCloudTextures();
	ndfManager.SetupResources();
	highCloudMapManager.SetupResources();

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
		ShaderInfo{ &csVolAmbientSH, "Volumetrics.cs.hlsl", {}, "buildCloudAmbientSH" },
		ShaderInfo{ &csVolMainView, "Volumetrics.cs.hlsl" },
		ShaderInfo{ &csVolReproject, "Volumetrics.cs.hlsl", {}, "reproject" },
		ShaderInfo{ &csVolUpscale, "Volumetrics.cs.hlsl", {}, "upscale" },
		ShaderInfo{ &csVolShadowVolume, "Volumetrics.cs.hlsl", {}, "renderShadowVolume" },
		ShaderInfo{ &csVolCubemap, "Volumetrics.cs.hlsl", {}, "renderCubemap" }
	};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.data())))
			info.csPtr->attach(rawPtr);
	}
	ndfManager.CompileShaders();
	highCloudMapManager.CompileShaders();
}

void PhysicalSky::RenderVolumetricClouds(VolumetricCloudPass a_pass)
{
	if (!csVolMainView || !csVolReproject || !csVolUpscale || !csVolShadowVolume || !csVolCubemap || !csVolAmbientSH)
		return;
	if (!baseShapeNoiseSrv || !cloudTopLutSrv || !cloudBottomLutSrv)
		return;

	auto state = globals::state;
	auto context = globals::d3d::context;
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	float3 cloudLightDir = cbData.sunDir;
	if (auto* shaderManager = globals::game::smState) {
		if (auto* shadowSceneNode = shaderManager->shadowSceneNode[0]) {
			auto shadowSunLight = shadowSceneNode->GetRuntimeData().sunLight;
			if (shadowSunLight && shadowSunLight->light) {
				if (auto* dirLight = skyrim_cast<RE::NiDirectionalLight*>(shadowSunLight->light.get())) {
					const auto& direction = dirLight->GetWorldDirection();
					cloudLightDir = { -direction.x, -direction.y, -direction.z };
					const float lengthSq = cloudLightDir.x * cloudLightDir.x + cloudLightDir.y * cloudLightDir.y + cloudLightDir.z * cloudLightDir.z;
					if (lengthSq > 1e-6f) {
						const float invLength = 1.0f / std::sqrt(lengthSq);
						cloudLightDir.x *= invLength;
						cloudLightDir.y *= invLength;
						cloudLightDir.z *= invLength;
					} else {
						cloudLightDir = cbData.sunDir;
					}
				}
			}
		}
	}
	const uint32_t textureW = (uint32_t)cbData.texDim.x;
	const uint32_t textureH = (uint32_t)cbData.texDim.y;
	const uint32_t renderW = (uint32_t)cbData.frameDim.x;
	const uint32_t renderH = (uint32_t)cbData.frameDim.y;
	if (volHistoryWidth != textureW || volHistoryHeight != textureH) {
		volMainHistoryValid = false;
		volHistoryWidth = textureW;
		volHistoryHeight = textureH;
	}
	const float sunHistoryDot = std::clamp(
		volHistorySunDir.x * cloudLightDir.x + volHistorySunDir.y * cloudLightDir.y + volHistorySunDir.z * cloudLightDir.z,
		-1.0f,
		1.0f);
	const float sunAngleDifferenceDegrees = std::acos(sunHistoryDot) * (180.0f / std::numbers::pi_v<float>);
	const float cloudHistoryInvalidation = std::clamp(1.0f - sunAngleDifferenceDegrees / 10.0f, 0.0f, 1.0f);

	auto& low = settings.cloudLayer.low;
	auto& high = settings.cloudLayer.high;
	auto& lighting = settings.cloudLayer.lighting;
	auto& phi = settings.cloudLayer.phiFwd;

	low.thickness = std::clamp(low.thickness, 0.05f, 3.0f);
	low.ndfScale.x = std::clamp(low.ndfScale.x, 1.0f, 50.0f);
	low.ndfScale.y = std::clamp(low.ndfScale.y, 1.0f, 50.0f);
	low.noiseCompositeScale = std::clamp(low.noiseCompositeScale, 0.02f, 8.0f);
	high.weatherDim = std::clamp(high.weatherDim, 128u, 1024u);
	high.weatherWorldSize = std::clamp(high.weatherWorldSize, 8.0f, 256.0f);

	const float timeSeconds = state->timer * 1e-3f;
	const float windLen = std::sqrt(low.windDirection.x * low.windDirection.x + low.windDirection.y * low.windDirection.y);
	const float2 windDir = windLen > 1e-4f ? low.windDirection / windLen : float2{ 1.f, 0.f };
	const float2 noiseWindOffset = windDir * MetersToGameUnits(low.windSpeed * timeSeconds);
	const float2 crossWind = { -windDir.y, windDir.x };
	const float2 shearDirection = (windDir * 0.78f + crossWind * 0.36f) / std::sqrt(0.78f * 0.78f + 0.36f * 0.36f);
	const float2 noiseHeightShear = shearDirection * KilometersToGameUnits(low.noiseCompositeScale * 0.04f);
	const float lowCloudBaseKm = std::max(low.baseAltitude, 0.0f);
	const float lowCloudDepthKm = low.thickness;
	const float lowCloudTopKm = lowCloudBaseKm + lowCloudDepthKm;
	const float lowTraceDepthKm = lowCloudDepthKm;
	const float lowCloudTraceTopKm = lowCloudBaseKm + lowTraceDepthKm;
	const float highCloudBottomKm = std::max(high.bottomAltitude, 0.0f);
	const float highCloudTopKm = std::max(high.topAltitude, highCloudBottomKm + 0.1f);
	const float traceBottomKm = high.enabled ? std::min(lowCloudBaseKm, highCloudBottomKm) : lowCloudBaseKm;
	const float traceTopKm = high.enabled ? std::max(lowCloudTraceTopKm, highCloudTopKm) : lowCloudTraceTopKm;

	constexpr bool fullResolutionMainView = false;
	const uint32_t lowW = texVolLowTr->desc.Width;
	const uint32_t lowH = texVolLowTr->desc.Height;

	// Update StructuredBuffer
	VolumetricCloudSB sbData = {
		.rayMarchRange = KilometersToGameUnits(settings.rayMarchRange),
		.shadowVolumeRange = KilometersToGameUnits(settings.shadowVolumeRange),
		.cloudMaxStep = settings.cloudMaxStep,
		.fullResolution = fullResolutionMainView ? 1u : 0u,
		.frameDim = { cbData.texDim.x, cbData.texDim.y },
		.rcpFrameDim = { cbData.rcpTexDim.x, cbData.rcpTexDim.y },
		.dirlightDir = cloudLightDir,
		._pad1 = 0,
		.bottomZ = cbData.zBottom,
		.planetRadius = cbData.rPlanet,
		.activeFrameDim = { static_cast<float>(renderW), static_cast<float>(renderH) },
		.lowestCloudAltitude = KilometersToGameUnits(traceBottomKm),
		.highestCloudAltitude = KilometersToGameUnits(traceTopKm),
		.lowCloudBaseAltitude = KilometersToGameUnits(lowCloudBaseKm),
		.lowCloudTopAltitude = KilometersToGameUnits(lowCloudTopKm),
		.lowCloudTraceTopAltitude = KilometersToGameUnits(lowCloudTraceTopKm),
		.weatherCenter = { KilometersToGameUnits(high.weatherCenter.x), KilometersToGameUnits(high.weatherCenter.y) },
		.weatherWorldSize = KilometersToGameUnits(high.weatherWorldSize),
		.highCloudEnabled = high.enabled ? 1.0f : 0.0f,
		.lowNdfFrequency = { Util::Units::GAME_UNIT_TO_KM / low.ndfScale.x, Util::Units::GAME_UNIT_TO_KM / low.ndfScale.y },
		.noiseWindOffset = noiseWindOffset,
		// Physical repeat length of the authored 128^3 RGBA Nubis noise composite.
		.noiseFrequency = 1.0f / KilometersToGameUnits(low.noiseCompositeScale),
		.noiseOffset = low.noiseOffset,
		.extinctionCoefficient = low.extinctionCoefficient,
		.noiseHeightShear = noiseHeightShear,
		._padNoise = 0.0f,
		.highCellScale = high.cellScale,
		.highCellWindSpeed = high.cellWindSpeed,
		.highCellWarpScale = high.cellWarpScale,
		.highCellWarpStrength = high.cellWarpStrength,
		.highCellThickStrength = high.cellThickStrength,
		.highAsCellThickStrength = high.asCellThickStrength,
		.highCellThickPow = high.cellThickPow,
		.highCloudBottom = KilometersToGameUnits(highCloudBottomKm),
		.highCloudTop = KilometersToGameUnits(highCloudTopKm),
		.highBottomCoverageScale = high.bottomCoverageScale,
		.highHeightCurvePow = high.heightCurvePow,
		.highDensityThreshold = high.densityThreshold,
		.highDensitySoftness = high.densitySoftness,
		.highCloudSoftness = high.softness,
		.highWispScale = high.wispScale,
		.highWispStrength = high.wispStrength,
		.highDensityMultiplier = high.densityMultiplier,
		.highDensitySoftAIntensity = high.densitySoftAIntensity,
		.highDensitySoftAContrast = high.densitySoftAContrast,
		.highDensityModAIntensity = high.densityModAIntensity,
		.highDensityModAContrast = high.densityModAContrast,
		.scatterTint = lighting.scatterTint,
		.forwardEccentricity = lighting.forwardEccentricity,
		.backwardEccentricity = lighting.backwardEccentricity,
		.ambientTopMultiplier = lighting.ambientTopMultiplier,
		.ambientBottomMultiplier = lighting.ambientBottomMultiplier,
		.aoUpwardScale = lighting.aoUpwardScale,
		.msAttenuation = lighting.msAttenuation,
		.msContribution = lighting.msContribution,
		.msEccentricity = lighting.msEccentricity,
		.scatterSourceODScale = lighting.scatterSourceODScale,
		.scatterSourceCurvePow = lighting.scatterSourceCurvePow,
		.powderIntensity = lighting.powderIntensity,
		.lightSteps = lighting.lightSteps,
		.cloudPhaseModel = lighting.phaseModel,
		.phiFwdIntensity = phi.intensity,
		.phiFwdDepthPow = phi.depthPow,
		.phiFwdDepthBias = phi.depthBias,
		.phiFwdBoundaryConfidence = phi.boundaryConfidence,
		.phiFwdMSBuildScale = phi.msBuildScale,
		.phiFwdCompress = phi.compress,
		.highForwardEccentricity = high.forwardEccentricity,
		.highBackwardEccentricity = high.backwardEccentricity,
		.highAmbientTopMultiplier = high.ambientTopMultiplier,
		.highAmbientBottomMultiplier = high.ambientBottomMultiplier,
		.highSkyBlendStrength = high.skyBlendStrength,
		.highMSAttenuation = high.msAttenuation,
		.highMSContribution = high.msContribution,
		.highMSEccentricity = high.msEccentricity,
		.highLightAbsorption = high.lightAbsorption,
		.highViewAbsorption = high.viewAbsorption,
		.highCoverAbsorptionStrength = high.coverAbsorptionStrength,
		.lowFrameDim = { static_cast<float>((renderW + 3u) / 4u), static_cast<float>((renderH + 3u) / 4u) },
		.rcpLowFrameDim = { 1.0f / static_cast<float>((renderW + 3u) / 4u), 1.0f / static_cast<float>((renderH + 3u) / 4u) },
		.historyValid = volMainHistoryValid ? 1u : 0u,
		.temporalAccumulationFactor = std::clamp(settings.temporalAccumulationFactor, 0.0f, 1.0f),
		.cloudHistoryInvalidation = cloudHistoryInvalidation,
		.ghostingReduction = settings.ghostingReduction ? 1u : 0u,
		.scatterIntegration = lighting.scatterIntegration,
		.lightStepDistanceLod = std::clamp(lighting.lightStepDistanceLod, 0.0f, 1.0f),
		.shadowVolumeBottom = KilometersToGameUnits(lowCloudBaseKm),
		.shadowVolumeTop = KilometersToGameUnits(lowCloudTopKm),
	};
	volCloudSb->Update(&sbData, sizeof(sbData));

	// Shared SRVs for both passes
	auto* ndfSrv = ndfManager.GetNdf(settings.cloudMap, ndfTexManager);
	auto highTextures = highCloudMapManager.GetTextures(high);
	if (!ndfSrv || (high.enabled && (!highTextures.highWeather || !highTextures.highCell || !highTextures.highWarp || !highTextures.highWisp)))
		return;

	std::array<ID3D11ShaderResourceView*, 19> srvs = {
		volCloudSb->SRV(),                                                                                             // t0
		texTrLut->srv.get(),                                                                                           // t1
		texMsLut->srv.get(),                                                                                           // t2
		texApLut->srv.get(),                                                                                           // t3
		renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV,  // t4
		baseShapeNoiseSrv.get(),                                                                                       // t5 authored Nubis RGBA noise composite
		texApSunLut->srv.get(),                                                                                        // t6 direct solar single-scattering AP LUT
		ndfSrv,                                                                                                        // t7 five-layer NDF
		nullptr,                                                                                                       // t8 unused by Nubis low clouds
		texApShadow ? texApShadow->srv.get() : nullptr,                                                                // t9
		texSvLut->srv.get(),                                                                                           // t10
		highTextures.highWeather,                                                                                      // t11
		nullptr,                                                                                                       // t12 unused by Nubis low clouds
		highTextures.highCell,                                                                                         // t13
		highTextures.highWarp,                                                                                         // t14
		highTextures.highWisp,                                                                                         // t15
		nullptr,                                                                                                       // t16 ambient SH (bound per pass)
		cloudTopLutSrv.get(),                                                                                          // t17 Nubis top profile
		cloudBottomLutSrv.get(),                                                                                       // t18 Nubis bottom profile
	};
	ID3D11ShaderResourceView* ambientShSrv = texVolCloudAmbientSH ? texVolCloudAmbientSH->srv.get() : nullptr;
	if (a_pass == VolumetricCloudPass::kMainViewAndCubemap && texVolCloudAmbientSH) {
		state->BeginPerfEvent("Volumetric Clouds: Ambient SH");
		std::array<ID3D11UnorderedAccessView*, 1> ambientUavs = { texVolCloudAmbientSH->uav.get() };
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)ambientUavs.size(), ambientUavs.data(), nullptr);
		context->CSSetShader(csVolAmbientSH.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricCloudAmbientSH");
		context->Dispatch(1, 1, 1);
		globals::profiler->EndPass();

		ID3D11UnorderedAccessView* nullUav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		state->EndPerfEvent();
	}

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
		// Shadow path: accumulate the cloud extinction column into the 3D shadow
		// volume along the light direction.
		state->BeginPerfEvent("Volumetric Clouds: Shadow Volume");
		std::array<ID3D11UnorderedAccessView*, 2> uavs = { texShadowVolume->uav.get(), nullptr };
		ID3D11ShaderResourceView* nullPsShadowSrv = nullptr;
		context->PSSetShaderResources(112, 1, &nullPsShadowSrv);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(16, 1, &ambientShSrv);
		context->CSSetShaderResources(20, (uint)shadowSrvs.size(), shadowSrvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csVolShadowVolume.get(), nullptr, 0);

		// Dispatch based on dominant light direction component
		const float shadowRangeGu = KilometersToGameUnits(settings.shadowVolumeRange);
		const float shadowThicknessGu = KilometersToGameUnits(lowCloudTopKm - lowCloudBaseKm);
		float3 ray_px_dir = { -cloudLightDir.x, -cloudLightDir.y, -cloudLightDir.z };
		ray_px_dir.x *= kShadowVolW / shadowRangeGu;
		ray_px_dir.y *= kShadowVolH / shadowRangeGu;
		ray_px_dir.z *= kShadowVolD / shadowThicknessGu;
		const float dir_max_component = std::max({ std::abs(ray_px_dir.x), std::abs(ray_px_dir.y), std::abs(ray_px_dir.z) });
		uint32_t dispatch_size[2];
		if (std::abs(ray_px_dir.x) == dir_max_component || std::abs(ray_px_dir.y) == dir_max_component) {
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
		// Pass 1: quarter-resolution checkerboard ray trace.
		state->BeginPerfEvent("Volumetric Clouds: Main View");
		std::array<ID3D11UnorderedAccessView*, 3> uavs = fullResolutionMainView ?
		                                                     std::array<ID3D11UnorderedAccessView*, 3>{ texVolUpscaleTr->uav.get(), texVolUpscaleLum->uav.get(), texVolUpscaleAux->uav.get() } :
		                                                     std::array<ID3D11UnorderedAccessView*, 3>{ texVolLowTr->uav.get(), texVolLowLum->uav.get(), texVolLowAux->uav.get() };
		shadowSrvs[3] = texShadowVolume->srv.get();

		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(16, 1, &ambientShSrv);
		context->CSSetShaderResources(20, (uint)shadowSrvs.size(), shadowSrvs.data());
		context->CSSetShader(csVolMainView.get(), nullptr, 0);

		globals::profiler->BeginPass("PhysicalSky::VolumetricMainView");
		context->Dispatch((lowW + 7u) >> 3, (lowH + 7u) >> 3, 1);
		globals::profiler->EndPass();
		state->EndPerfEvent();

		// Pass 2: quarter-resolution trace -> half-resolution temporal reprojection.
		{
			ID3D11UnorderedAccessView* nullUavs[3] = {};
			context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);

			std::array<ID3D11ShaderResourceView*, 9> passSrvs = {
				texVolHistoryTr->srv.get(), texVolHistoryLum->srv.get(), texVolHistoryAux->srv.get(),
				texVolLowTr->srv.get(), texVolLowLum->srv.get(), texVolLowAux->srv.get(),
				nullptr, nullptr, nullptr
			};
			uavs = { texVolUpscaleTr->uav.get(), texVolUpscaleLum->uav.get(), texVolUpscaleAux->uav.get() };
			context->CSSetShaderResources(26, (uint)passSrvs.size(), passSrvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(csVolReproject.get(), nullptr, 0);
			globals::profiler->BeginPass("PhysicalSky::VolumetricReproject");
			context->Dispatch((texVolUpscaleTr->desc.Width + 7u) >> 3, (texVolUpscaleTr->desc.Height + 7u) >> 3, 1);
			globals::profiler->EndPass();
		}

		// Pass 3: half-resolution accumulated result -> full-resolution bilateral upscale.
		{
			ID3D11UnorderedAccessView* nullUavs[3] = {};
			ID3D11ShaderResourceView* nullSrvs[9] = {};
			context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
			context->CSSetShaderResources(26, 9, nullSrvs);
			std::array<ID3D11ShaderResourceView*, 9> passSrvs = {
				nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
				texVolUpscaleTr->srv.get(), texVolUpscaleLum->srv.get(), texVolUpscaleAux->srv.get()
			};
			uavs = { texVolTr->uav.get(), texVolLum->uav.get(), texVolAux->uav.get() };
			context->CSSetShaderResources(26, (uint)passSrvs.size(), passSrvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(csVolUpscale.get(), nullptr, 0);
			globals::profiler->BeginPass("PhysicalSky::VolumetricUpscale");
			context->Dispatch((renderW + 7u) >> 3, (renderH + 7u) >> 3, 1);
			globals::profiler->EndPass();
		}

		// Ping-pong is unnecessary because the previous history is consumed before these copies.
		{
			ID3D11UnorderedAccessView* nullUavs[3] = {};
			ID3D11ShaderResourceView* nullSrvs[9] = {};
			context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
			context->CSSetShaderResources(26, 9, nullSrvs);
			context->CopyResource(texVolHistoryTr->resource.get(), texVolUpscaleTr->resource.get());
			context->CopyResource(texVolHistoryLum->resource.get(), texVolUpscaleLum->resource.get());
			context->CopyResource(texVolHistoryAux->resource.get(), texVolUpscaleAux->resource.get());
			volMainHistoryValid = true;
			volHistorySunDir = cloudLightDir;
		}

		// ===== Cubemap Ray March =====
		if (texVolCubeTr && texVolCubeLum) {
			state->BeginPerfEvent("Volumetric Clouds: Cubemap");
			uavs = { texVolCubeTr->uav.get(), texVolCubeLum->uav.get(), nullptr };
			shadowSrvs[3] = texShadowVolume->srv.get();

			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetShaderResources(16, 1, &ambientShSrv);
			context->CSSetShaderResources(20, (uint)shadowSrvs.size(), shadowSrvs.data());
			context->CSSetShader(csVolCubemap.get(), nullptr, 0);

			globals::profiler->BeginPass("PhysicalSky::VolumetricCubemap");
			// The shader maps these two slices to one opposite-face pair and cycles
			// through all three pairs over consecutive frames.
			context->Dispatch((kVolCubeSize + 7u) >> 3, (kVolCubeSize + 7u) >> 3, 2);
			globals::profiler->EndPass();
			state->EndPerfEvent();
		}
	}

	// Cleanup
	{
		ID3D11ShaderResourceView* nullSrvs[19] = {};
		ID3D11ShaderResourceView* nullShadowSrvs[4] = {};
		ID3D11ShaderResourceView* nullHistorySrvs[11] = {};
		ID3D11UnorderedAccessView* nullUavs[3] = {};
		ID3D11Buffer* nullCb[1] = {};
		ID3D11SamplerState* nullSamplers[3] = {};
		context->CSSetShaderResources(0, 19, nullSrvs);
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
