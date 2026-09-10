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
#include <cstdint>
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
	volMainHistoryValid = false;
	volLightCacheValid = false;
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

	constexpr DXGI_FORMAT historyRadianceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	const uint32_t lowW = std::max(1u, (mainDesc.Width + 3u) / 4u);
	const uint32_t lowH = std::max(1u, (mainDesc.Height + 3u) / 4u);
	texVolLowTr = createCloudTexture(lowW, lowH, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::CloudTraceTr");
	texVolLowLum = createCloudTexture(lowW, lowH, historyRadianceFormat, "PhysicalSky::CloudTraceLum");
	texVolLowAux = createCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CloudTraceAux");
	texVolTr = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::CloudTr");
	texVolLum = createCloudTexture(mainDesc.Width, mainDesc.Height, historyRadianceFormat, "PhysicalSky::CloudLum");
	texVolAux = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CloudAux");
	texVolHistoryTr = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::CloudHistoryTr");
	texVolHistoryLum = createCloudTexture(mainDesc.Width, mainDesc.Height, historyRadianceFormat, "PhysicalSky::CloudHistoryLum");
	texVolHistoryAux = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CloudHistoryAux");

	auto createCube = [](uint32_t size, DXGI_FORMAT format, const char* name) {
		D3D11_TEXTURE2D_DESC desc = {
			.Width = size,
			.Height = size,
			.MipLevels = 1,
			.ArraySize = 6,
			.Format = format,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srv = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray = { .MipSlice = 0, .FirstArraySlice = 0, .ArraySize = 6 }
		};
		auto texture = eastl::make_unique<Texture2D>(desc, name);
		texture->CreateSRV(srv);
		texture->CreateUAV(uav);
		return texture;
	};
	texVolCubeTr = createCube(kVolCubeSize, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::VolCubeTr");
	texVolCubeLum = createCube(kVolCubeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeLum");
	texVolCubeAux = createCube(kVolCubeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeAux");
	texVolCubeHistoryTr = createCube(kVolCubeSize, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::VolCubeHistoryTr");
	texVolCubeHistoryLum = createCube(kVolCubeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeHistoryLum");
	texVolCubeHistoryAux = createCube(kVolCubeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeHistoryAux");
	texVolCubeTraceTr = createCube(kVolCubeSize / 4u, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::VolCubeTraceTr");
	texVolCubeTraceLum = createCube(kVolCubeSize / 4u, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeTraceLum");
	texVolCubeTraceAux = createCube(kVolCubeSize / 4u, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeTraceAux");
	const float clearTr[4] = { 1.f, 1.f, 1.f, 1.f };
	const float clearLum[4] = {};
	context->ClearUnorderedAccessViewFloat(texVolCubeTr->uav.get(), clearTr);
	context->ClearUnorderedAccessViewFloat(texVolCubeLum->uav.get(), clearLum);
	context->ClearUnorderedAccessViewFloat(texVolCubeAux->uav.get(), clearLum);
	volMainHistoryValid = false;
	volLightCacheValid = false;

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
		tex3d_desc.Width = 64;
		tex3d_desc.Height = 64;
		tex3d_desc.Depth = 16;
		tex3d_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
		srv_desc.Format = tex3d_desc.Format;
		uav_desc.Format = tex3d_desc.Format;
		uav_desc.Texture3D.WSize = tex3d_desc.Depth;
		texLowCloudLightCache = eastl::make_unique<Texture3D>(tex3d_desc, "PhysicalSky::LowCloudLightCache");
		texLowCloudLightCache->CreateSRV(srv_desc);
		texLowCloudLightCache->CreateUAV(uav_desc);
		texHighCloudLightCache = eastl::make_unique<Texture3D>(tex3d_desc, "PhysicalSky::HighCloudLightCache");
		texHighCloudLightCache->CreateSRV(srv_desc);
		texHighCloudLightCache->CreateUAV(uav_desc);
	}

	// Load textures and NDF
	LoadCloudTextures();
	ndfManager.SetupResources();
	highCloudMapManager.SetupResources();

	CompileVolumetricShaders();
}

void PhysicalSky::CompileVolumetricShaders()
{
	volMainHistoryValid = false;
	volLightCacheValid = false;
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
		ShaderInfo{ &csVolCubeReproject, "Volumetrics.cs.hlsl", {}, "reprojectCubemap" },
		ShaderInfo{ &csVolShadowVolume, "Volumetrics.cs.hlsl", {}, "renderShadowVolume" },
		ShaderInfo{ &csVolLowLightCache, "Volumetrics.cs.hlsl", {}, "buildCloudLightCache" },
		ShaderInfo{ &csVolHighLightCache, "Volumetrics.cs.hlsl", { { "HIGH_CLOUD_CACHE", "1" } }, "buildCloudLightCache" },
		ShaderInfo{ &csVolCubemap, "Volumetrics.cs.hlsl", {}, "renderCubemap" }
	};

	for (auto& info : shaderInfos) {
		*info.csPtr = nullptr;
		auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.data())))
			info.csPtr->attach(rawPtr);
	}
	ndfManager.CompileShaders();
	highCloudMapManager.CompileShaders();
}

void PhysicalSky::RenderVolumetricClouds(VolumetricCloudPass a_pass)
{
	if (!csVolMainView || !csVolReproject || !csVolCubeReproject || !csVolLowLightCache || !csVolHighLightCache || !csVolShadowVolume || !csVolCubemap || !csVolAmbientSH)
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
	// Ground shadow directions may be elevation-clamped or switched to a moon
	// while elevated clouds are still sunlit. Trace their actual solar column.
	if (cbData.volCloudUseSun != 0)
		cloudLightDir = cbData.sunDir;
	const uint32_t renderW = (uint32_t)cbData.frameDim.x;
	const uint32_t renderH = (uint32_t)cbData.frameDim.y;
	if (volHistoryWidth != renderW || volHistoryHeight != renderH) {
		volMainHistoryValid = false;
		volHistoryWidth = renderW;
		volHistoryHeight = renderH;
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
	const float elapsed = timeSeconds - volHistoryTime;
	if (elapsed < 0.0f || elapsed > 0.25f) {
		volMainHistoryValid = false;
		volLightCacheValid = false;
	}
	const float2 windDelta = volMainHistoryValid ? noiseWindOffset - volHistoryWindOffset : float2{ 0.f, 0.f };
	const float lowHistoryConfidence = 1.f;
	const float windDeltaMeters = std::sqrt(windDelta.x * windDelta.x + windDelta.y * windDelta.y) * Util::Units::GAME_UNIT_TO_M;
	const float highRelativeMotion = windDeltaMeters * std::abs(high.cellWindSpeed) /
	                                 (high.weatherWorldSize * 1000.f) * high.weatherDim;
	const float highHistoryConfidence = std::exp(-highRelativeMotion);
	const float lowCloudBaseKm = std::max(low.baseAltitude, 0.0f);
	const float lowCloudDepthKm = low.thickness;
	const float lowCloudTopKm = lowCloudBaseKm + lowCloudDepthKm;
	const float lowTraceDepthKm = lowCloudDepthKm;
	const float lowCloudTraceTopKm = lowCloudBaseKm + lowTraceDepthKm;
	const float highCloudBottomKm = std::max(high.bottomAltitude, 0.0f);
	const float highCloudTopKm = std::max(high.topAltitude, highCloudBottomKm + 0.1f);
	const float traceBottomKm = high.enabled ? std::min(lowCloudBaseKm, highCloudBottomKm) : lowCloudBaseKm;
	const float traceTopKm = high.enabled ? std::max(lowCloudTraceTopKm, highCloudTopKm) : lowCloudTraceTopKm;

	const uint32_t lowW = texVolLowTr->desc.Width;
	const uint32_t lowH = texVolLowTr->desc.Height;

	const auto cameraPosition = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float2 cameraXY = { cameraPosition.x, cameraPosition.y };
	const float2 cacheMovement = cameraXY - volLightCacheOrigin - (noiseWindOffset - volLightCacheWind);
	const bool rebuildLightCache = a_pass == VolumetricCloudPass::kMainViewAndCubemap &&
	                               (!volLightCacheValid || sunAngleDifferenceDegrees > 0.25f ||
									   cacheMovement.x * cacheMovement.x + cacheMovement.y * cacheMovement.y > MetersToGameUnits(1000.f) * MetersToGameUnits(1000.f));
	if (rebuildLightCache) {
		volLightCacheValid = false;
		volLightCacheOrigin = cameraXY;
		volLightCacheWind = noiseWindOffset;
	}

	// Update StructuredBuffer
	VolumetricCloudSB sbData = {
		.rayMarchRange = KilometersToGameUnits(settings.rayMarchRange),
		.shadowVolumeRange = KilometersToGameUnits(settings.shadowVolumeRange),
		.lowViewSteps = std::clamp(settings.lowViewSteps, 32u, 512u),
		.cloudFrameIndex = volFrameIndex,
		.rcpFrameDim = { cbData.rcpTexDim.x, cbData.rcpTexDim.y },
		.dirlightDir = cloudLightDir,
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
		.noiseRoundness = std::clamp(low.noiseRoundness, 0.0f, 1.0f),
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
		.highDensitySoftAContrast = high.densitySoftAContrast,
		.highDensityModAIntensity = high.densityModAIntensity,
		.highDensityModAContrast = high.densityModAContrast,
		.scatterTint = lighting.scatterTint,
		.forwardEccentricity = lighting.forwardEccentricity,
		.backwardEccentricity = lighting.backwardEccentricity,
		.ambientTopMultiplier = lighting.ambientTopMultiplier,
		.ambientBottomMultiplier = lighting.ambientBottomMultiplier,
		.aoUpwardScale = lighting.aoUpwardScale,
		.msDepthPower = std::clamp(lighting.msDepthPower, 0.01f, 1.f),
		.msContribution = lighting.msContribution,
		.msEccentricity = lighting.msEccentricity,
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
		.historyValid = volMainHistoryValid ? 1u : 0u,
		.temporalAccumulationFactor = std::clamp(settings.temporalAccumulationFactor, 0.0f, 1.0f),
		.cloudHistoryInvalidation = cloudHistoryInvalidation,
		.shadowVolumeBottom = KilometersToGameUnits(lowCloudBaseKm),
		.shadowVolumeTop = KilometersToGameUnits(lowCloudTopKm),
		.cloudWindDelta = windDelta,
		.cloudShapeShear = windDir * KilometersToGameUnits(std::clamp(low.shapeShear, 0.f, 2.f)),
		.lowHistoryConfidence = lowHistoryConfidence,
		.highHistoryConfidence = highHistoryConfidence,
		.highViewSteps = std::clamp(high.viewSteps, 8u, 256u),
		.ndfAccelerationValid = ndfManager.accelerationValid ? 1u : 0u,
		.crossLayerShadows = lighting.crossLayerShadows ? 1u : 0u,
		.lightCacheSteps = std::clamp(lighting.cacheSteps, 4u, 32u),
		.msHeightPower = std::clamp(lighting.msHeightPower, 0.f, 2.f),
		.lightCacheOrigin = volLightCacheOrigin,
		.lightCacheWindDelta = noiseWindOffset - volLightCacheWind,
		.lightCacheRange = KilometersToGameUnits(256.f),
		.lightCacheUpdatePhase = rebuildLightCache ? 8u : (volFrameIndex & 7u),
		.previousViewProj = volHistoryViewProj,
		.previousCamera = volHistoryCamera,
	};
	volCloudSb->Update(&sbData, sizeof(sbData));

	// Shared SRVs for both passes
	auto ndfTextures = ndfManager.GetNdf(settings.cloudMap, ndfTexManager);
	auto highTextures = highCloudMapManager.GetTextures(high);
	if (!ndfTextures || (high.enabled && (!highTextures.highWeather || !highTextures.highCell || !highTextures.highWarp || !highTextures.highWisp)))
		return;

	std::array<ID3D11ShaderResourceView*, 19> srvs = {
		volCloudSb->SRV(),                                                                                             // t0
		texTrLut->srv.get(),                                                                                           // t1
		texMsLut->srv.get(),                                                                                           // t2
		texApLut->srv.get(),                                                                                           // t3
		renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV,  // t4
		baseShapeNoiseSrv.get(),                                                                                       // t5 authored Nubis RGBA noise composite
		texApSunLut->srv.get(),                                                                                        // t6 direct solar single-scattering AP LUT
		ndfTextures.height,                                                                                            // t7 NDF height
		ndfTextures.modeling,                                                                                          // t8 NDF modeling
		texApShadow ? texApShadow->srv.get() : nullptr,                                                                // t9
		texSvLut->srv.get(),                                                                                           // t10
		highTextures.highWeather,                                                                                      // t11
		ndfManager.accelerationValid ? ndfManager.texDistance->srv.get() : nullptr,                                    // t12
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
			// renderShadowVolume packs four full-depth columns into each group.
			constexpr uint32_t columnsPerGroup = kShadowVolW / kShadowVolD;
			static_assert(kShadowVolW % kShadowVolD == 0);
			dispatch_size[0] = kShadowVolW / columnsPerGroup;
			dispatch_size[1] = kShadowVolH;
		}

		globals::profiler->BeginPass("PhysicalSky::VolumetricShadowVolume");
		context->Dispatch(dispatch_size[0], dispatch_size[1], 1);
		globals::profiler->EndPass();
		state->EndPerfEvent();
	}

	if (a_pass == VolumetricCloudPass::kMainViewAndCubemap) {
		ID3D11UnorderedAccessView* nullCacheUav = nullptr;
		ID3D11ShaderResourceView* nullCacheSrvs[2] = {};
		context->CSSetUnorderedAccessViews(0, 1, &nullCacheUav, nullptr);
		context->CSSetShaderResources(24, 2, nullCacheSrvs);
		{
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			globals::profiler->BeginPass("PhysicalSky::CloudLightCache");
			auto* cacheUav = texLowCloudLightCache->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &cacheUav, nullptr);
			context->CSSetShader(csVolLowLightCache.get(), nullptr, 0);
			context->Dispatch(16, 16, rebuildLightCache ? 4u : 1u);
			if (high.enabled) {
				cacheUav = texHighCloudLightCache->uav.get();
				context->CSSetUnorderedAccessViews(0, 1, &cacheUav, nullptr);
				context->CSSetShader(csVolHighLightCache.get(), nullptr, 0);
				context->Dispatch(16, 16, rebuildLightCache ? 4u : 1u);
			}
			globals::profiler->EndPass();
		}
		context->CSSetUnorderedAccessViews(0, 1, &nullCacheUav, nullptr);
		ID3D11ShaderResourceView* cacheSrvs[2] = {
			texLowCloudLightCache->srv.get(),
			high.enabled ? texHighCloudLightCache->srv.get() : nullptr
		};
		context->CSSetShaderResources(24, 2, cacheSrvs);
		ID3D11UnorderedAccessView* nullUavs[3] = {};
		ID3D11ShaderResourceView* nullTemporalSrvs[12] = {};
		ID3D11ShaderResourceView* nullOutputs[2] = {};
		context->PSSetShaderResources(110, 2, nullOutputs);
		context->PSSetShaderResources(114, 2, nullOutputs);
		texVolTr.swap(texVolHistoryTr);
		texVolLum.swap(texVolHistoryLum);
		texVolAux.swap(texVolHistoryAux);
		texVolCubeTr.swap(texVolCubeHistoryTr);
		texVolCubeLum.swap(texVolCubeHistoryLum);
		texVolCubeAux.swap(texVolCubeHistoryAux);
		outputSrvs = { texVolTr->srv.get(), texVolLum->srv.get(), texShadowVolume->srv.get() };
		shadowSrvs[3] = texShadowVolume->srv.get();
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(16, 1, &ambientShSrv);
		context->CSSetShaderResources(20, (uint)shadowSrvs.size(), shadowSrvs.data());
		std::array<ID3D11UnorderedAccessView*, 3> uavs = { texVolLowTr->uav.get(), texVolLowLum->uav.get(), texVolLowAux->uav.get() };
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csVolMainView.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricMainView");
		context->Dispatch((lowW + 7u) >> 3, (lowH + 7u) >> 3, 1);
		globals::profiler->EndPass();
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);

		std::array<ID3D11ShaderResourceView*, 6> temporalSrvs = {
			texVolHistoryTr->srv.get(), texVolHistoryLum->srv.get(), texVolHistoryAux->srv.get(),
			texVolLowTr->srv.get(), texVolLowLum->srv.get(), texVolLowAux->srv.get()
		};
		context->CSSetShaderResources(26, (uint)temporalSrvs.size(), temporalSrvs.data());
		uavs = { texVolTr->uav.get(), texVolLum->uav.get(), texVolAux->uav.get() };
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csVolReproject.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricReproject");
		context->Dispatch((renderW + 7u) >> 3, (renderH + 7u) >> 3, 1);
		globals::profiler->EndPass();
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetShaderResources(26, 12, nullTemporalSrvs);

		uavs = { texVolCubeTraceTr->uav.get(), texVolCubeTraceLum->uav.get(), texVolCubeTraceAux->uav.get() };
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csVolCubemap.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricCubemap");
		context->Dispatch((kVolCubeSize / 4u + 7u) >> 3, (kVolCubeSize / 4u + 7u) >> 3, 6);
		globals::profiler->EndPass();
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		temporalSrvs = {
			texVolCubeHistoryTr->srv.get(), texVolCubeHistoryLum->srv.get(), texVolCubeHistoryAux->srv.get(),
			texVolCubeTraceTr->srv.get(), texVolCubeTraceLum->srv.get(), texVolCubeTraceAux->srv.get()
		};
		context->CSSetShaderResources(32, (uint)temporalSrvs.size(), temporalSrvs.data());
		uavs = { texVolCubeTr->uav.get(), texVolCubeLum->uav.get(), texVolCubeAux->uav.get() };
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csVolCubeReproject.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricCubeReproject");
		context->Dispatch((kVolCubeSize + 7u) >> 3, (kVolCubeSize + 7u) >> 3, 6);
		globals::profiler->EndPass();
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetShaderResources(26, 12, nullTemporalSrvs);
		volMainHistoryValid = true;
		volHistorySunDir = cloudLightDir;
		volHistoryWindOffset = noiseWindOffset;
		volHistoryTime = timeSeconds;
		volHistoryViewProj = globals::game::frameBufferCached.GetCameraViewProj();
		volHistoryCamera = { cameraPosition.x, cameraPosition.y, cameraPosition.z };
		volLightCacheValid = true;
		++volFrameIndex;
	}

	// Cleanup
	{
		ID3D11ShaderResourceView* nullSrvs[19] = {};
		ID3D11ShaderResourceView* nullShadowSrvs[4] = {};
		ID3D11ShaderResourceView* nullHistorySrvs[14] = {};
		ID3D11UnorderedAccessView* nullUavs[3] = {};
		ID3D11Buffer* nullCb[1] = {};
		ID3D11SamplerState* nullSamplers[3] = {};
		context->CSSetShaderResources(0, 19, nullSrvs);
		context->CSSetShaderResources(20, 4, nullShadowSrvs);
		context->CSSetShaderResources(24, 14, nullHistorySrvs);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetConstantBuffers(1, 1, nullCb);
		context->CSSetSamplers(2, 3, nullSamplers);
		context->CSSetShader(nullptr, nullptr, 0);

		// Make results available to any later pixel-shader passes that sample PhysicalSky/Common.hlsli.
		context->PSSetShaderResources(110, (uint)outputSrvs.size(), outputSrvs.data());
	}
}
