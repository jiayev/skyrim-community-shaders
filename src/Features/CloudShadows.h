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

	struct Settings
	{
		uint EnableCloudShadows = true;

		float CloudHeight = 2e3f / 1.428e-2f;
		float PlanetRadius = 6371e3f / 1.428e-2f;

		float EffectMix = 1.f;

		float TransparencyPower = 0.1f;
		float AbsorptionAmbient = 0.2f;
	} settings;

	struct alignas(16) PerPass
	{
		Settings Settings;

		float RcpHPlusR;

		float padding;
	};
	std::unique_ptr<Buffer> perPass = nullptr;

	bool isCubemapPass = false;
	ID3D11BlendState* resetBlendState = nullptr;
	std::set<ID3D11BlendState*> mappedBlendStates;
	std::map<ID3D11BlendState*, ID3D11BlendState*> modifiedBlendStates;

	Texture2D* texCubemapCloudOcc = nullptr;
	ID3D11RenderTargetView* cubemapCloudOccRTVs[6] = { nullptr };
	ID3D11ShaderResourceView* cubemapCloudOccDebugSRV = nullptr;

	virtual void SetupResources() override;
	virtual inline void Reset() override {}

	virtual void DrawSettings() override;

	void CheckResourcesSide(int side);
	void ModifySky(const RE::BSShader* shader, const uint32_t descriptor);
	void ModifyLighting();
	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor) override;

	virtual void Load(json& o_json) override;
	virtual void Save(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual inline void PostPostLoad() override { Hooks::Install(); }

	struct Hooks
	{
		struct BSBatchRenderer__RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* Pass, uint32_t Technique, bool AlphaTest, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_thunk_call<BSBatchRenderer__RenderPassImmediately>(REL::RelocationID(100877, 107630).address() + REL::Relocate(0x1E5, 0xFD));  // need SE addr
		}
	};
};
