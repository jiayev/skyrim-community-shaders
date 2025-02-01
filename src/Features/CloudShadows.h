#pragma once

#include "Buffer.h"
#include "Feature.h"

struct CloudShadows : Feature
{
	static CloudShadows* GetSingleton()
	{
		static CloudShadows singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Cloud Shadows"; }
	virtual inline std::string GetShortName() override { return "CloudShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "CLOUD_SHADOWS"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	bool overrideSky = false;
	void SkyShaderHacks();

	Texture2D* texCubemapCloudOcc = nullptr;
	ID3D11RenderTargetView* cubemapCloudOccRTVs[6] = { nullptr };
	ID3D11BlendState* cloudShadowBlendState = nullptr;

	virtual void SetupResources() override;

	void CheckResourcesSide(int side);
	void ModifySky(RE::BSRenderPass* Pass);

	virtual void EarlyPrepass() override;

	virtual inline void PostPostLoad() override { Hooks::Install(); }

	struct Hooks
	{
		struct BSSkyShader_SetupMaterial
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSSkyShader_SetupMaterial>(RE::VTABLE_BSSkyShader[0]);
			logger::info("[Cloud Shadows] Installed hooks");
		}
	};
	virtual bool SupportsVR() override { return true; };
};
