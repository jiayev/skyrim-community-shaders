#include "Raytracing.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/HairSpecular.h"
#include "Features/LinearLighting.h"
#include "Features/LODBlending.h"
#include "Features/PathTracing.h"
#include "Features/Skin.h"
#include "Features/Upscaling/DXVKInterop.h"
#include "Features/WetnessEffects.h"
#include "Globals.h"
#include "Menu.h"
#include "Menu/ThemeManager.h"
#include "Utils/D3D.h"
#include <thread>

#define I18N_KEY_PREFIX "feature.raytracing."

static std::string StableLabel(const char* label, std::string_view id)
{
	return std::format("{}###{}", label, id);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Raytracing::Settings,
	PerfOverlay,
	DisplaySceneGraphCounters,
	CreationEngineRaytracingSettings,
	RendererSettings)

void Raytracing::RestoreDefaultSettings()
{
	settings = {};
}

void Raytracing::LoadSettings(json& o_json)
{
	settings = o_json;
	UpdateSettings();
}

void Raytracing::SaveSettings(json& o_json)
{
	o_json = settings;
}

bool Raytracing::Available(bool a_initialized) const
{
	if (forcedDisabled || !loaded)
		return false;
	if (a_initialized && !initialized)
		return false;
	return settings.CreationEngineRaytracingSettings.Enabled;
}

CreationEngineRaytracing::Settings Raytracing::GetSettings() const
{
	auto certSettings = settings.CreationEngineRaytracingSettings;

	switch (settings.PerfOverlay) {
	case OverlayMode::Simple:
	case OverlayMode::Complete:
		certSettings.DebugSettings.Timings = CreationEngineRaytracing::TimingMode::Standard;
		break;
	case OverlayMode::Extended:
		certSettings.DebugSettings.Timings = CreationEngineRaytracing::TimingMode::Extended;
		break;
	default:
		certSettings.DebugSettings.Timings = CreationEngineRaytracing::TimingMode::Disabled;
		break;
	}

	if (IsPathTracing()) {
		const auto& pt = globals::features::pathTracing.settings;
		certSettings.GeneralSettings.Mode = CreationEngineRaytracing::Mode::PathTracing;
		certSettings.GeneralSettings.Denoiser = pt.GeneralSettings.Denoiser;
		certSettings.RaytracingSettings = pt.RaytracingSettings;
		certSettings.AdvancedSettings.StablePlanes = pt.StablePlanes;
		certSettings.NRDSettings = pt.NRDSettings;
		certSettings.NRDReblurSettings = pt.NRDReblurSettings;
		certSettings.NRDRelaxSettings = pt.NRDRelaxSettings;
		certSettings.AdvancedSettings.SSSSettings = pt.SSSSettings;
		certSettings.MaterialSettings = pt.MaterialSettings;
		certSettings.LightingSettings = pt.LightingSettings;
		certSettings.WaterSettings = pt.WaterSettings;
		certSettings.ExperimentalSettings.PathTracingCull = pt.ExperimentalSettings.PathTracingCull;
	} else {
		certSettings.GeneralSettings.Mode = CreationEngineRaytracing::Mode::None;
	}

	return certSettings;
}

CreationEngineRaytracing::Mode Raytracing::Mode() const
{
	if (!Available())
		return CreationEngineRaytracing::Mode::None;

	if (globals::features::pathTracing.loaded && globals::features::pathTracing.settings.Enabled)
		return CreationEngineRaytracing::Mode::PathTracing;

	return CreationEngineRaytracing::Mode::None;
}

bool Raytracing::IsPathTracing() const
{
	return Mode() == CreationEngineRaytracing::Mode::PathTracing;
}

bool Raytracing::IsPathTracingCull() const
{
	return Mode() == CreationEngineRaytracing::Mode::PathTracing 
		&& settings.CreationEngineRaytracingSettings.ExperimentalSettings.PathTracingCull != CreationEngineRaytracing::PTCullMode::Disabled;
}

void Raytracing::UpdateJitter(float2 a_jitter)
{
	if (!initialized)
		return;

	creationEngineRaytracing->UpdateJitter(a_jitter);
}

void Raytracing::GetRayReconstructionInputs(ID3D11Resource*& diffuseAlbedo, ID3D11Resource*& specularAlbedo,
	ID3D11Resource*& normalRoughness, ID3D11Resource*& specHitDist)
{
	diffuseAlbedo = nullptr;
	specularAlbedo = nullptr;
	normalRoughness = nullptr;
	specHitDist = nullptr;

	if (!creationEngineRaytracing || !creationEngineRaytracing->GetRRInput)
		return;

	const auto mode = Mode();
	if (mode != CreationEngineRaytracing::Mode::GlobalIllumination &&
		mode != CreationEngineRaytracing::Mode::PathTracing)
		return;

	void* diffuse = nullptr;
	void* specular = nullptr;
	void* hitDist = nullptr;
	creationEngineRaytracing->GetRRInput(diffuse, specular, hitDist);

	diffuseAlbedo = static_cast<ID3D11Resource*>(diffuse);
	specularAlbedo = static_cast<ID3D11Resource*>(specular);
	specHitDist = static_cast<ID3D11Resource*>(hitDist);
	normalRoughness = normalRoughnessTexture.get();
}

void Raytracing::UpdateSettings()
{
	if (!initialized)
		return;

	creationEngineRaytracing->UpdateSettings(GetSettings());
}

void Raytracing::Execute()
{
	if (!Available() || Mode() == CreationEngineRaytracing::Mode::None)
		return;

	if (auto* dxvk = DXVKInterop::GetSingleton()) {
		if (auto* interopDevice = dxvk->GetInteropDevice()) {
			interopDevice->FlushRenderingCommands();
		}
	}

	creationEngineRaytracing->Execute();
	const uint32_t completedSlot = creationEngineRaytracing->PostExecution();

	if (settings.PerfOverlay != OverlayMode::None && creationEngineRaytracing->GetPassTimings) {
		creationEngineRaytracing->GetPassTimings(passTimings);
	}

	if (completedSlot >= CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT)
		return;

	auto* renderer = globals::game::renderer;
	if (!renderer)
		return;

	auto* context = globals::d3d::context;
	if (!context)
		return;

	const auto& renderTargets = renderer->GetRuntimeData().renderTargets;
	auto& main = renderTargets[RE::RENDER_TARGETS::kMAIN];

	if (IsPathTracing()) {
		float2 screenSize{ static_cast<float>(globals::game::graphicsState->screenWidth), static_cast<float>(globals::game::graphicsState->screenHeight) };
		auto dynamicScreenSize = Util::ConvertToDynamic(screenSize);

		if (screenCB && screenData) {
			screenData->Resolution = { static_cast<uint32_t>(screenSize.x), static_cast<uint32_t>(screenSize.y) };
			screenData->DynamicResolution = { static_cast<uint32_t>(dynamicScreenSize.x), static_cast<uint32_t>(dynamicScreenSize.y) };
			screenCB->Update(screenData.get(), sizeof(ScreenData));
		}

		// Blend pathtracing and sky (colors and motion vectors)
		if (ptCompositeCS && screenCB && sharedMainTextures[completedSlot].srv && sharedMotionVectorTextures[completedSlot].srv) {
			auto& mv = renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

			context->CSSetShader(ptCompositeCS.get(), nullptr, 0);

			ID3D11Buffer* cb = screenCB->CB();
			context->CSSetConstantBuffers(0, 1, &cb);

			ID3D11ShaderResourceView* srvs[] = {
				sharedMainTextures[completedSlot].srv.get(),
				sharedMotionVectorTextures[completedSlot].srv.get()
			};
			context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

			ID3D11UnorderedAccessView* uavs[] = {
				main.UAV,
				mv.UAV
			};
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			auto dispatchCount = Util::GetScreenDispatchCount(true);
			context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

			uavs[0] = nullptr;
			uavs[1] = nullptr;
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		} else if (sharedMainTextures[completedSlot].texture.shared && main.texture) {
			context->CopyResource(main.texture, sharedMainTextures[completedSlot].texture.shared);
		}

		// Copy Depth buffer
		if (copyDepthVS && copyDepthPS && sharedDepthTextures[completedSlot].srv) {
			auto depthStencils = renderer->GetDepthStencilData().depthStencils;

			auto& mainDepth = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto& mainDepthCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
			auto& zPrePassCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

			context->ClearDepthStencilView(mainDepth.views[0], D3D11_CLEAR_DEPTH, 1.0f, 0u);
			context->ClearDepthStencilView(mainDepthCopy.views[0], D3D11_CLEAR_DEPTH, 1.0f, 0u);
			context->ClearDepthStencilView(zPrePassCopy.views[0], D3D11_CLEAR_DEPTH, 1.0f, 0u);

			ID3D11DepthStencilState* oldDSS = nullptr;
			UINT oldRef = 0;
			context->OMGetDepthStencilState(&oldDSS, &oldRef);

			ID3D11RenderTargetView* oldRTV = nullptr;
			ID3D11DepthStencilView* oldDSV = nullptr;
			context->OMGetRenderTargets(1, &oldRTV, &oldDSV);

			UINT numViewports = 1;
			D3D11_VIEWPORT oldViewport = {};
			context->RSGetViewports(&numViewports, &oldViewport);

			D3D11_VIEWPORT viewport = {};
			viewport.TopLeftX = 0.0f;
			viewport.TopLeftY = 0.0f;
			viewport.Width = screenSize.x;
			viewport.Height = screenSize.y;
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			context->RSSetViewports(1, &viewport);

			context->IASetInputLayout(nullptr);
			context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
			context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			context->OMSetRenderTargets(0, nullptr, mainDepth.views[0]);
			context->OMSetDepthStencilState(depthStencilState.get(), 0);

			context->RSSetState(copyRasterizerState.get());
			context->OMSetBlendState(copyBlendState.get(), nullptr, 0xffffffff);

			context->VSSetShader(copyDepthVS.get(), nullptr, 0);
			context->PSSetShader(copyDepthPS.get(), nullptr, 0);

			ID3D11ShaderResourceView* srvs[] = { sharedDepthTextures[completedSlot].srv.get() };
			context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

			context->Draw(3, 0);

			context->OMSetDepthStencilState(oldDSS, oldRef);
			context->OMSetRenderTargets(1, &oldRTV, oldDSV);
			context->RSSetViewports(1, &oldViewport);

			if (oldDSS) {
				oldDSS->Release();
				oldDSS = nullptr;
			}
			if (oldRTV) {
				oldRTV->Release();
				oldRTV = nullptr;
			}
			if (oldDSV) {
				oldDSV->Release();
				oldDSV = nullptr;
			}

			context->PSSetShader(nullptr, nullptr, 0);
			context->VSSetShader(nullptr, nullptr, 0);

			context->CopyResource(mainDepthCopy.texture, mainDepth.texture);
			context->CopyResource(zPrePassCopy.texture, mainDepth.texture);
		}

		// Clear Specular render target
		{
			float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			context->ClearRenderTargetView(renderTargets[RE::RENDER_TARGETS::kINDIRECT_DOWNSCALED].RTV, clearColor);
		}
	} else if (sharedMainTextures[completedSlot].texture.shared && main.texture) {
		context->CopyResource(main.texture, sharedMainTextures[completedSlot].texture.shared);
	}
}

void Raytracing::Load()
{
	if (forcedDisabled)
		return;

	Hooks::Install();
}

void Raytracing::PostPostLoad()
{
	creationEngineRaytracing = eastl::make_unique<CreationEngineRaytracing>();

	if (!creationEngineRaytracing->handle) {
		settings.CreationEngineRaytracingSettings.Enabled = false;
		forcedDisabled = true;
		disableReason = DisableReason::MissingPlugin;
		logger::warn("[Raytracing] 'CreationEngineRaytracing.dll' not found, feature disabled.");
		return;
	}

	RE::GetINISetting("bReflectLODLand:Water")->data.b = false;
	RE::GetINISetting("bReflectLODObjects:Water")->data.b = false;
	RE::GetINISetting("bReflectLODTrees:Water")->data.b = false;
	RE::GetINISetting("bReflectSky:Water")->data.b = true;

	logger::info("[Raytracing] Loaded 'CreationEngineRaytracing.dll' module successfully.");
}

void Raytracing::DataLoaded()
{
	if (forcedDisabled)
		return;

	BGSActorCellEventHandler::Register();
}

bool Raytracing::InitializeCERaytracing()
{
	if (forcedDisabled || initialized)
		return false;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (dxvk && (dxvk->IsAvailable() || dxvk->Initialize())) {
		VkQueue graphicsQueue = dxvk->GetQueue();
		uint32_t queueIndex = 0;
		uint32_t queueFamilyIndex = dxvk->GetQueueFamilyIndex();
		dxvk->GetSubmissionQueue1(&graphicsQueue, &queueIndex, &queueFamilyIndex);

		bool result = creationEngineRaytracing->InitializeVulkanRenderer(
			&settings.RendererSettings,
			dxvk->GetInstance(),
			dxvk->GetPhysicalDevice(),
			dxvk->GetDevice(),
			graphicsQueue,
			static_cast<int>(queueIndex),
			graphicsQueue,
			static_cast<int>(queueIndex),
			graphicsQueue,
			static_cast<int>(queueIndex));

		if (!result) {
			settings.CreationEngineRaytracingSettings.Enabled = false;
			initialized = false;
			forcedDisabled = true;
			disableReason = DisableReason::InitFailed;
			logger::error("[Raytracing] Failed to initialize Creation Engine ray tracing via Vulkan renderer.");
			return false;
		}

		initialized = true;
		UpdateResolution();
		logger::info("[Raytracing] Successfully initialized Creation Engine ray tracing (Vulkan).");
		return true;
	}

	logger::warn("[Raytracing] Renderer backend unavailable or incompatible for Creation Engine ray tracing initialization.");
	return false;
}

bool Raytracing::UpdateResolution()
{
	if (!globals::game::graphicsState)
		return false;

	uint2 resolution{
		globals::game::graphicsState->screenWidth,
		globals::game::graphicsState->screenHeight
	};

	if (resolution == m_Resolution)
		return false;

	m_Resolution = resolution;
	creationEngineRaytracing->SetResolution(m_Resolution.x, m_Resolution.y);
	return true;
}

void Raytracing::SetupResourcesPostDeferred()
{
	if (!loaded)
		return;

	if (forcedDisabled)
		return;

	if (!initialized)
		InitializeCERaytracing();

	if (!initialized)
		return;

	creationEngineRaytracing->Initialize(GetSettings());

	if (!featureData)
		featureData = std::make_unique<CreationEngineRaytracing::FeatureData>();

	auto* device = globals::d3d::device;
	if (!screenCB)
		screenCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ScreenData>());
	if (!screenData)
		screenData = std::make_unique<ScreenData>();

	if (device) {
		if (!copyBlendState) {
			D3D11_BLEND_DESC blendDesc = {};
			blendDesc.AlphaToCoverageEnable = false;
			blendDesc.IndependentBlendEnable = false;
			blendDesc.RenderTarget[0].BlendEnable = false;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, copyBlendState.put()));
		}

		if (!copyRasterizerState) {
			D3D11_RASTERIZER_DESC rasterizerDesc = {};
			rasterizerDesc.FillMode = D3D11_FILL_SOLID;
			rasterizerDesc.CullMode = D3D11_CULL_NONE;
			rasterizerDesc.FrontCounterClockwise = false;
			rasterizerDesc.DepthBias = 0;
			rasterizerDesc.DepthBiasClamp = 0.0f;
			rasterizerDesc.SlopeScaledDepthBias = 0.0f;
			rasterizerDesc.DepthClipEnable = false;
			rasterizerDesc.ScissorEnable = false;
			rasterizerDesc.MultisampleEnable = false;
			rasterizerDesc.AntialiasedLineEnable = false;
			DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, copyRasterizerState.put()));
		}

		if (!depthStencilState) {
			D3D11_DEPTH_STENCIL_DESC dsDesc = {};
			dsDesc.DepthEnable = TRUE;
			dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
			dsDesc.StencilEnable = false;
			DX::ThrowIfFailed(device->CreateDepthStencilState(&dsDesc, depthStencilState.put()));
		}
	}

	SetupSkyHemisphere();
	SetupWaterFlowMap();
	SetupSharedTextures();
	CompileShaders();
}

void Raytracing::SetupSkyHemisphere()
{
	if (skyHemisphere)
		return;

	auto* device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width = SKY_HEMI_SIZE;
	texDesc.Height = SKY_HEMI_SIZE;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, skyHemisphere.put()));

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	DX::ThrowIfFailed(device->CreateShaderResourceView(skyHemisphere.get(), &srvDesc, skyHemisphereSRV.put()));

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = texDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice = 0;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(skyHemisphere.get(), &uavDesc, skyHemisphereUAV.put()));

	if (!samplerState) {
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, samplerState.put()));
	}

	waterReflections = RE::NiPointer(new RE::TESWaterReflections());
	waterReflections->flags.set(true, RE::TESWaterReflections::Flags::kDirty, RE::TESWaterReflections::Flags::kDynamicCubemap, RE::TESWaterReflections::Flags::kWorldOrigin);
	for (uint32_t i = 0; i < 6; i++) {
		waterReflections->cubeMapSides[i] = RE::TESWaterReflections::CubeMapSide(i, 0.0f);
	}

	creationEngineRaytracing->SetSkyHemisphere(skyHemisphere.get());
}

void Raytracing::SetupWaterFlowMap()
{
	if (waterFlowMap)
		return;

	auto* device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width = WATER_FLOWMAP_SIZE;
	texDesc.Height = WATER_FLOWMAP_SIZE;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, waterFlowMap.put()));

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	DX::ThrowIfFailed(device->CreateShaderResourceView(waterFlowMap.get(), &srvDesc, waterFlowMapSRV.put()));

	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = texDesc.Format;
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	rtvDesc.Texture2D.MipSlice = 0;
	DX::ThrowIfFailed(device->CreateRenderTargetView(waterFlowMap.get(), &rtvDesc, waterFlowMapRTV.put()));

	creationEngineRaytracing->SetWaterFlowMap(waterFlowMap.get());
}

void Raytracing::SetupSharedTextures()
{
	auto* renderer = globals::game::renderer;
	if (!renderer)
		return;

	auto* device = globals::d3d::device;
	if (!device)
		return;

	auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!mainTex.texture)
		return;

	D3D11_TEXTURE2D_DESC mainDesc{};
	mainTex.texture->GetDesc(&mainDesc);

	if (normalRoughnessTexture) {
		D3D11_TEXTURE2D_DESC currentDesc{};
		normalRoughnessTexture->GetDesc(&currentDesc);
		if (currentDesc.Width != mainDesc.Width || currentDesc.Height != mainDesc.Height) {
			normalRoughnessTexture = nullptr;
			normalRoughnessSRV = nullptr;
			normalRoughnessUAV = nullptr;
		}
	}

	if (!normalRoughnessTexture) {
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = mainDesc.Width;
		texDesc.Height = mainDesc.Height;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, normalRoughnessTexture.put()));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		DX::ThrowIfFailed(device->CreateShaderResourceView(normalRoughnessTexture.get(), &srvDesc, normalRoughnessSRV.put()));

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(normalRoughnessTexture.get(), &uavDesc, normalRoughnessUAV.put()));
	}

	auto* albedoTex = renderer->GetRuntimeData().renderTargets[ALBEDO].texture;
	auto* gnmaoTex = renderer->GetRuntimeData().renderTargets[MASKS2].texture;

	if (!albedoTex || !gnmaoTex) {
		return;
	}

	creationEngineRaytracing->SetSharedTextures(albedoTex, normalRoughnessTexture.get(), gnmaoTex);

	CreationEngineRaytracing::SharedTexture depth[CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT]{};
	CreationEngineRaytracing::SharedTexture motionVector[CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT]{};
	CreationEngineRaytracing::SharedTexture main[CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT]{};
	creationEngineRaytracing->GetSharedTextures(depth, motionVector, main);

	auto setupSharedWrapper = [device](SharedTextureWrapper& wrapper, const CreationEngineRaytracing::SharedTexture& st) {
			wrapper.texture = st;
			wrapper.srv = nullptr;
			if (st.shared) {
				D3D11_TEXTURE2D_DESC desc{};
				st.shared->GetDesc(&desc);
				if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
					D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
					srvDesc.Format = desc.Format;
					srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					srvDesc.Texture2D.MostDetailedMip = 0;
					srvDesc.Texture2D.MipLevels = 1;
					DX::ThrowIfFailed(device->CreateShaderResourceView(st.shared, &srvDesc, wrapper.srv.put()));
				}
			}
		};

		for (uint32_t i = 0; i < CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT; i++) {
			setupSharedWrapper(sharedDepthTextures[i], depth[i]);
			setupSharedWrapper(sharedMotionVectorTextures[i], motionVector[i]);
			setupSharedWrapper(sharedMainTextures[i], main[i]);
		}
}

void Raytracing::CompileShaders()
{
	std::string skyHemiSize = std::to_string(SKY_HEMI_SIZE);
	if (auto* rawPtr = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\Raytracing\\CubeToHemiCS.hlsl",
			{ { "RESOLUTION", skyHemiSize.c_str() } },
			"cs_5_0"))) {
		cubeToHemiCS.attach(rawPtr);
	}

	if (auto* rawPtr = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\Raytracing\\PTCompositeCS.hlsl",
			{},
			"cs_5_0"))) {
		ptCompositeCS.attach(rawPtr);
	}

	if (auto* rawPtr = static_cast<ID3D11VertexShader*>(Util::CompileShader(
			L"Data\\Shaders\\Raytracing\\CopyDepth.hlsl",
			{},
			"vs_5_0",
			"MainVS"))) {
		copyDepthVS.attach(rawPtr);
	}

	if (auto* rawPtr = static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\Raytracing\\CopyDepth.hlsl",
			{},
			"ps_5_0",
			"MainPS"))) {
		copyDepthPS.attach(rawPtr);
	}
}

void Raytracing::SkyCubeToHemi() const
{
	auto* context = globals::d3d::context;

	context->CSSetShader(cubeToHemiCS.get(), nullptr, 0);

	auto reflections = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];
	auto* reflectionOcc = globals::features::cloudShadows.loaded && globals::features::cloudShadows.texCubemapCloudOccCopy ?
	                          globals::features::cloudShadows.texCubemapCloudOccCopy->srv.get() :
	                          nullptr;

	ID3D11ShaderResourceView* srvs[] = {
		reflections.SRV,
		reflectionOcc
	};
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	auto* sampler = samplerState.get();
	context->CSSetSamplers(0, 1, &sampler);

	ID3D11UnorderedAccessView* uav = skyHemisphereUAV.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	uint32_t dispatch = static_cast<uint32_t>(std::ceil(SKY_HEMI_SIZE / 8.0f));
	context->Dispatch(dispatch, dispatch, 1);

	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	ID3D11ShaderResourceView* nullSRVs[ARRAYSIZE(srvs)] = { nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), nullSRVs);
}

void Raytracing::CopyWaterFlowMap() const
{
	if (!waterFlowMap || !waterFlowMapRTV)
		return;

	auto* context = globals::d3d::context;

	auto clearFlowMap = [&]() {
		const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearRenderTargetView(waterFlowMapRTV.get(), clearColor);
	};

	if (REL::Module::IsVR()) {
		clearFlowMap();
		return;
	}

	REL::Relocation<RE::NiPointer<RE::NiSourceTexture>*> gFlowMapSourceTex{ REL::RelocationID(527694, 414616) };
	auto* flowMapSourceTex = gFlowMapSourceTex.get();
	if (!flowMapSourceTex) {
		clearFlowMap();
		return;
	}

	auto* sourceTexture = flowMapSourceTex->get();
	if (!sourceTexture || !sourceTexture->rendererTexture || !sourceTexture->rendererTexture->texture) {
		clearFlowMap();
		return;
	}

	context->CopyResource(waterFlowMap.get(), sourceTexture->rendererTexture->texture);
}

void Raytracing::DrawSettings()
{
	if (disableReason == DisableReason::MissingPlugin) {
		ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
			T(TKEY("disable_reason_missing_plugin"), "Missing 'CreationEngineRaytracing.dll', check your mod manager."));
		return;
	} else if (disableReason == DisableReason::InitFailed) {
		ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
			T(TKEY("disable_reason_init_failed"), "Initialization Failed, check CreationEngineRaytracing.txt log"));
		return;
	}

	ImGui::Text("%s", T(TKEY("status_initialized"), "CreationEngineRaytracing runtime is active and initialized."));
	ImGui::Spacing();

	auto ceRTSettingsBefore = GetSettings();

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.CreationEngineRaytracingSettings.Enabled);

	const auto maxThreads = std::max(1u, std::thread::hardware_concurrency() - 1u);
	ImGui::SliderInt(T(TKEY("num_worker_threads"), "Number of Worker Threads"), reinterpret_cast<int*>(&settings.CreationEngineRaytracingSettings.AdvancedSettings.NumWorkerThreads), 1, maxThreads);

	ImGui::Checkbox(T(TKEY("validation_layer"), "Validation Layer"), &settings.RendererSettings.ValidationLayer);

	ImGui::Checkbox(T(TKEY("enable_ser"), "Enable Shader Execution Reordering"), &settings.CreationEngineRaytracingSettings.AdvancedSettings.ShaderExecutionReordering);

	ImGui::Checkbox(T(TKEY("use_ray_query"), "Use Ray Query"), &settings.RendererSettings.UseRayQuery);

	ImGui::Checkbox(T(TKEY("render_tree_lod"), "Render Tree LOD"), &settings.CreationEngineRaytracingSettings.ExperimentalSettings.RenderTreeLOD);

	const char* overlayNames[] = {
		T(TKEY("performance_overlay_none"), "None"),
		T(TKEY("performance_overlay_simple"), "Simple"),
		T(TKEY("performance_overlay_complete"), "Complete"),
		T(TKEY("performance_overlay_extended"), "Extended")
	};
	int currentOverlay = static_cast<int>(settings.PerfOverlay);
	if (ImGui::Combo(T(TKEY("performance_overlay"), "Performance Overlay"), &currentOverlay, overlayNames, IM_ARRAYSIZE(overlayNames))) {
		settings.PerfOverlay = static_cast<OverlayMode>(currentOverlay);
	}

	ImGui::Checkbox(T(TKEY("display_scenegraph_counters"), "Display SceneGraph Counters"), &settings.DisplaySceneGraphCounters);

	if (ceRTSettingsBefore != GetSettings()) {
		UpdateSettings();
	}
}

void Raytracing::DrawOverlay()
{
	if (!IsOverlayVisible())
		return;

	auto* menu = Menu::GetSingleton();

	if (!globals::state || !menu)
		return;

	// Set window flags - no decoration and only movable when menu is enabled
	ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize;

	// Only allow mouse interaction when the main menu is open
	if (!menu->IsEnabled) {
		windowFlags |= ImGuiWindowFlags_NoInputs;
	}

	windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;

	if (!PositionSet) {
		Position = ImVec2(10, 10);
		ImGui::SetNextWindowPos(Position);
		PositionSet = true;
	} else {
		ImGui::SetNextWindowPos(Position, ImGuiCond_FirstUseEver);
	}

	const auto overlayTitle = StableLabel(T(TKEY("overlay_title"), "Raytracing Overlay"), "RaytracingOverlay");
	ImGui::Begin(overlayTitle.c_str(), nullptr, windowFlags);

	auto DrawRow = [](const char* label, float cpuMS, float gpuMS) {
		ImGui::TableNextRow();

		ImGui::TableNextColumn();
		ImGui::TextUnformatted(label);

		ImGui::TableNextColumn();
		ImGui::Text(T(TKEY("overlay_cpu_ms"), "%g ms"), cpuMS);

		ImGui::TableNextColumn();
		ImGui::Text(T(TKEY("overlay_gpu_ms"), "%g ms"), gpuMS);
	};

	if (ImGui::BeginTable("Passes", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
		ImGui::TableSetupColumn(T(TKEY("overlay_pass"), "Pass"));
		ImGui::TableSetupColumn(T(TKEY("overlay_cpu"), "CPU"));
		ImGui::TableSetupColumn(T(TKEY("overlay_gpu"), "GPU"));
		ImGui::TableHeadersRow();

		if (settings.PerfOverlay == OverlayMode::Simple) {
			if (!passTimings.empty()) {
				const auto& passTiming = passTimings.back();
				DrawRow(passTiming.name.c_str(), passTiming.cpuTiming, passTiming.gpuTiming);
			}
		} else if (settings.PerfOverlay == OverlayMode::Complete || settings.PerfOverlay == OverlayMode::Extended) {
			for (const auto& passTiming : passTimings)
				DrawRow(passTiming.name.c_str(), passTiming.cpuTiming, passTiming.gpuTiming);
		}

		ImGui::EndTable();
	}

	// Display accumulated frame count when using Accumulation denoiser
	if (GetSettings().GeneralSettings.Denoiser == CreationEngineRaytracing::Denoiser::Accumulation &&
		creationEngineRaytracing && creationEngineRaytracing->GetAccumulatedFrameCount) {
		uint32_t accumulatedFrames = creationEngineRaytracing->GetAccumulatedFrameCount();
		ImGui::Text(T(TKEY("overlay_accumulated_frames"), "Accumulated Frames: %u"), accumulatedFrames);
	}

	if (settings.DisplaySceneGraphCounters && creationEngineRaytracing && creationEngineRaytracing->GetSceneGraphCounters) {
		uint32_t textures = 0;
		uint32_t models = 0;
		uint32_t instances = 0;

		creationEngineRaytracing->GetSceneGraphCounters(textures, models, instances);

		ImGui::Text(T(TKEY("overlay_textures"), "Textures %u"), textures);
		ImGui::Text(T(TKEY("overlay_models"), "Models %u"), models);
		ImGui::Text(T(TKEY("overlay_instances"), "Instances %u"), instances);
	}

	ImGui::End();
}

void Raytracing::UpdateFeatureData()
{
	if (!initialized)
		return;

	if (!featureData)
		featureData = std::make_unique<CreationEngineRaytracing::FeatureData>();

	std::memset(featureData.get(), 0, sizeof(CreationEngineRaytracing::FeatureData));

	auto wetnessEffect = globals::features::wetnessEffects.GetCommonBufferData();
	auto linearLighting = globals::features::linearLighting.GetCommonBufferData();
	auto skinData = globals::features::skin.GetCommonBufferData();

	// Extended Material
	{
		const auto& em = globals::features::extendedMaterials.settings;
		featureData->ExtendedMaterial.EnableComplexMaterial = em.EnableComplexMaterial;
		featureData->ExtendedMaterial.EnableParallax = em.EnableParallax;
		featureData->ExtendedMaterial.EnableTerrainParallax = em.EnableTerrain;
		featureData->ExtendedMaterial.EnableHeightBlending = em.EnableHeightBlending;
		featureData->ExtendedMaterial.EnableShadows = em.EnableShadows;
		featureData->ExtendedMaterial.ExtendShadows = FALSE;
		featureData->ExtendedMaterial.EnableParallaxWarpingFix = em.EnableParallaxWarpingFix;
		featureData->ExtendedMaterial.pad0 = 0;
	}

	// Wetness Effects
	{
		std::memcpy(&featureData->WetnessEffects.OcclusionViewProj, &wetnessEffect.OcclusionViewProj, sizeof(featureData->WetnessEffects.OcclusionViewProj));
		featureData->WetnessEffects.Time = wetnessEffect.Time;
		featureData->WetnessEffects.Raining = wetnessEffect.Raining;
		featureData->WetnessEffects.Wetness = wetnessEffect.Wetness;
		featureData->WetnessEffects.PuddleWetness = wetnessEffect.PuddleWetness;

		const auto& weSettings = wetnessEffect.settings;
		featureData->WetnessEffects.EnableWetnessEffects = weSettings.EnableWetnessEffects;
		featureData->WetnessEffects.MaxRainWetness = weSettings.MaxRainWetness;
		featureData->WetnessEffects.MaxPuddleWetness = weSettings.MaxPuddleWetness;
		featureData->WetnessEffects.MaxShoreWetness = weSettings.MaxShoreWetness;
		featureData->WetnessEffects.ShoreRange = weSettings.ShoreRange;
		featureData->WetnessEffects.PuddleRadius = weSettings.PuddleRadius;
		featureData->WetnessEffects.PuddleMaxAngle = weSettings.PuddleMaxAngle;
		featureData->WetnessEffects.PuddleMinWetness = weSettings.PuddleMinWetness;
		featureData->WetnessEffects.MinRainWetness = weSettings.MinRainWetness;
		featureData->WetnessEffects.SkinWetness = weSettings.SkinWetness;
		featureData->WetnessEffects.WeatherTransitionSpeed = weSettings.WeatherTransitionSpeed;
		featureData->WetnessEffects.EnableRaindropFx = weSettings.EnableRaindropFx;
		featureData->WetnessEffects.EnableSplashes = weSettings.EnableSplashes;
		featureData->WetnessEffects.EnableRipples = weSettings.EnableRipples;
		featureData->WetnessEffects.EnableVanillaRipples = weSettings.EnableVanillaRipples;
		featureData->WetnessEffects.RaindropFxRange = weSettings.RaindropFxRange;
		featureData->WetnessEffects.RaindropGridSizeRcp = weSettings.RaindropGridSize;
		featureData->WetnessEffects.RaindropIntervalRcp = weSettings.RaindropInterval;
		featureData->WetnessEffects.RaindropChance = weSettings.RaindropChance;
		featureData->WetnessEffects.SplashesLifetime = weSettings.SplashesLifetime;
		featureData->WetnessEffects.SplashesStrength = weSettings.SplashesStrength;
		featureData->WetnessEffects.SplashesMinRadius = weSettings.SplashesMinRadius;
		featureData->WetnessEffects.SplashesMaxRadius = weSettings.SplashesMaxRadius;
		featureData->WetnessEffects.RippleStrength = weSettings.RippleStrength;
		featureData->WetnessEffects.RippleRadius = weSettings.RippleRadius;
		featureData->WetnessEffects.RippleBreadth = weSettings.RippleBreadth;
		featureData->WetnessEffects.RippleLifetimeRcp = weSettings.RippleLifetime;
		featureData->WetnessEffects.pad0 = 0.0f;
	}

	// Cloud Shadows
	{
		featureData->CloudShadows.Opacity = globals::features::cloudShadows.settings.Opacity;
		featureData->CloudShadows.pad0 = { 0.0f, 0.0f, 0.0f };
	}

	// Hair Specular
	{
		const auto& hs = globals::features::hairSpecular.settings;
		featureData->HairSpecular.Enabled = hs.Enabled;
		featureData->HairSpecular.HairGlossiness = hs.HairGlossiness;
		featureData->HairSpecular.SpecularMult = hs.SpecularMult;
		featureData->HairSpecular.DiffuseMult = hs.DiffuseMult;
		featureData->HairSpecular.EnableTangentShift = hs.EnableTangentShift;
		featureData->HairSpecular.PrimaryTangentShift = hs.PrimaryTangentShift;
		featureData->HairSpecular.SecondaryTangentShift = hs.SecondaryTangentShift;
		featureData->HairSpecular.HairSaturation = hs.HairSaturation;
		featureData->HairSpecular.SpecularIndirectMult = hs.SpecularIndirectMult;
		featureData->HairSpecular.DiffuseIndirectMult = hs.DiffuseIndirectMult;
		featureData->HairSpecular.BaseColorMult = hs.BaseColorMult;
		featureData->HairSpecular.Transmission = hs.Transmission;
		featureData->HairSpecular.EnableSelfShadow = hs.EnableSelfShadow;
		featureData->HairSpecular.SelfShadowStrength = hs.SelfShadowStrength;
		featureData->HairSpecular.SelfShadowExponent = hs.SelfShadowExponent;
		featureData->HairSpecular.SelfShadowScale = hs.SelfShadowScale;
		featureData->HairSpecular.HairMode = hs.HairMode;
		featureData->HairSpecular.pad1 = 0;
		featureData->HairSpecular.pad2 = 0;
		featureData->HairSpecular.pad3 = 0;
	}

	// Extended Translucency
	{
		const auto& et = globals::features::extendedTranslucency.GetCommonBufferData();
		featureData->ExtendedTranslucency.MaterialModel = et.AlphaMode;
		featureData->ExtendedTranslucency.Reduction = et.AlphaReduction;
		featureData->ExtendedTranslucency.Softness = et.AlphaSoftness;
		featureData->ExtendedTranslucency.Strength = et.AlphaStrength;
	}

	// Linear Lighting
	{
		featureData->LinearLighting.enableLinearLighting = linearLighting.enableLinearLighting;
		featureData->LinearLighting.isDirLightLinear = linearLighting.isDirLightLinear;
		featureData->LinearLighting.dirLightMult = linearLighting.dirLightMult;
		featureData->LinearLighting.lightGamma = linearLighting.lightGamma;
		featureData->LinearLighting.colorGamma = linearLighting.colorGamma;
		featureData->LinearLighting.emitColorGamma = linearLighting.emitColorGamma;
		featureData->LinearLighting.glowmapGamma = linearLighting.glowmapGamma;
		featureData->LinearLighting.ambientGamma = linearLighting.ambientGamma;
		featureData->LinearLighting.fogGamma = linearLighting.fogGamma;
		featureData->LinearLighting.fogAlphaGamma = linearLighting.fogAlphaGamma;
		featureData->LinearLighting.effectGamma = linearLighting.effectGamma;
		featureData->LinearLighting.effectAlphaGamma = linearLighting.effectAlphaGamma;
		featureData->LinearLighting.skyGamma = linearLighting.skyGamma;
		featureData->LinearLighting.waterGamma = linearLighting.waterGamma;
		featureData->LinearLighting.vlGamma = linearLighting.vlGamma;
		featureData->LinearLighting.vanillaDiffuseColorMult = linearLighting.vanillaDiffuseColorMult;
		featureData->LinearLighting.directionalLightMult = linearLighting.directionalLightMult;
		featureData->LinearLighting.pointLightMult = linearLighting.pointLightMult;
		featureData->LinearLighting.ambientMult = linearLighting.ambientMult;
		featureData->LinearLighting.emitColorMult = linearLighting.emitColorMult;
		featureData->LinearLighting.glowmapMult = linearLighting.glowmapMult;
		featureData->LinearLighting.effectLightingMult = linearLighting.effectLightingMult;
		featureData->LinearLighting.membraneEffectMult = linearLighting.membraneEffectMult;
		featureData->LinearLighting.bloodEffectMult = linearLighting.bloodEffectMult;
		featureData->LinearLighting.projectedEffectMult = linearLighting.projectedEffectMult;
		featureData->LinearLighting.deferredEffectMult = linearLighting.deferredEffectMult;
		featureData->LinearLighting.otherEffectMult = linearLighting.otherEffectMult;
		featureData->LinearLighting.pad0 = 0;
	}

	// Exponential Height Fog
	{
		const auto& ehf = globals::features::exponentialHeightFog.settings;
		featureData->ExponentialHeightFog.enabled = ehf.enabled;
		featureData->ExponentialHeightFog.useDynamicCubemaps = ehf.useDynamicCubemaps;
		featureData->ExponentialHeightFog.startDistance = ehf.startDistance;
		featureData->ExponentialHeightFog.fogHeight = ehf.fogHeight;
		featureData->ExponentialHeightFog.fogHeightFalloff = ehf.fogHeightFalloff;
		featureData->ExponentialHeightFog.fogDensity = ehf.fogDensity;
		featureData->ExponentialHeightFog.directionalInscatteringMultiplier = ehf.directionalInscatteringMultiplier;
		featureData->ExponentialHeightFog.directionalInscatteringAnisotropy = ehf.directionalInscatteringAnisotropy;
		featureData->ExponentialHeightFog.inscatteringTint = ehf.inscatteringTint;
		featureData->ExponentialHeightFog.cubemapMipLevel = ehf.cubemapMipLevel;
		featureData->ExponentialHeightFog.sunlightAttenuationAmount = ehf.sunlightAttenuationAmount;
		featureData->ExponentialHeightFog.respectVanillaFogFade = ehf.respectVanillaFogFade;
		featureData->ExponentialHeightFog.disableVanillaFog = ehf.disableVanillaFog;
		featureData->ExponentialHeightFog.fogInscatteringColor = ehf.fogInscatteringColor;
		featureData->ExponentialHeightFog.originalFogColorAmount = ehf.originalFogColorAmount;
		featureData->ExponentialHeightFog.volumetricFogEnabled = ehf.volumetricFogEnabled;
		featureData->ExponentialHeightFog.volumetricGridPixelSize = ehf.volumetricGridPixelSize;
		featureData->ExponentialHeightFog.volumetricGridSizeZ = ehf.volumetricGridSizeZ;
		featureData->ExponentialHeightFog.volumetricFogDistance = ehf.volumetricFogDistance;
		featureData->ExponentialHeightFog.volumetricFogStartDistance = ehf.volumetricFogStartDistance;
		featureData->ExponentialHeightFog.volumetricFogNearFadeInDistance = ehf.volumetricFogNearFadeInDistance;
		featureData->ExponentialHeightFog.volumetricFogExtinctionScale = ehf.volumetricFogExtinctionScale;
		featureData->ExponentialHeightFog.volumetricFogAlbedo = ehf.volumetricFogAlbedo;
		featureData->ExponentialHeightFog.volumetricFogEmissive = ehf.volumetricFogEmissive;
		featureData->ExponentialHeightFog.volumetricDirectionalScatteringIntensity = ehf.volumetricDirectionalScatteringIntensity;
		featureData->ExponentialHeightFog.volumetricShadowBias = ehf.volumetricShadowBias;
		featureData->ExponentialHeightFog.volumetricDepthDistributionScale = ehf.volumetricDepthDistributionScale;
		featureData->ExponentialHeightFog.volumetricSkyLightingIntensity = ehf.volumetricSkyLightingIntensity;
		featureData->ExponentialHeightFog.volumetricFogScatteringDistribution = ehf.volumetricFogScatteringDistribution;
		featureData->ExponentialHeightFog.volumetricHistoryWeight = ehf.volumetricHistoryWeight;
		featureData->ExponentialHeightFog.volumetricHistoryMissSampleCount = ehf.volumetricHistoryMissSampleCount;
		featureData->ExponentialHeightFog.volumetricSampleJitterMultiplier = ehf.volumetricSampleJitterMultiplier;
		featureData->ExponentialHeightFog.volumetricUpsampleJitterMultiplier = ehf.volumetricUpsampleJitterMultiplier;
		featureData->ExponentialHeightFog.volumetricLocalLightScatteringIntensity = ehf.volumetricLocalLightScatteringIntensity;
		featureData->ExponentialHeightFog.pad0 = ehf.pad0;
	}

	// LOD Blending
	{
		const auto& lod = globals::features::lodBlending.settings;
		featureData->LODBlending.LODTerrainBrightness = lod.LODTerrainBrightness;
		featureData->LODBlending.LODObjectBrightness = lod.LODObjectBrightness;
		featureData->LODBlending.LODObjectSnowBrightness = lod.LODObjectSnowBrightness;
		featureData->LODBlending.DisableTerrainVertexColors = lod.DisableTerrainVertexColors;
		featureData->LODBlending.LODTerrainGamma = lod.LODTerrainGamma;
		featureData->LODBlending.LODObjectGamma = lod.LODObjectGamma;
		featureData->LODBlending.LODObjectSnowGamma = lod.LODObjectSnowGamma;
		featureData->LODBlending.pad = lod.pad;
	}

	// Skin
	{
		featureData->Skin.skinParams = skinData.skinParams;
		featureData->Skin.skinParams2 = skinData.skinParams2;
		featureData->Skin.skinDetailParams = skinData.skinDetailParams;
		featureData->Skin.sssParams = skinData.sssParams;
		featureData->Skin.fuzzParams = skinData.fuzzParams;
		featureData->Skin.physicalParams = skinData.physicalParams;
		featureData->Skin.wetParams = skinData.wetParams;
	}

	creationEngineRaytracing->UpdateFeatureData(featureData.get(), sizeof(CreationEngineRaytracing::FeatureData));
}

RE::BSEventNotifyControl Raytracing::BGSActorCellEventHandler::ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*)
{
	if (a_event->flags.underlying() != static_cast<uint32_t>(RE::BGSActorCellEvent::CellFlag::kEnter))
		return RE::BSEventNotifyControl::kContinue;

	auto* tesWaterSystem = RE::TESWaterSystem::GetSingleton();

	if (tesWaterSystem->waterReflections.empty()) {
		tesWaterSystem->waterReflections.push_back(globals::features::raytracing.waterReflections);
	}

	tesWaterSystem->Enable();

	return RE::BSEventNotifyControl::kContinue;
}
