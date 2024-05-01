#pragma once

#include "Buffer.h"
#include "Deferred.h"
#include "Feature.h"

struct TerrainBlending : Feature
{
public:
	static TerrainBlending* GetSingleton()
	{
		static TerrainBlending singleton;
		return &singleton;
	}

	static void InstallHooks()
	{
		Hooks::Install();
	}

	bool enableBlending = true;
	int optimisationDistance = 4096;

	virtual inline std::string GetName() { return "Terrain Blending"; }
	virtual inline std::string GetShortName() { return "TerrainBlending"; }
	virtual inline std::string_view GetShaderDefineName() { return "TERRAIN_BLENDING"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type) { return true; }

	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();

	bool ValidBlendingPass(RE::BSRenderPass* a_pass);

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void RestoreDefaultSettings();
	virtual void PostPostLoad() override;

	void SetupGeometry(RE::BSRenderPass* a_pass);

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
			{
				func(This, a_pass, a_renderFlags);
				GetSingleton()->SetupGeometry(a_pass);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSGrassShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
			{
				func(This, a_pass, a_renderFlags);
				//	Deferred::GetSingleton()->SetOverwriteTerrainMode(false);
				//	Deferred::GetSingleton()->SetOverwriteTerrainMaskingMode(Deferred::TerrainMaskMode::kNone);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			stl::write_vfunc<0x6, BSGrassShader_SetupGeometry>(RE::VTABLE_BSGrassShader[0]);

			logger::info("[Terrain Blending] Installed hooks");
		}
	};

	bool SupportsVR() override { return true; };
};
