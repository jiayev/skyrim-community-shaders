#include "Streamline.h"

#include <dxgi.h>

#include "Util.h"

void LoggingCallback(sl::LogType type, const char* msg)
{
	switch (type) {
	case sl::LogType::eInfo:
		logger::info("{}", msg);
		break;
	case sl::LogType::eWarn:
		logger::warn("{}", msg);
		break;
	case sl::LogType::eError:
		logger::error("{}", msg);
		break;
	}
}

void Streamline::DrawSettings()
{
	if (!REL::Module::IsVR()) {
		if (ImGui::CollapsingHeader("NVIDIA DLSS", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick)) {
			if (streamlineActive) {
				ImGui::Text("Streamline uses a D3D11 to D3D12 proxy");
				ImGui::Text("Frame Generation always defaults to Auto");
				ImGui::Text("To disable Frame Generation, disable it in your mod manager");

				const char* frameGenerationModes[] = { "Off", "On", "Auto" };
				frameGenerationMode = (sl::DLSSGMode)std::min(2u, (uint)frameGenerationMode);
				ImGui::SliderInt("Frame Generation", (int*)&frameGenerationMode, 0, 2, std::format("{}", frameGenerationModes[(uint)frameGenerationMode]).c_str());
			} else {
				ImGui::Text("Streamline uses a D3D11 to D3D12 proxy");
				ImGui::Text("Streamline is not active due to no available plugins");
				ImGui::Text("To enable Frame Generation, enable it in your mod manager");
			}
		}
	}
}

void Streamline::Shutdown()
{
	if (SL_FAILED(res, slShutdown())) {
		logger::error("[Streamline] Failed to shutdown Streamline");
	} else {
		logger::info("[Streamline] Sucessfully shutdown Streamline");
	}
}

void Streamline::Initialize_preDevice()
{
	logger::info("[Streamline] Initializing Streamline");

	interposer = LoadLibraryW(L"Data/SKSE/Plugins/Streamline/sl.interposer.dll");

	sl::Preferences pref;

	sl::Feature featuresToLoad[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex };
	pref.featuresToLoad = featuresToLoad;
	pref.numFeaturesToLoad = _countof(featuresToLoad);

	pref.logLevel = sl::LogLevel::eOff;
	pref.logMessageCallback = LoggingCallback;

	const wchar_t* pathsToPlugins[] = { L"Data/SKSE/Plugins/Streamline" };
	pref.pathsToPlugins = pathsToPlugins;
	pref.numPathsToPlugins = _countof(pathsToPlugins);

	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";

	pref.renderAPI = sl::RenderAPI::eD3D11;

	// Hook up all of the functions exported by the SL Interposer Library
	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(interposer, "slIsFeatureSupported");
	slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)GetProcAddress(interposer, "slIsFeatureLoaded");
	slSetFeatureLoaded = (PFun_slSetFeatureLoaded*)GetProcAddress(interposer, "slSetFeatureLoaded");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
	slAllocateResources = (PFun_slAllocateResources*)GetProcAddress(interposer, "slAllocateResources");
	slFreeResources = (PFun_slFreeResources*)GetProcAddress(interposer, "slFreeResources");
	slSetTag = (PFun_slSetTag*)GetProcAddress(interposer, "slSetTag");
	slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GetProcAddress(interposer, "slGetFeatureRequirements");
	slGetFeatureVersion = (PFun_slGetFeatureVersion*)GetProcAddress(interposer, "slGetFeatureVersion");
	slUpgradeInterface = (PFun_slUpgradeInterface*)GetProcAddress(interposer, "slUpgradeInterface");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetNativeInterface = (PFun_slGetNativeInterface*)GetProcAddress(interposer, "slGetNativeInterface");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");

	if (SL_FAILED(res, slInit(pref, sl::kSDKVersion))) {
		logger::error("[Streamline] Failed to initialize Streamline");
		streamlineActive = false;
	} else {
		logger::info("[Streamline] Sucessfully initialized Streamline");
		streamlineActive = true;
	}

	initialized = true;
}

void Streamline::Initialize_postDevice()
{
	// Hook up all of the feature functions using the sl function slGetFeatureFunction
	slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", (void*&)slDLSSGGetState);
	slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)slDLSSGSetOptions);

	slGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", (void*&)slReflexGetState);
	slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetMarker", (void*&)slReflexSetMarker);
	slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", (void*&)slReflexSleep);
	slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", (void*&)slReflexSetOptions);

	// We set reflex consts to a default config. This can be changed at runtime in the UI.
	auto reflexOptions = sl::ReflexOptions{};
	reflexOptions.mode = sl::ReflexMode::eLowLatencyWithBoost;
	reflexOptions.useMarkersToOptimize = false;
	reflexOptions.virtualKey = 0;
	reflexOptions.frameLimitUs = 0;

	if (SL_FAILED(res, slReflexSetOptions(reflexOptions))) {
		logger::error("[Streamline] Failed to set reflex options");
	} else {
		logger::info("[Streamline] Sucessfully set reflex options");
	}
}

HRESULT Streamline::CreateDXGIFactory(REFIID riid, void** ppFactory)
{
	if (!initialized)
		Initialize_preDevice();

	logger::info("[Streamline] Proxying CreateDXGIFactory");

	auto slCreateDXGIFactory1 = reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(interposer, "CreateDXGIFactory1"));

	return slCreateDXGIFactory1(riid, ppFactory);
}

HRESULT Streamline::CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	const D3D_FEATURE_LEVEL* pFeatureLevels,
	UINT FeatureLevels,
	UINT SDKVersion,
	const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext,
	bool& o_streamlineProxy)
{
	if (!streamlineActive) {
		logger::info("[Streamline] Streamline was not initialized before calling D3D11CreateDeviceAndSwapChain");

		streamlineActive = false;
		o_streamlineProxy = false;

		return S_OK;
	}

	bool featureLoaded = false;
	slIsFeatureLoaded(sl::kFeatureDLSS_G, featureLoaded);

	if (featureLoaded) {
		logger::info("[Streamline] Frame generation feature is loaded");
	} else {
		logger::info("[Streamline] Frame generation feature is not loaded");

		Shutdown();

		streamlineActive = false;
		o_streamlineProxy = false;

		return S_OK;
	}

	DXGI_ADAPTER_DESC adapterDesc;
	pAdapter->GetDesc(&adapterDesc);

	sl::AdapterInfo adapterInfo;
	adapterInfo.deviceLUID = (uint8_t*)&adapterDesc.AdapterLuid;
	adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

	if (slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo) == sl::Result::eOk) {
		logger::info("[Streamline] Frame generation is supported on this adapter");
	} else {
		logger::info("[Streamline] Frame generation is not supported on this adapter");

		Shutdown();

		streamlineActive = false;
		o_streamlineProxy = false;

		return S_OK;
	}

	logger::info("[Streamline] Proxying D3D11CreateDeviceAndSwapChain");

	auto slD3D11CreateDeviceAndSwapChain = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(GetProcAddress(interposer, "D3D11CreateDeviceAndSwapChain"));

	auto hr = slD3D11CreateDeviceAndSwapChain(
		pAdapter,
		DriverType,
		Software,
		Flags,
		pFeatureLevels,
		FeatureLevels,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);

	Initialize_postDevice();

	viewport = { 0 };

	o_streamlineProxy = true;

	return hr;
}

void Streamline::CreateFrameGenerationResources()
{
	logger::info("[Streamline] Creating frame generation resources");

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

	texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	colorBufferShared = new Texture2D(texDesc);
	colorBufferShared->CreateSRV(srvDesc);
	colorBufferShared->CreateRTV(rtvDesc);
	colorBufferShared->CreateUAV(uavDesc);

	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.Format = texDesc.Format;
	rtvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	depthBufferShared = new Texture2D(texDesc);
	depthBufferShared->CreateSRV(srvDesc);
	depthBufferShared->CreateRTV(rtvDesc);
	depthBufferShared->CreateUAV(uavDesc);

	copyDepthToSharedBufferCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Streamline\\CopyDepthToSharedBufferCS.hlsl", {}, "cs_5_0");
}

void Streamline::SetupFrameGeneration()
{
	if (!streamlineActive)
		return;

	CreateFrameGenerationResources();

	sl::DLSSGOptions options{};
	options.mode = sl::DLSSGMode::eAuto;
	options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;

	if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
		logger::error("[Streamline] Could not enable DLSSG");
	}
}

void Streamline::CopyResourcesToSharedBuffers()
{
	if (!streamlineActive || frameGenerationMode == sl::DLSSGMode::eOff)
		return;

	auto& context = State::GetSingleton()->context;
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	if (RE::UI::GetSingleton()->GameIsPaused()) {
		float clearColor[4] = { 0, 0, 0, 0 };
		auto& motionVectorsBuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::RENDER_TARGET::kMOTION_VECTOR];
		context->ClearRenderTargetView(motionVectorsBuffer.RTV, clearColor);
	}

	ID3D11RenderTargetView* backupViews[8];
	ID3D11DepthStencilView* backupDsv;
	context->OMGetRenderTargets(8, backupViews, &backupDsv);  // Backup bound render targets
	context->OMSetRenderTargets(0, nullptr, nullptr);         // Unbind all bound render targets

	auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

	ID3D11Resource* swapChainResource;
	swapChain.SRV->GetResource(&swapChainResource);

	context->CopyResource(colorBufferShared->resource.get(), swapChainResource);

	{
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		{
			auto dispatchCount = Util::GetScreenDispatchCount();

			ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			ID3D11UnorderedAccessView* uavs[1] = { depthBufferShared->uav.get() };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetShader(copyDepthToSharedBufferCS, nullptr, 0);

			context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
		}

		ID3D11ShaderResourceView* views[1] = { nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);
	}

	context->OMSetRenderTargets(8, backupViews, backupDsv);  // Restore all bound render targets

	for (int i = 0; i < 8; i++) {
		if (backupViews[i])
			backupViews[i]->Release();
	}

	if (backupDsv)
		backupDsv->Release();
}

void Streamline::Present()
{
	if (!streamlineActive)
		return;

	static auto currentFrameGenerationMode = frameGenerationMode;

	if (currentFrameGenerationMode != frameGenerationMode) {
		currentFrameGenerationMode = frameGenerationMode;

		sl::DLSSGOptions options{};
		options.mode = frameGenerationMode;
		options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;

		if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
			logger::error("[Streamline] Could not set DLSSG");
		}
	}

	// Fake NVIDIA Reflex to prevent DLSSG errors
	slReflexSetMarker(sl::ReflexMarker::eInputSample, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::eSimulationStart, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::eSimulationEnd, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::eRenderSubmitStart, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::eRenderSubmitEnd, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::ePresentStart, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::ePresentEnd, *currentFrame);

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto& motionVectorsBuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::RENDER_TARGET::kMOTION_VECTOR];

	auto state = State::GetSingleton();

	sl::Extent fullExtent{ 0, 0, (uint)state->screenSize.x, (uint)state->screenSize.y };

	float2 dynamicScreenSize = Util::ConvertToDynamic(State::GetSingleton()->screenSize);
	sl::Extent dynamicExtent{ 0, 0, (uint)dynamicScreenSize.x, (uint)dynamicScreenSize.y };

	sl::Resource depth = { sl::ResourceType::eTex2d, depthBufferShared->resource.get(), 0 };
	sl::ResourceTag depthTag = sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &dynamicExtent };

	sl::Resource mvec = { sl::ResourceType::eTex2d, motionVectorsBuffer.texture, 0 };
	sl::ResourceTag mvecTag = sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &dynamicExtent };

	sl::Resource hudLess = { sl::ResourceType::eTex2d, colorBufferShared->resource.get(), 0 };
	sl::ResourceTag hudLessTag = sl::ResourceTag{ &hudLess, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

	sl::Resource ui = { sl::ResourceType::eTex2d, nullptr, 0 };
	sl::ResourceTag uiTag = sl::ResourceTag{ &ui, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

	sl::ResourceTag inputs[] = { depthTag, mvecTag, hudLessTag, uiTag };
	slSetTag(viewport, inputs, _countof(inputs), nullptr);
}

void Streamline::SetConstants()
{
	if (!streamlineActive)
		return;

	auto state = State::GetSingleton();

	auto cameraData = Util::GetCameraData(0);
	auto eyePosition = Util::GetEyePosition(0);

	auto clipToCameraView = cameraData.viewMat.Invert();
	auto cameraToWorld = cameraData.viewProjMatrixUnjittered.Invert();
	auto cameraToWorldPrev = cameraData.previousViewProjMatrixUnjittered.Invert();

	float4x4 cameraToPrevCamera;

	calcCameraToPrevCamera(*(sl::float4x4*)&cameraToPrevCamera, *(sl::float4x4*)&cameraToWorld, *(sl::float4x4*)&cameraToWorldPrev);

	float4x4 prevCameraToCamera = cameraToPrevCamera;

	prevCameraToCamera.Invert();

	sl::Constants slConstants = {};

	slConstants.cameraAspectRatio = state->screenSize.x / state->screenSize.y;
	slConstants.cameraFOV = Util::GetVerticalFOVRad();
	slConstants.cameraFar = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x44));
	slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
	slConstants.cameraNear = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x40));
	slConstants.cameraPinholeOffset = { 0.f, 0.f };
	slConstants.cameraPos = *(sl::float3*)&eyePosition;
	slConstants.cameraFwd = *(sl::float3*)&cameraData.viewForward;
	slConstants.cameraUp = *(sl::float3*)&cameraData.viewUp;
	slConstants.cameraRight = *(sl::float3*)&cameraData.viewRight;
	slConstants.cameraViewToClip = *(sl::float4x4*)&cameraData.viewMat;
	slConstants.clipToCameraView = *(sl::float4x4*)&clipToCameraView;
	slConstants.clipToPrevClip = *(sl::float4x4*)&cameraToPrevCamera;
	slConstants.depthInverted = sl::Boolean::eFalse;
	slConstants.jitterOffset = { 0, 0 };
	slConstants.mvecScale = { 1, 1 };
	slConstants.prevClipToClip = *(sl::float4x4*)&prevCameraToCamera;
	slConstants.reset = sl::Boolean::eFalse;
	slConstants.motionVectors3D = sl::Boolean::eFalse;
	slConstants.motionVectorsInvalidValue = FLT_MIN;
	slConstants.orthographicProjection = sl::Boolean::eFalse;
	slConstants.motionVectorsDilated = sl::Boolean::eFalse;
	slConstants.motionVectorsJittered = sl::Boolean::eFalse;

	if (SL_FAILED(res, slGetNewFrameToken(currentFrame, nullptr))) {
		logger::error("[Streamline] Could not get frame token");
	}

	if (SL_FAILED(res, slSetConstants(slConstants, *currentFrame, viewport))) {
		logger::error("[Streamline] Could not set constants");
	}
}