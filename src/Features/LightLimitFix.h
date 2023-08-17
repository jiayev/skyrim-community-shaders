#pragma once
#include <DirectXMath.h>
#include <d3d11.h>

#include "Buffer.h"
#include <shared_mutex>

#include "Feature.h"
#include "ShaderCache.h"

struct LightLimitFix : Feature
{
public:
	static LightLimitFix* GetSingleton()
	{
		static LightLimitFix render;
		return &render;
	}

	static void InstallHooks()
	{
		Hooks::Install();
	}
	virtual inline std::string GetName() { return "Light Limit Fix"; }
	virtual inline std::string GetShortName() { return "LightLimitFix"; }

	struct LightData
	{
		float3 color;
		float radius;
		float3 positionWS;
		float3 positionVS;
		uint firstPerson;
		uint pad;
	};

	struct ClusterAABB
	{
		DirectX::XMVECTOR minPoint;
		DirectX::XMVECTOR maxPoint;
	};

	struct LightGrid
	{
		std::uint32_t offset;
		std::uint32_t lightCount;
	};

	struct alignas(16) PerFrameLightCulling
	{
		float4x4 InvProjMatrix;
		float4x4 ViewMatrix;
		float LightsNear;
		float LightsFar;
		float pad[2];
	};

	struct PerPass
	{
		uint EnableGlobalLights;
		float LightsNear;
		float LightsFar;
		float4 CameraData;
		float2 BufferDim;
		uint FrameCount;
		uint EnableContactShadows;
	};

	struct CachedParticleLight
	{
		RE::NiColor color;
		RE::NiPoint3 position;
		float radius;
	};

	std::unique_ptr<Buffer> perPass = nullptr;

	bool rendered = false;

	ID3D11ComputeShader* clusterBuildingCS = nullptr;
	ID3D11ComputeShader* clusterCullingCS = nullptr;

	ConstantBuffer* perFrameLightCulling = nullptr;

	eastl::unique_ptr<Buffer> lights = nullptr;
	eastl::unique_ptr<Buffer> clusters = nullptr;
	eastl::unique_ptr<Buffer> lightCounter = nullptr;
	eastl::unique_ptr<Buffer> lightList = nullptr;
	eastl::unique_ptr<Buffer> lightGrid = nullptr;
	eastl::unique_ptr<Buffer> strictLights = nullptr;

	std::uint32_t lightCount = 0;

	RE::BSRenderPass* currentPass = nullptr;

	eastl::hash_map<RE::BSGeometry*, RE::NiColor> queuedParticleLights;
	eastl::hash_map<RE::BSGeometry*, RE::NiColor> particleLights;

	std::uint32_t strictLightsCount = 0;
	eastl::vector<LightData> strictLightsData;

	virtual void SetupResources();
	virtual void Reset();

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void DrawSettings();
	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	void UpdateLights();
	void Bind();

	static inline bool IsValidLight(RE::BSLight* a_light);
	static inline bool IsGlobalLight(RE::BSLight* a_light);

	struct Settings
	{
		bool EnableContactShadows = true;
		bool ExtendFirstPersonShadows = true;
		bool EnableParticleLights = true;
		bool EnableParticleLightsCulling = true;
		bool EnableParticleLightsFade = true;
		bool EnableParticleLightsDetection = true;
		float ParticleLightsBrightness = 1.0f;
		float ParticleLightsRadius = 100.0f;
		float ParticleLightsRadiusBillboards = 0.5f;
		bool EnableParticleLightsOptimization = true;
		float ParticleLightsOptimisationClusterRadius = 32.0f;
	};

	float lightsNear = 0.0f;
	float lightsFar = 16384.0f;

	Settings settings;

	bool CheckParticleLights(RE::BSRenderPass* a_pass, uint32_t a_technique);
	void BSEffectShader_SetupGeometry(RE::BSRenderPass* a_pass);

	void BSLightingShader_SetupGeometry_Before(RE::BSRenderPass* a_pass);
	void BSLightingShader_SetupGeometry_After(RE::BSRenderPass* a_pass);

	void BSEffectShader_SetupGeometry_Before(RE::BSRenderPass* a_pass);

	void BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights(RE::BSRenderPass* Pass, uint32_t LightCount, uint32_t ShadowLightCount);

	enum class Space
	{
		World = 0,
		Model = 1,
	};

	std::shared_mutex cachedParticleLightsMutex;
	eastl::vector<CachedParticleLight> cachedParticleLights;
	uint32_t particleLightsDetectionHits = 0;

	float CalculateLuminance(CachedParticleLight& light, RE::NiPoint3& point);

	void AddParticleLightLuminance(RE::NiPoint3& targetPosition, int& numHits, float& lightLevel);

	struct Hooks
	{
		struct ValidLight1
		{
			static bool thunk(RE::BSShaderProperty* a_property, RE::BSLight* a_light)
			{
				return func(a_property, a_light) && (a_light->portalStrict || !a_light->portalGraph || skyrim_cast<RE::BSShadowLight*>(a_light));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ValidLight2
		{
			static bool thunk(RE::BSShaderProperty* a_property, RE::BSLight* a_light)
			{
				return func(a_property, a_light) && (a_light->portalStrict || !a_light->portalGraph || skyrim_cast<RE::BSShadowLight*>(a_light));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ValidLight3
		{
			static bool thunk(RE::BSShaderProperty* a_property, RE::BSLight* a_light)
			{
				return func(a_property, a_light) && (a_light->portalStrict || !a_light->portalGraph || skyrim_cast<RE::BSShadowLight*>(a_light));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSBatchRenderer__RenderPassImmediately1
		{
			static void thunk(RE::BSRenderPass* Pass, uint32_t Technique, bool AlphaTest, uint32_t RenderFlags)
			{
				if (!GetSingleton()->CheckParticleLights(Pass, Technique))
					func(Pass, Technique, AlphaTest, RenderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSBatchRenderer__RenderPassImmediately2
		{
			static void thunk(RE::BSRenderPass* Pass, uint32_t Technique, bool AlphaTest, uint32_t RenderFlags)
			{
				if (!GetSingleton()->CheckParticleLights(Pass, Technique))
					func(Pass, Technique, AlphaTest, RenderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSBatchRenderer__RenderPassImmediately3
		{
			static void thunk(RE::BSRenderPass* Pass, uint32_t Technique, bool AlphaTest, uint32_t RenderFlags)
			{
				if (!GetSingleton()->CheckParticleLights(Pass, Technique))
					func(Pass, Technique, AlphaTest, RenderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				GetSingleton()->BSLightingShader_SetupGeometry_Before(Pass);
				func(This, Pass, RenderFlags);
				//GetSingleton()->BSLightingShader_SetupGeometry_After(Pass);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights
		{
			static void thunk(RE::BSGraphics::PixelShader* PixelShader, RE::BSRenderPass* Pass, DirectX::XMMATRIX& Transform, uint32_t LightCount, uint32_t ShadowLightCount, float WorldScale, Space RenderSpace)
			{
				GetSingleton()->BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights(Pass, LightCount, ShadowLightCount);
				func(PixelShader, Pass, Transform, LightCount, ShadowLightCount, WorldScale, RenderSpace);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSEffectShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				GetSingleton()->BSEffectShader_SetupGeometry_Before(Pass);
				func(This, Pass, RenderFlags);
				GetSingleton()->BSLightingShader_SetupGeometry_After(Pass);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AIProcess_CalculateLightValue_GetLuminance
		{
			static void thunk(RE::ShadowSceneNode* shadowSceneNode, RE::NiPoint3& targetPosition, int& numHits, float& sunLightLevel, float& lightLevel, RE::NiLight& refLight, int32_t shadowBitMask)
			{
				func(shadowSceneNode, targetPosition, numHits, sunLightLevel, lightLevel, refLight, shadowBitMask);
				GetSingleton()->AddParticleLightLuminance(targetPosition, numHits, lightLevel);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_thunk_call<ValidLight1>(REL::RelocationID(100994, 107781).address() + 0x92);
			stl::write_thunk_call<ValidLight2>(REL::RelocationID(100997, 107784).address() + REL::Relocate(0x139, 0x12A));
			stl::write_thunk_call<ValidLight3>(REL::RelocationID(101296, 108283).address() + REL::Relocate(0xB7, 0x7E));

			stl::write_thunk_call<BSBatchRenderer__RenderPassImmediately1>(REL::RelocationID(100877, 107673).address() + REL::Relocate(0x1E5, 0x1EE));
			stl::write_thunk_call<BSBatchRenderer__RenderPassImmediately2>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
			stl::write_thunk_call<BSBatchRenderer__RenderPassImmediately3>(REL::RelocationID(100871, 107667).address() + REL::Relocate(0xEE, 0xED));

			stl::write_thunk_call<AIProcess_CalculateLightValue_GetLuminance>(REL::RelocationID(38900, 39946).address() + REL::Relocate(0x1C9, 0x1D3));

			//stl::write_thunk_call<BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights>(REL::RelocationID(100565, 107300).address() + REL::Relocate(0x523, 0xB0E));
			//stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);

			//stl::write_vfunc<0x6, BSEffectShader_SetupGeometry>(RE::VTABLE_BSEffectShader[0]);
		}
	};
};
