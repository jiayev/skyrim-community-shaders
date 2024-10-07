#include "FidelityFX.h"

#include "Upscaling.h"

#include "State.h"
#include "Util.h"

FfxResource ffxGetResource(ID3D11Resource* dx11Resource,
	[[maybe_unused]] wchar_t const* ffxResName,
	FfxResourceStates state /*=FFX_RESOURCE_STATE_COMPUTE_READ*/)
{
	FfxResource resource = {};
	resource.resource = reinterpret_cast<void*>(const_cast<ID3D11Resource*>(dx11Resource));
	resource.state = state;
	resource.description = GetFfxResourceDescriptionDX11(dx11Resource);

#ifdef _DEBUG
	if (ffxResName) {
		wcscpy_s(resource.name, ffxResName);
	}
#endif

	return resource;
}

void FidelityFX::CreateFSRResources()
{
	auto state = State::GetSingleton();

	auto fsrDevice = ffxGetDeviceDX11(state->device);

	size_t scratchBufferSize = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
	void* scratchBuffer = calloc(scratchBufferSize, 1);
	memset(scratchBuffer, 0, scratchBufferSize);

	FfxInterface fsrInterface;
	if (ffxGetInterfaceDX11(&fsrInterface, fsrDevice, scratchBuffer, scratchBufferSize, FFX_FSR3UPSCALER_CONTEXT_COUNT) != FFX_OK)
		logger::critical("[FidelityFX] Failed to initialize FSR3 backend interface!");

	FfxFsr3ContextDescription contextDescription;
	contextDescription.maxRenderSize.width = (uint)state->screenSize.x;
	contextDescription.maxRenderSize.height = (uint)state->screenSize.y;
	contextDescription.maxUpscaleSize.width = (uint)state->screenSize.x;
	contextDescription.maxUpscaleSize.height = (uint)state->screenSize.y;
	contextDescription.displaySize.width = (uint)state->screenSize.x;
	contextDescription.displaySize.height = (uint)state->screenSize.y;
	contextDescription.flags = FFX_FSR3_ENABLE_UPSCALING_ONLY | FFX_FSR3_ENABLE_AUTO_EXPOSURE;
	contextDescription.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;

	contextDescription.backendInterfaceUpscaling = fsrInterface;

	if (ffxFsr3ContextCreate(&fsrContext, &contextDescription) != FFX_OK)
		logger::critical("[FidelityFX] Failed to initialize FSR3 context!");
}

void FidelityFX::DestroyFSRResources()
{
	if (ffxFsr3ContextDestroy(&fsrContext) != FFX_OK)
		logger::critical("[FidelityFX] Failed to destroy FSR3 context!");
}

// https://github.com/PureDark/Skyrim-Upscaler/blob/fa057bb088cf399e1112c1eaba714590c881e462/src/SkyrimUpscaler.cpp#L88
float GetVerticalFOVRad()
{
	static float& fac = (*(float*)(RELOCATION_ID(513786, 388785).address()));
	const auto base = fac;
	const auto x = base / 1.30322540f;
	auto state = State::GetSingleton();
	const auto vFOV = 2 * atan(x / (state->screenSize.x / state->screenSize.y));
	return vFOV;
}

void FidelityFX::Upscale(Texture2D* a_color)
{
	auto state = State::GetSingleton();

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto& depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto& motionVectorsTexture = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	{
		FfxFsr3DispatchUpscaleDescription dispatchParameters{};

		dispatchParameters.commandList = ffxGetCommandListDX11(state->context);
		dispatchParameters.color = ffxGetResource(a_color->resource.get(), L"FSR3_Input_OutputColor", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.depth = ffxGetResource(depthTexture.texture, L"FSR3_InputDepth", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.motionVectors = ffxGetResource(motionVectorsTexture.texture, L"FSR3_InputMotionVectors", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.exposure = ffxGetResource(nullptr, L"FSR3_InputExposure", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.upscaleOutput = dispatchParameters.color;
		dispatchParameters.reactive = ffxGetResource(nullptr, L"FSR3_InputReactiveMap", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.transparencyAndComposition = ffxGetResource(nullptr, L"FSR3_TransparencyAndCompositionMap", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);

		dispatchParameters.motionVectorScale.x = state->isVR ? state->screenSize.x / 2 : state->screenSize.x;
		dispatchParameters.motionVectorScale.y = state->screenSize.y;
		dispatchParameters.renderSize.width = (uint)state->screenSize.x;
		dispatchParameters.renderSize.height = (uint)state->screenSize.y;

		static auto gameViewport = RE::BSGraphics::State::GetSingleton();
		dispatchParameters.jitterOffset.x = gameViewport->projectionPosScaleX;
		dispatchParameters.jitterOffset.y = gameViewport->projectionPosScaleY;

		static float& deltaTime = (*(float*)REL::RelocationID(523660, 410199).address());
		dispatchParameters.frameTimeDelta = deltaTime * 1000.f;

		static float& cameraNear = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x40));
		static float& cameraFar = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x44));
		dispatchParameters.cameraFar = cameraFar;
		dispatchParameters.cameraNear = cameraNear;

		dispatchParameters.enableSharpening = true;
		dispatchParameters.sharpness = Upscaling::GetSingleton()->settings.sharpness;

		dispatchParameters.cameraFovAngleVertical = GetVerticalFOVRad();
		dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;
		dispatchParameters.reset = false;
		dispatchParameters.preExposure = 1.0f;

		dispatchParameters.flags = 0;

		FfxErrorCode errorCode = ffxFsr3ContextDispatchUpscale(&fsrContext, &dispatchParameters);
		if (errorCode != FFX_OK) {
			logger::error("[FidelityFX] Failed to dispatch upscaling!");
		}
	}
}
