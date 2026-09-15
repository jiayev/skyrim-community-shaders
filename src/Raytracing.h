#pragma once

#include "Buffer.h"
#include "CreationEngineRaytracing.h"
#include "Feature.h"
#include "FeatureCategories.h"
#include "Globals.h"
#include <d3d11.h>
#include <memory>
#include <winrt/base.h>

struct uint2
{
	uint32_t x = 0;
	uint32_t y = 0;

	bool operator==(const uint2&) const = default;
	bool operator!=(const uint2&) const = default;
};

/**
 * @brief Core feature integrating CreationEngineRaytracing hardware-accelerated ray tracing.
 */
struct Raytracing : Feature
{
public:
	// Metadata
	virtual std::string GetName() override { return "Raytracing"; }
	virtual std::string GetDisplayName() override { return T("feature.raytracing.name", "Raytracing"); }
	virtual std::string GetShortName() override { return "Raytracing"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual bool IsCore() const override { return true; }
	virtual bool IsInMenu() const override { return true; }
	virtual bool DrawFailLoadMessage() const override { return false; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return std::make_pair(
			"Raytracing integrates hardware-accelerated ray tracing via Creation Engine Raytracing (CERT).",
			std::vector<std::string>{
				"Hardware ray tracing pipeline integration",
				"Support for Global Illumination and Path Tracing modes"
			});
	}

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	// Lifecycle
	virtual void Load() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;

	void SetupResources();

	bool Available(bool a_initialized = true) const;
	bool InitializeCERaytracing();
	bool UpdateResolution();
	void UpdateSettings();
	CreationEngineRaytracing::Settings GetSettings() const;

	void Execute();

	CreationEngineRaytracing::Mode Mode() const;
	bool IsPathTracing() const;
	bool IsPathTracingCull() const;
	void UpdateJitter(float2 a_jitter);

	void GetRayReconstructionInputs(ID3D11Resource*& diffuseAlbedo, ID3D11Resource*& specularAlbedo,
		ID3D11Resource*& normalRoughness, ID3D11Resource*& specHitDist);

	static constexpr uint32_t SKY_CUBEMAP_SIZE = 256;
	static constexpr uint32_t SKY_HEMI_SIZE = SKY_CUBEMAP_SIZE * 2;
	static constexpr uint32_t WATER_FLOWMAP_SIZE = 320;

	void SetupSkyHemisphere();
	void SetupWaterFlowMap();
	void SetupSharedTextures();
	void SkyCubeToHemi() const;
	void CopyWaterFlowMap() const;
	void CompileShaders();

	enum struct DisableReason
	{
		None,
		UnsupportedGPU,
		OutdatedDrivers,
		MissingPlugin,
		InitFailed,
	};

	struct Settings
	{
		CreationEngineRaytracing::Settings CreationEngineRaytracingSettings;
		CreationEngineRaytracing::RendererSettings RendererSettings;

		bool operator==(const Settings&) const = default;
	} settings;

	bool initialized = false;
	bool forcedDisabled = false;
	DisableReason disableReason = DisableReason::None;

	uint2 m_Resolution{ 0, 0 };

	std::unique_ptr<CreationEngineRaytracing> creationEngineRaytracing = nullptr;

	// Sky Hemisphere
	winrt::com_ptr<ID3D11Texture2D> skyHemisphere = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> skyHemisphereSRV = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> skyHemisphereUAV = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> cubeToHemiCS = nullptr;
	winrt::com_ptr<ID3D11SamplerState> samplerState = nullptr;
	RE::NiPointer<RE::TESWaterReflections> waterReflections = nullptr;

	// Water Flow Map
	winrt::com_ptr<ID3D11Texture2D> waterFlowMap = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> waterFlowMapSRV = nullptr;
	winrt::com_ptr<ID3D11RenderTargetView> waterFlowMapRTV = nullptr;

	// Shared Textures (Normal + Roughness)
	winrt::com_ptr<ID3D11Texture2D> normalRoughnessTexture = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> normalRoughnessSRV = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> normalRoughnessUAV = nullptr;

	struct SharedTextureWrapper
	{
		CreationEngineRaytracing::SharedTexture texture{};
		winrt::com_ptr<ID3D11ShaderResourceView> srv = nullptr;
	};

	SharedTextureWrapper sharedDepthTextures[CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT]{};
	SharedTextureWrapper sharedMotionVectorTextures[CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT]{};
	SharedTextureWrapper sharedMainTextures[CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT]{};

	struct alignas(16) ScreenData
	{
		uint2 Resolution;
		uint2 DynamicResolution;
	};
	static_assert(sizeof(ScreenData) % 16 == 0);

	std::unique_ptr<ConstantBuffer> screenCB = nullptr;
	std::unique_ptr<ScreenData> screenData = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> ptCompositeCS = nullptr;
	winrt::com_ptr<ID3D11VertexShader> copyDepthVS = nullptr;
	winrt::com_ptr<ID3D11PixelShader> copyDepthPS = nullptr;
	winrt::com_ptr<ID3D11BlendState> copyBlendState = nullptr;
	winrt::com_ptr<ID3D11RasterizerState> copyRasterizerState = nullptr;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState = nullptr;

	void UpdateFeatureData();

	std::unique_ptr<CreationEngineRaytracing::FeatureData> featureData = nullptr;

	struct Hooks
	{
		struct Main_RenderWorld
		{
			static void thunk(bool a1)
			{
				auto& rt = globals::features::raytracing;
				if (rt.Available() && rt.Mode() != CreationEngineRaytracing::Mode::None) {
					rt.UpdateFeatureData();
					rt.SkyCubeToHemi();
					rt.creationEngineRaytracing->UpdateCamera();

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
			}

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

						rt.waterReflections->flags.set(true, RE::TESWaterReflections::Flags::kDirty);
					}
				}

				func();
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

				rt.CopyWaterFlowMap();
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::detour_thunk<Main_RenderWorld>(REL::RelocationID(100424, 107142));
			if (!REL::Module::IsVR()) {
				stl::detour_thunk<Main_RenderWaterEffects>(REL::RelocationID(35561, 36560));
				stl::write_thunk_call<CopyToWaterFlowmap>(REL::RelocationID(35561, 36560).address() + REL::Relocate(0x202, 0x242));
			}
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