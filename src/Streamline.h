#pragma once

#include "Buffer.h"
#include "State.h"

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_reflex.h>

class Streamline
{
public:
	static Streamline* GetSingleton()
	{
		static Streamline singleton;
		return &singleton;
	}

	bool initialized = false;
	bool streamlineActive = false;

	sl::ViewportHandle viewport;
	sl::FrameToken* currentFrame;

	sl::DLSSGMode frameGenerationMode = sl::DLSSGMode::eAuto;

	HMODULE interposer = NULL;

	// SL Interposer Functions
	PFun_slInit* slInit{};
	PFun_slShutdown* slShutdown{};
	PFun_slIsFeatureSupported* slIsFeatureSupported{};
	PFun_slIsFeatureLoaded* slIsFeatureLoaded{};
	PFun_slSetFeatureLoaded* slSetFeatureLoaded{};
	PFun_slEvaluateFeature* slEvaluateFeature{};
	PFun_slAllocateResources* slAllocateResources{};
	PFun_slFreeResources* slFreeResources{};
	PFun_slSetTag* slSetTag{};
	PFun_slGetFeatureRequirements* slGetFeatureRequirements{};
	PFun_slGetFeatureVersion* slGetFeatureVersion{};
	PFun_slUpgradeInterface* slUpgradeInterface{};
	PFun_slSetConstants* slSetConstants{};
	PFun_slGetNativeInterface* slGetNativeInterface{};
	PFun_slGetFeatureFunction* slGetFeatureFunction{};
	PFun_slGetNewFrameToken* slGetNewFrameToken{};
	PFun_slSetD3DDevice* slSetD3DDevice{};

	// DLSSG specific functions
	PFun_slDLSSGGetState* slDLSSGGetState{};
	PFun_slDLSSGSetOptions* slDLSSGSetOptions{};

	// Reflex specific functions
	PFun_slReflexGetState* slReflexGetState{};
	PFun_slReflexSetMarker* slReflexSetMarker{};
	PFun_slReflexSleep* slReflexSleep{};
	PFun_slReflexSetOptions* slReflexSetOptions{};

	Texture2D* colorBufferShared;
	Texture2D* depthBufferShared;

	ID3D11ComputeShader* copyDepthToSharedBufferCS;

	void DrawSettings();

	void Shutdown();

	void Initialize_preDevice();
	void Initialize_postDevice();

	HRESULT CreateDXGIFactory(REFIID riid, void** ppFactory);

	HRESULT CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter,
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
		bool& o_streamlineProxy);

	void CreateFrameGenerationResources();

	void SetupFrameGeneration();

	void CopyResourcesToSharedBuffers();

	void Present();

	void SetConstants();

	struct Main_RenderWorld
	{
		static void thunk(bool a1)
		{
			GetSingleton()->SetConstants();
			func(a1);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1)
		{
			GetSingleton()->CopyResourcesToSharedBuffers();
			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void InstallHooks()
	{
		stl::write_thunk_call<Main_RenderWorld>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x831, 0x841, 0x791));
		stl::write_thunk_call<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084).address() + REL::Relocate(0x7E, 0x83, 0x97));
	}
};
