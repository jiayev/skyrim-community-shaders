#pragma once

#include "Buffer.h"
#include "State.h"

#define NV_WINDOWS
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

	inline std::string GetShortName() { return "Streamline"; }

	enum class AAMode
	{
		kTAA,
		kDLAA
	};

	struct Settings
	{
		uint aaMode = (uint)AAMode::kDLAA;
		uint dlaaPreset = (uint)sl::DLSSPreset::ePresetC;
		float sharpness = 0.5f;
	};

	Settings settings;

	bool enabledAtBoot = false;
	bool initialized = false;

	bool featureDLSS = false;
	bool featureDLSSG = false;
	bool featureReflex = false;

	sl::ViewportHandle viewport{ 0 };
	sl::FrameToken* frameToken;

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

	// DLSS specific functions
	PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings{};
	PFun_slDLSSGetState* slDLSSGetState{};
	PFun_slDLSSSetOptions* slDLSSSetOptions{};

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
	ID3D11ComputeShader* rcasCS;

	ID3D11ComputeShader* GetRCASComputeShader();
	void ClearShaderCache();

	void SaveSettings(json&);
	void LoadSettings(json&);
	void RestoreDefaultSettings();
	void DrawSettings();

	void LoadInterposer();
	void Initialize();
	void PostDevice();

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
		ID3D11DeviceContext** ppImmediateContext);

	void SetupResources();

	void CopyResourcesToSharedBuffers();

	void Present();
	void Upscale();

	void UpdateConstants();

	struct Main_RenderWorld
	{
		static void thunk(bool a1)
		{
			GetSingleton()->UpdateConstants();
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

	bool validTaaPass = false;

	struct TAA_BeginTechnique
	{
		static void thunk(RE::BSImagespaceShaderISTemporalAA* a_shader, RE::BSTriShape* a_null)
		{
			func(a_shader, a_null);
			GetSingleton()->validTaaPass = true;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct TAA_EndTechnique
	{
		static void thunk(RE::BSImagespaceShaderISTemporalAA* a_shader, RE::BSTriShape* a_null)
		{
			auto singleton = GetSingleton();
			if (singleton->featureDLSS && singleton->settings.aaMode == (uint)AAMode::kDLAA && singleton->validTaaPass)
				singleton->Upscale();
			else
				func(a_shader, a_null);
			singleton->validTaaPass = false;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void InstallHooks()
	{
		if (!REL::Module::VR()) {
			stl::write_thunk_call<Main_RenderWorld>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x831, 0x841, 0x791));
			stl::write_thunk_call<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084).address() + REL::Relocate(0x7E, 0x83, 0x97));
			stl::write_thunk_call<TAA_BeginTechnique>(REL::RelocationID(100540, 107270).address() + REL::Relocate(0x3E9, 0x3EA, 0x448));
			stl::write_thunk_call<TAA_EndTechnique>(REL::RelocationID(100540, 107270).address() + REL::Relocate(0x3F3, 0x3F4, 0x452));
		}
	}
};
