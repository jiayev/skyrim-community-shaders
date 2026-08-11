#pragma once

#include <array>

#include <d3d11_4.h>
#include <directx/d3d12.h>

#include "OverlayFeature.h"

#include "Features/Upscaling/DX12SwapChain.h"

#include "Util.h"

#define NTDDI_VERSION NTDDI_WINBLUE

#include <DXProgrammableCapture.h>

#include "Features/CloudShadows.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/HairSpecular.h"
#include "Features/LODBlending.h"
#include "Features/LinearLighting.h"
#include "Features/PhysicalSky.h"
#include "Features/Skin.h"
#include "Features/Upscaling.h"
#include "Features/WetnessEffects.h"

#include "CreationEngineRaytracing.h"

struct uint2
{
	uint x;
	uint y;

	bool operator==(const uint2&) const = default;
	bool operator!=(const uint2&) const = default;
};
static_assert(sizeof(uint2) == 8);

struct Raytracing : public OverlayFeature
{
	static constexpr uint SKY_CUBEMAP_SIZE = 256;
	static constexpr uint SKY_HEMI_SIZE = SKY_CUBEMAP_SIZE * 2;

	static constexpr uint WATER_FLOWMAP_SIZE = 320;

	// Metadata
	virtual inline std::string GetName() override { return "Raytracing"; }
	virtual inline std::string GetShortName() override { return "Raytracing"; }
	virtual inline std::string_view GetCategory() const override { return "Lighting"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL("999999"); }
	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"This is a terse description.",
			{
				"This is a subfeature.",
				"This is another subfeature.",
				"Cheese.",
			}
		};
	}

	// Functionality
	virtual inline std::string_view GetShaderDefineName() override { return "RAYTRACING"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::Lighting || t == RE::BSShader::Type::Grass || t == RE::BSShader::Type::Sky;
	};

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	virtual bool IsInMenu() const override { return true; }

	virtual bool IsOverlayVisible() const override { return Available() && settings.PerfOverlay != OverlayMode::None; };

	virtual void DrawOverlay() override;

	virtual std::vector<FeatureConstraints::Constraint> GetActiveConstraints() const override
	{
		if (!loaded)
			return {};

		return {
			{ { "UnifiedWater", "Enabled" },
				false,
				"Unified Water alters the behaviour of water meshes which breaks raytracing scene management",
				true }
		};
	}

	bool Available(bool a_initialized = true) const;

	// Resources
	virtual void SetupResources() override;

	void Load() override;
	void PostPostLoad() override;
	void DataLoaded() override;

	void DrawGeneralSettings();
	void DrawNRDSettings();
	void DrawReblurSettings();
	void DrawRelaxSettings();
	void DrawSHaRCSettings();
	void DrawSSSSettings();
	void DrawAdvancedSettings();
	void DrawReSTIRGISettings();
	void DrawReSTIRPTSettings();
	void DrawExperimentalSettings();
	void DrawDebugSettings();

	void CompileShaders();

	uint64_t GetSharedVRAMOffset() const
	{
		return loaded && initialized ? creationEngineRaytracing->GetFakeDoubledVRAMUsage() : 0;
	}

	void InitializeCERaytracing(ID3D11Device5* d3d11Device, ID3D12Device5* d3d12Device, ID3D12CommandQueue* commandQueue, ID3D12CommandQueue* computeCommandQueue, ID3D12CommandQueue* copyCommandQueue);
	bool UpdateResolution();
	void UpdateJitter(float2 jitter);
	void UpdateFeatureData();
	void UpdateSkinDetailNormal(ID3D11Texture2D* skinDetailTexture);
	void SkyCubeToHemi() const;
	void CopyWaterFlowap() const;
	void ConvertTextures();
	void DeferredPasses();
	void GetRayReconstructionInputs(ID3D12Resource*& diffuseAlbedo, ID3D12Resource*& specularAlbedo, ID3D12Resource*& normalRoughness, ID3D12Resource*& specHitDist);

	CreationEngineRaytracing::Settings GetSettings() const;

	void UpdateSettings();

	inline CreationEngineRaytracing::Mode Mode() const
	{
		return Available() ? settings.CreationEngineRaytracingSettings.GeneralSettings.Mode : CreationEngineRaytracing::Mode::None;
	}

	inline bool IsPathTracing() const
	{
		return Mode() == CreationEngineRaytracing::Mode::PathTracing;
	}

	inline bool IsPathTracingCull() const
	{
		return Mode() == CreationEngineRaytracing::Mode::PathTracing && settings.CreationEngineRaytracingSettings.ExperimentalSettings.PathTracingCull != CreationEngineRaytracing::PTCullMode::Disabled;
	}

	inline uint32_t GetDiffuseAlbedoIndex() const
	{
		// Use only the first texture since the only use will be as DLSS RR input
		if (Mode() == CreationEngineRaytracing::Mode::GlobalIllumination)
			return 0;

		return currentFrame;
	}

	enum struct OverlayMode
	{
		None,
		Simple,
		Complete,
		Extended
	};

	////////////////////////////////////////////////// Feature Specific Data
	struct Settings
	{
		OverlayMode PerfOverlay = OverlayMode::None;
		bool DisplaySceneGraphCounters = false;
		bool DisableVanillaFogPT = false;
		CreationEngineRaytracing::Settings CreationEngineRaytracingSettings;
	} settings;

	ImVec2 Position = ImVec2(10.f, 10.f);
	bool PositionSet = false;

	bool initialized = false;

	// Used when feature was force disabled, likely due to missing dll or incompatible hardware
	bool forcedDisabled = false;

	uint2 m_Resolution;

	bool fogEnabled = false;
	bool ssaoEnabled = false;

	enum DisableReason
	{
		None,
		UnsupportedGPU,
		OutdatedDrivers,
		MissingPlugin,
		InitFailed,
	} disableReason = DisableReason::None;

	struct alignas(16) FeatureData
	{
		ExtendedMaterials::Settings ExtendedMaterials;
		WetnessEffects::PerFrame WetnessEffects;
		CloudShadows::Settings CloudShadows;
		HairSpecular::Settings HairSpecular;
		ExtendedTranslucency::PerFrame ExtendedTranslucency;
		LinearLighting::PerFrameData LinearLighting;
		ExponentialHeightFog::Settings ExponentialHeightFog;
		LODBlending::Settings LODBlending;
		PhysicalSky::CbData PhysicalSky;
		Skin::SkinData Skin;
	};

	eastl::unique_ptr<FeatureData> featureData;

	struct alignas(16) SharedData
	{
		float InteriorDirectional;
		float Ambient;
		float EnvMap;
		uint Albedo;
		uint PathTracing;
		uint _padding0;
		uint _padding1;
		uint _padding2;
	};
	STATIC_ASSERT_ALIGNAS_16(SharedData);

	SharedData GetCommonBufferData() const;

	winrt::com_ptr<ID3D11SamplerState> samplerState = nullptr;

	// Available when Pathtracing
	eastl::array<eastl::unique_ptr<WrappedResource>, CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT> depthTexture;
	eastl::array<eastl::unique_ptr<WrappedResource>, CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT> motionVectorsTexture;

	// Available for both GI and PT
	eastl::array<eastl::unique_ptr<WrappedResource>, CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT> mainTexture;
	eastl::array<eastl::unique_ptr<WrappedResource>, CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT> diffuseAlbedoTexture;

	winrt::com_ptr<ID3D12Resource> albedoTexture = nullptr;
	eastl::unique_ptr<WrappedResource> normalRoughnessTexture = nullptr;
	winrt::com_ptr<ID3D12Resource> gnmaoTexture = nullptr;

	eastl::unique_ptr<WrappedResource> skyHemisphere = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> cubeToHemiCS = nullptr;
	eastl::unique_ptr<WrappedResource> physicalSkyTrLUT = nullptr;

	eastl::unique_ptr<WrappedResource> waterFlowMap = nullptr;
	RE::NiPointer<RE::TESWaterReflections> waterReflections = nullptr;

	eastl::unique_ptr<CreationEngineRaytracing> creationEngineRaytracing = nullptr;

	eastl::vector<CreationEngineRaytracing::PassTiming> passTimings;

	uint32_t currentFrame;

	struct alignas(16) ScreenData
	{
		uint2 Resolution;
		uint2 DynamicResolution;
	};
	static_assert(sizeof(ScreenData) % 16 == 0);

	eastl::unique_ptr<ConstantBuffer> screenCB = nullptr;

	eastl::unique_ptr<ScreenData> screenData;

	winrt::com_ptr<ID3D11ComputeShader> ptCompositeCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> convertTexturesCS[2];
	winrt::com_ptr<ID3D11ComputeShader> giCompositeCS = nullptr;

	winrt::com_ptr<ID3D11VertexShader> copyDepthVS = nullptr;
	winrt::com_ptr<ID3D11PixelShader> copyDepthPS = nullptr;
	winrt::com_ptr<ID3D11BlendState> copyBlendState;
	winrt::com_ptr<ID3D11RasterizerState> copyRasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState = nullptr;

	winrt::com_ptr<ID3D12Resource> skinDetailNormalTexture = nullptr;

	struct Hooks
	{
		struct Main_RenderWorld
		{
			static void thunk(bool a1)
			{
				auto& rt = globals::features::raytracing;

				if (rt.Mode() != CreationEngineRaytracing::Mode::None) {
					rt.UpdateFeatureData();
					rt.SkyCubeToHemi();

					rt.creationEngineRaytracing->UpdateCamera();

					auto dx12Interop = globals::dx12Interop;
					if (dx12Interop->pixCapture && !dx12Interop->pixCaptureStarted) {
						dx12Interop->pixCaptureStarted = true;
						dx12Interop->ga->BeginCapture();
					}

					// Clear render targets
					if (rt.Mode() == CreationEngineRaytracing::Mode::PathTracing || rt.Mode() == CreationEngineRaytracing::Mode::Debug) {
						if (rt.IsPathTracingCull()) {
							auto renderer = globals::game::renderer;
							auto context = globals::d3d::context;

							// Clear Depth
							{
								auto depthStencils = renderer->GetDepthStencilData().depthStencils;
								auto& mainDepth = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
								auto& mainDepthCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
								auto& zPrePassCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

								context->ClearDepthStencilView(mainDepth.views[0], D3D11_CLEAR_DEPTH, 1.0f, 0u);
								context->ClearDepthStencilView(mainDepthCopy.views[0], D3D11_CLEAR_DEPTH, 1.0f, 0u);
								context->ClearDepthStencilView(zPrePassCopy.views[0], D3D11_CLEAR_DEPTH, 1.0f, 0u);
							}

							// Clear Motion Vector
							{
								auto renderTargets = renderer->GetRuntimeData().renderTargets;
								float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
								context->ClearRenderTargetView(renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV, clearColor);
							}
						}
					}
				}

				func(a1);
			};
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderWaterEffects
		{
			static void thunk()
			{
				if (auto& rt = globals::features::raytracing; rt.loaded && !rt.forcedDisabled) {
					auto* tes = RE::TES::GetSingleton();
					if (tes->interiorCell) {
						if (tes->interiorCell->cellFlags.none(RE::TESObjectCELL::Flag::kHasWater))
							tes->interiorCell->cellFlags.set(true, RE::TESObjectCELL::Flag::kHasWater);

						globals::features::raytracing.waterReflections->flags.set(true, RE::TESWaterReflections::Flags::kDirty);
					}
				}

				func();
			};
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSImagespaceShaderRefraction_Render
		{
			static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
			{
				auto& rt = globals::features::raytracing;
				if (rt.Mode() == CreationEngineRaytracing::Mode::PathTracing)
					return;

				func(imageSpaceShader, shape, param);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CopyToWaterFlowmap
		{
			static void thunk(void* a1)
			{
				func(a1);

				auto& rt = globals::features::raytracing;
				if (!rt.initialized || rt.forcedDisabled)
					return;

				rt.CopyWaterFlowap();
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::detour_thunk<Main_RenderWorld>(REL::RelocationID(100424, 107142));
			stl::detour_thunk<Main_RenderWaterEffects>(REL::RelocationID(35561, 36560));
			stl::write_vfunc<0x1, BSImagespaceShaderRefraction_Render>(RE::VTABLE_BSImagespaceShaderRefraction[0]);
			stl::write_thunk_call<CopyToWaterFlowmap>(REL::RelocationID(35561, 36560).address() + REL::Relocate(0x202, 0x242));
		}
	};

	class BGSActorCellEventHandler : public RE::BSTEventSink<RE::BGSActorCellEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*);

		static bool Register()
		{
			static BGSActorCellEventHandler singleton;

			auto* player = RE::PlayerCharacter::GetSingleton();
			player->AsBGSActorCellEventSource()->AddEventSink(&singleton);

			logger::info("Registered {}", typeid(singleton).name());

			return true;
		}
	};
};
