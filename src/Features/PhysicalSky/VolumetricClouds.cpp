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
#include <imgui.h>
#include <vector>

namespace
{
	constexpr float KilometersToGameUnits(float value) { return value / Util::Units::GAME_UNIT_TO_KM; }

	float2 CloudWindDirection(const LowCloudSettings& settings)
	{
		const float length = std::hypot(settings.windDirection.x, settings.windDirection.y);
		return std::isfinite(length) && length > 1e-4f ? settings.windDirection / length : float2{ 1.f, 0.f };
	}

	bool IsVolumeTexture(ID3D11ShaderResourceView* srv)
	{
		if (!srv)
			return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		srv->GetDesc(&desc);
		return desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D;
	}
}

float2 LowCloudSettings::GetNdfAltitudeRangeKm() const
{
	const float offset = std::isfinite(ndfAltitudeOffset) ? std::clamp(ndfAltitudeOffset, 0.f, 20000.f) : 256.f;
	const float scale = std::isfinite(ndfAltitudeScale) ? std::clamp(ndfAltitudeScale, 1.f, 20000.f) : 1792.f;
	return { offset * 0.001f, (offset + scale) * 0.001f };
}

float CirrusSettings::GetAltitudeKm() const
{
	return (std::isfinite(altitude) ? std::clamp(altitude, 1.f, 24000.f) : 2048.f) * 0.001f;
}

void PhysicalSky::LoadCloudTextures()
{
	volMainHistoryValid = false;
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	auto loadDDS = [&](const wchar_t* path, winrt::com_ptr<ID3D11ShaderResourceView>& srv) {
		srv = nullptr;
		HRESULT hr = DirectX::CreateDDSTextureFromFile(device, context, path, nullptr, srv.put());
		if (FAILED(hr))
			logger::warn("Failed to load DDS texture: {}", std::filesystem::path(path).string());
	};

	loadDDS(L"Data\\Textures\\PhysicalSky\\NubisCloudShapeNoise.dds", baseShapeNoiseSrv);
	if (baseShapeNoiseSrv && !IsVolumeTexture(baseShapeNoiseSrv.get())) {
		logger::warn("Ignoring Nubis noise composite because it is not a 3D texture.");
		baseShapeNoiseSrv = nullptr;
	}

	loadDDS(L"Data\\Textures\\PhysicalSky\\NubisVerticalProfile.dds", cloudProfileLutSrv);
	loadDDS(L"Data\\Textures\\PhysicalSky\\NubisVerticalAdjustment.dds", cloudAdjustmentLutSrv);
}

void PhysicalSky::SetupVolumetricResources()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	logger::debug("Setting up volumetric cloud resources...");

	debugCubeFaceSrvs.clear();

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
	texVolFilteredTr = createCloudTexture(mainDesc.Width, mainDesc.Height, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::CloudFilteredTr");
	texVolFilteredLum = createCloudTexture(mainDesc.Width, mainDesc.Height, historyRadianceFormat, "PhysicalSky::CloudFilteredLum");
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
	cirrusMapManager.SetupResources();

	CompileVolumetricShaders();
}

void PhysicalSky::CompileVolumetricShaders()
{
	volMainHistoryValid = false;
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
		ShaderInfo{ &csVolFilter, "Volumetrics.cs.hlsl", {}, "filterCloud" },
		ShaderInfo{ &csVolReproject, "Volumetrics.cs.hlsl", {}, "reproject" },
		ShaderInfo{ &csVolCubeReproject, "Volumetrics.cs.hlsl", {}, "reprojectCubemap" },
		ShaderInfo{ &csVolShadowVolume, "Volumetrics.cs.hlsl", {}, "renderShadowVolume" },
		ShaderInfo{ &csVolCubemap, "Volumetrics.cs.hlsl", {}, "renderCubemap" }
	};

	for (auto& info : shaderInfos) {
		*info.csPtr = nullptr;
		auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.data())))
			info.csPtr->attach(rawPtr);
	}
	ndfManager.CompileShaders();
	cirrusMapManager.CompileShaders();
}

void PhysicalSky::UpdateCloudWind()
{
	auto* ui = globals::game::ui;
	if (!ui || ui->GameIsPaused())
		return;
	const double elapsed = RE::GetSecondsSinceLastFrame();
	if (!std::isfinite(elapsed) || elapsed <= 0.0)
		return;

	const auto& low = settings.cloudLayer.low;
	const float2 direction = CloudWindDirection(low);
	const double speed = std::isfinite(low.windSpeed) ? std::clamp(low.windSpeed, 0.f, 80.f) : 0.0;
	volWindOffsetMeters[0] += direction.x * speed * elapsed;
	volWindOffsetMeters[1] += direction.y * speed * elapsed;
	volWindTime += elapsed;
}

void PhysicalSky::RenderVolumetricClouds(VolumetricCloudPass a_pass)
{
	if (!csVolMainView || !csVolReproject || !csVolCubeReproject || !csVolShadowVolume || !csVolCubemap || !csVolAmbientSH)
		return;
	if (!baseShapeNoiseSrv || !cloudProfileLutSrv || !cloudAdjustmentLutSrv)
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
	const auto& frameBuffer = globals::game::frameBufferCached;
	const auto& resolutionScale = frameBuffer.GetDynamicResolutionParams1();
	const float2 textureDim = { static_cast<float>(texVolTr->desc.Width), static_cast<float>(texVolTr->desc.Height) };
	const float2 frameDim = {
		std::clamp(textureDim.x * resolutionScale.x, 1.0f, textureDim.x),
		std::clamp(textureDim.y * resolutionScale.y, 1.0f, textureDim.y)
	};
	const uint32_t renderW = static_cast<uint32_t>(frameDim.x);
	const uint32_t renderH = static_cast<uint32_t>(frameDim.y);
	const float sunHistoryDot = std::clamp(
		volHistorySunDir.x * cloudLightDir.x + volHistorySunDir.y * cloudLightDir.y + volHistorySunDir.z * cloudLightDir.z,
		-1.0f,
		1.0f);
	const float sunAngleDifferenceDegrees = std::acos(sunHistoryDot) * (180.0f / std::numbers::pi_v<float>);
	const float cloudHistoryInvalidation = std::clamp(1.0f - sunAngleDifferenceDegrees / 10.0f, 0.0f, 1.0f);

	auto& low = settings.cloudLayer.low;
	auto& cirrus = settings.cloudLayer.cirrus;
	auto& lighting = settings.cloudLayer.lighting;

	low.ndfScale.x = std::clamp(low.ndfScale.x, 1.0f, 50.0f);
	low.ndfScale.y = std::clamp(low.ndfScale.y, 1.0f, 50.0f);

	const float2 windDir = CloudWindDirection(low);
	const float2 noiseWindOffset = {
		static_cast<float>(volWindOffsetMeters[0] / Util::Units::GAME_UNIT_TO_M),
		static_cast<float>(volWindOffsetMeters[1] / Util::Units::GAME_UNIT_TO_M)
	};
	const double elapsed = volWindTime - volHistoryTime;
	if (elapsed < 0.0 || elapsed > 0.25) {
		volMainHistoryValid = false;
	}
	const float2 windDelta = volMainHistoryValid ? float2{
		static_cast<float>((volWindOffsetMeters[0] - volHistoryWindOffsetMeters[0]) / Util::Units::GAME_UNIT_TO_M),
		static_cast<float>((volWindOffsetMeters[1] - volHistoryWindOffsetMeters[1]) / Util::Units::GAME_UNIT_TO_M)
	} :
	                                               float2{ 0.f, 0.f };
	const float2 lowAltitudeRange = low.GetNdfAltitudeRangeKm();
	const float lowCloudBaseKm = lowAltitudeRange.x;
	const float lowCloudTopKm = lowAltitudeRange.y;
	const float lowCloudTraceTopKm = lowCloudTopKm;
	const float traceBottomKm = cirrus.enabled ? std::min(lowCloudBaseKm, cirrus.GetAltitudeKm()) : lowCloudBaseKm;
	const float traceTopKm = cirrus.enabled ? std::max(lowCloudTraceTopKm, cirrus.GetAltitudeKm()) : lowCloudTraceTopKm;

	const uint32_t lowW = (renderW + 3u) / 4u;
	const uint32_t lowH = (renderH + 3u) / 4u;

	const auto cameraPosition = globals::game::frameBufferCached.GetCameraPosAdjust();
	const auto cirrusTextures = cirrusMapManager.GetTextures();
	const bool renderCirrus = cirrus.enabled && bool(cirrusTextures);

	// Update StructuredBuffer
	VolumetricCloudSB sbData = {
		.rayMarchRange = KilometersToGameUnits(settings.rayMarchRange),
		.shadowVolumeRange = KilometersToGameUnits(settings.shadowVolumeRange),
		.marchStepScale = std::clamp(settings.marchStepScale, 0.0625f, 4.f),
		.cloudFrameIndex = volFrameIndex,
		.rcpFrameDim = float2(1.0f) / textureDim,
		.dirlightDir = cloudLightDir,
		.bottomZ = cbData.zBottom,
		.planetRadius = cbData.rPlanet,
		.activeFrameDim = { static_cast<float>(renderW), static_cast<float>(renderH) },
		.lowestCloudAltitude = KilometersToGameUnits(traceBottomKm),
		.highestCloudAltitude = KilometersToGameUnits(traceTopKm),
		.lowCloudBaseAltitude = KilometersToGameUnits(lowCloudBaseKm),
		.lowCloudTopAltitude = KilometersToGameUnits(lowCloudTopKm),
		.lowCloudTraceTopAltitude = KilometersToGameUnits(lowCloudTraceTopKm),
		.lowNdfFrequency = { Util::Units::GAME_UNIT_TO_KM / low.ndfScale.x, Util::Units::GAME_UNIT_TO_KM / low.ndfScale.y },
		.noiseWindOffset = noiseWindOffset,
		.lowDensityScale = std::max(low.densityScale, 0.f),
		.coverageBottomPower = std::clamp(low.coverageBottomPower, 0.01f, 4.f),
		.coverageHeightRange = std::clamp(low.coverageHeightRange, 0.001f, 1.f),
		.bottomDensityPower = std::clamp(low.bottomDensityPower, 0.f, 10.f),
		.bottomDensityWidth = std::clamp(low.bottomDensityWidth, 1.f, 10.f),
		.topExpansion = std::clamp(low.topExpansion, 0.f, 1.f),
		.cirrusEnabled = renderCirrus ? 1u : 0u,
		.cirrusAltitude = KilometersToGameUnits(cirrus.GetAltitudeKm()),
		.cirrusPatternFrequency = Util::Units::GAME_UNIT_TO_M / std::clamp(cirrus.patternScale, 100.f, 64000.f),
		.cirrusDensityScale = std::max(cirrus.densityScale, 0.f),
		.cirrusLightingScale = std::max(cirrus.lightingScale, 0.f),
		.lightingScale = std::max(lighting.lightingScale, 0.f),
		.sunExtinction = std::max(lighting.sunExtinction, 0.f),
		.phaseForwardG = std::clamp(lighting.phaseForwardG, 0.f, 0.95f),
		.phaseBackwardG = std::clamp(lighting.phaseBackwardG, -0.95f, 0.95f),
		.phaseForwardWeight = std::max(lighting.phaseForwardWeight, 0.f),
		.phaseBackwardWeight = std::max(lighting.phaseBackwardWeight, 0.f),
		.scatterVolumeStrength = std::max(lighting.scatterVolumeStrength, 0.f),
		.scatterVolumeDepth = std::clamp(lighting.scatterVolumeDepth, 0.001f, 1.f),
		.scatterVolumeHeight = std::clamp(lighting.scatterVolumeHeight, 0.f, 4.f),
		.softScatteringStrength = std::max(lighting.softScatteringStrength, 0.f),
		.powderStrength = std::clamp(lighting.powderStrength, 0.f, 1.f),
		.ambientStrength = std::max(lighting.ambientStrength, 0.f),
		.ambientFloor = std::clamp(lighting.ambientFloor, 0.f, 1.f),
		.ambientDensity = std::clamp(lighting.ambientDensity, 0.f, 1.f),
		.ambientBase = std::clamp(lighting.ambientBase, 0.f, 1.f),
		.lowFrameDim = { static_cast<float>(lowW), static_cast<float>(lowH) },
		.historyValid = volMainHistoryValid ? 1u : 0u,
		.temporalAccumulationFactor = std::clamp(settings.temporalAccumulationFactor, 0.0f, 1.0f),
		.cloudHistoryInvalidation = cloudHistoryInvalidation,
		.shadowVolumeBottom = KilometersToGameUnits(lowCloudBaseKm),
		.shadowVolumeTop = KilometersToGameUnits(lowCloudTopKm),
		.cloudWindDelta = windDelta,
		.cloudShapeShear = windDir * KilometersToGameUnits(std::clamp(low.shapeShear, 0.f, 2.f)),
		.ndfAccelerationValid = ndfManager.accelerationValid ? 1u : 0u,
		.previousViewProj = volHistoryViewProj,
		.previousCamera = volHistoryCamera,
		.previousFrameDim = volHistoryFrameDim,
	};
	volCloudSb->Update(&sbData, sizeof(sbData));

	// Shared SRVs for both passes
	auto ndfTextures = ndfManager.GetNdf(settings.cloudMap, ndfTexManager);
	if (!ndfTextures)
		return;

	std::array<ID3D11ShaderResourceView*, 19> srvs = {
		volCloudSb->SRV(),                                                                                             // t0
		texTrLut->srv.get(),                                                                                           // t1
		texMsLut->srv.get(),                                                                                           // t2
		texApLut->srv.get(),                                                                                           // t3
		renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV,  // t4
		baseShapeNoiseSrv.get(),                                                                                       // t5 cloud shape noise
		texApSunLut->srv.get(),                                                                                        // t6 direct solar single-scattering AP LUT
		ndfTextures.height,                                                                                            // t7 NDF height
		ndfTextures.modeling,                                                                                          // t8 NDF modeling
		texApShadow ? texApShadow->srv.get() : nullptr,                                                                // t9
		texSvLut->srv.get(),                                                                                           // t10
		renderCirrus ? cirrusTextures.weather : nullptr,                                                               // t11
		ndfManager.accelerationValid ? ndfManager.texDistance->srv.get() : nullptr,                                    // t12
		renderCirrus ? cirrusTextures.patterns : nullptr,                                                              // t13
		nullptr,                                                                                                       // t14
		nullptr,                                                                                                       // t15
		nullptr,                                                                                                       // t16 ambient SH (bound per pass)
		cloudProfileLutSrv.get(),                                                                                      // t17 vertical profile
		cloudAdjustmentLutSrv.get(),                                                                                   // t18 profile expansion and noise warp
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

		std::array<ID3D11ShaderResourceView*, 3> historySrvs = {
			texVolHistoryTr->srv.get(), texVolHistoryLum->srv.get(), texVolHistoryAux->srv.get()
		};
		std::array<ID3D11ShaderResourceView*, 3> traceSrvs = {
			texVolLowTr->srv.get(), texVolLowLum->srv.get(), texVolLowAux->srv.get()
		};
		context->CSSetShaderResources(26, (uint)historySrvs.size(), historySrvs.data());
		context->CSSetShaderResources(29, (uint)traceSrvs.size(), traceSrvs.data());
		uavs = { texVolTr->uav.get(), texVolLum->uav.get(), texVolAux->uav.get() };
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csVolReproject.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricReproject");
		context->Dispatch((renderW + 7u) >> 3, (renderH + 7u) >> 3, 1);
		globals::profiler->EndPass();
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetShaderResources(26, (uint)historySrvs.size(), nullTemporalSrvs);
		context->CSSetShaderResources(29, (uint)traceSrvs.size(), nullTemporalSrvs);

		std::array<ID3D11ShaderResourceView*, 3> resultSrvs = {
			texVolTr->srv.get(), texVolLum->srv.get(), texVolAux->srv.get()
		};
		context->CSSetShaderResources(38, (uint)resultSrvs.size(), resultSrvs.data());
		std::array<ID3D11UnorderedAccessView*, 2> filteredUavs = {
			texVolFilteredTr->uav.get(), texVolFilteredLum->uav.get()
		};
		context->CSSetUnorderedAccessViews(3, (uint)filteredUavs.size(), filteredUavs.data(), nullptr);
		context->CSSetShader(csVolFilter.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricFilter");
		context->Dispatch((renderW + 7u) >> 3, (renderH + 7u) >> 3, 1);
		globals::profiler->EndPass();
		context->CSSetUnorderedAccessViews(3, 2, nullUavs, nullptr);
		context->CSSetShaderResources(38, (uint)resultSrvs.size(), nullTemporalSrvs);
		outputSrvs[0] = texVolFilteredTr->srv.get();
		outputSrvs[1] = texVolFilteredLum->srv.get();

		uavs = { texVolCubeTraceTr->uav.get(), texVolCubeTraceLum->uav.get(), texVolCubeTraceAux->uav.get() };
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csVolCubemap.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricCubemap");
		context->Dispatch((kVolCubeSize / 4u + 7u) >> 3, (kVolCubeSize / 4u + 7u) >> 3, 6);
		globals::profiler->EndPass();
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		std::array<ID3D11ShaderResourceView*, 6> cubeSrvs = {
			texVolCubeHistoryTr->srv.get(), texVolCubeHistoryLum->srv.get(), texVolCubeHistoryAux->srv.get(),
			texVolCubeTraceTr->srv.get(), texVolCubeTraceLum->srv.get(), texVolCubeTraceAux->srv.get()
		};
		context->CSSetShaderResources(32, (uint)cubeSrvs.size(), cubeSrvs.data());
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
		volHistoryWindOffsetMeters = volWindOffsetMeters;
		volHistoryTime = volWindTime;
		volHistoryViewProj = globals::game::frameBufferCached.GetCameraViewProj();
		volHistoryCamera = { cameraPosition.x, cameraPosition.y, cameraPosition.z };
		volHistoryFrameDim = frameDim;
		++volFrameIndex;
	}

	// Cleanup
	{
		ID3D11ShaderResourceView* nullSrvs[19] = {};
		ID3D11ShaderResourceView* nullShadowSrvs[4] = {};
		ID3D11ShaderResourceView* nullHistorySrvs[12] = {};
		ID3D11ShaderResourceView* nullFilteredSrvs[3] = {};
		ID3D11UnorderedAccessView* nullUavs[3] = {};
		ID3D11Buffer* nullCb[1] = {};
		ID3D11SamplerState* nullSamplers[3] = {};
		context->CSSetShaderResources(0, 19, nullSrvs);
		context->CSSetShaderResources(20, 4, nullShadowSrvs);
		context->CSSetShaderResources(26, 12, nullHistorySrvs);
		context->CSSetShaderResources(38, 3, nullFilteredSrvs);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetUnorderedAccessViews(3, 3, nullUavs, nullptr);
		context->CSSetConstantBuffers(1, 1, nullCb);
		context->CSSetSamplers(2, 3, nullSamplers);
		context->CSSetShader(nullptr, nullptr, 0);

		// Make results available to any later pixel-shader passes that sample PhysicalSky/Common.hlsli.
		context->PSSetShaderResources(110, (uint)outputSrvs.size(), outputSrvs.data());
	}
}

ID3D11ShaderResourceView* PhysicalSky::GetDebugCubeFaceSrv(Texture2D* a_texture)
{
	if (!a_texture || !a_texture->resource)
		return nullptr;

	ID3D11Resource* resource = a_texture->resource.get();
	if (auto it = debugCubeFaceSrvs.find(resource); it != debugCubeFaceSrvs.end())
		return it->second.get();
	if (debugCubeFace < 0 || static_cast<uint32_t>(debugCubeFace) >= a_texture->desc.ArraySize)
		return nullptr;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = a_texture->desc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
		.Texture2DArray = { .MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = static_cast<UINT>(debugCubeFace), .ArraySize = 1 }
	};

	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	if (FAILED(globals::d3d::device->CreateShaderResourceView(resource, &srvDesc, srv.put())))
		return nullptr;

	return debugCubeFaceSrvs.emplace(resource, std::move(srv)).first->second.get();
}

void PhysicalSky::DrawDebugCube(Texture2D* a_texture, const char* a_label, float a_scale)
{
	if (!a_texture || !a_texture->srv)
		return;

	if (auto srv = GetDebugCubeFaceSrv(a_texture)) {
		ImGui::BulletText("%s", a_label);
		ImGui::Image(srv, { a_texture->desc.Width * a_scale, a_texture->desc.Height * a_scale });
	}
}

ID3D11ShaderResourceView* PhysicalSky::GetDebugVolumeSliceSrv(ID3D11ShaderResourceView* a_srv, eastl::unique_ptr<Texture2D>& a_target)
{
	if (!a_srv)
		return nullptr;

	winrt::com_ptr<ID3D11Resource> resource;
	a_srv->GetResource(resource.put());
	winrt::com_ptr<ID3D11Texture3D> texture;
	if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
		return nullptr;

	D3D11_TEXTURE3D_DESC desc;
	texture->GetDesc(&desc);

	if (!a_target || a_target->desc.Width != desc.Width || a_target->desc.Height != desc.Height || a_target->desc.Format != desc.Format) {
		D3D11_TEXTURE2D_DESC sliceDesc = {
			.Width = desc.Width,
			.Height = desc.Height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = desc.Format,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = sliceDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		a_target = eastl::make_unique<Texture2D>(sliceDesc, "PhysicalSky::DebugVolumeSlice");
		a_target->CreateSRV(srvDesc);
	}

	// A 3D subresource covers every depth slice, so the slice is selected by the box.
	const D3D11_BOX box = { 0, 0, 0, desc.Width, desc.Height, 1 };
	globals::d3d::context->CopySubresourceRegion(a_target->resource.get(), 0, 0, 0, 0, texture.get(), 0, &box);

	return a_target->srv.get();
}

void PhysicalSky::DrawDebugVolume(ID3D11ShaderResourceView* a_srv, const char* a_label, eastl::unique_ptr<Texture2D>& a_target, float a_scale)
{
	if (!a_srv)
		return;

	if (auto srv = GetDebugVolumeSliceSrv(a_srv, a_target)) {
		ImGui::BulletText("%s", a_label);
		ImGui::Image(srv, { a_target->desc.Width * a_scale, a_target->desc.Height * a_scale });
	}
}
