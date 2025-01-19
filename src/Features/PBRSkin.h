#pragma once

#include "Buffer.h"
#include "State.h"
#include "ShaderCache.h"
#include "Feature.h"

struct PBRSkin : Feature
{
    static PBRSkin* GetSingleton()
    {
        static PBRSkin singleton;
        return &singleton;
    }

    virtual inline std::string GetName() override { return "PBR Skin"; }
    virtual inline std::string GetShortName() override { return "PBRSkin"; }
    // virtual inline std::string_view GetShaderDefineName() override { return "PBR_SKIN"; }
    virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::Lighting;
	};

    virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

    struct Settings
    {
        bool EnablePBRSkin = false;
		bool EnablePBRHair = false;
		float SkinRoughnessScale = 0.6f;
		float SkinSpecularLevel = 0.0277f;
		float SkinSpecularTexMultiplier = 3.14f;
		float HairRoughnessScale = 0.5f;
		float HairSpecularLevel = 0.045f;
        float HairSpecularTexMultiplier = 1.0f;
    } settings;

    struct alignas(16) PBRSkinData
    {
        float4 skinParams;
        float4 hairParams;
    };

    PBRSkinData GetCommonBufferData();

    struct BSLightingShader_SetupGeometry
    {
        static void thunk(RE::BSLightingShader* shader, RE::BSRenderPass* pass, uint32_t renderFlags)
        {
            const auto originalTechnique = shader->currentRawTechnique;
            if (GetSingleton()->settings.EnablePBRSkin)
            {
                shader->currentRawTechnique |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::PbrSkin);
            }
            if (GetSingleton()->settings.EnablePBRHair)
            {
                shader->currentRawTechnique |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::PbrHair);
            }
            func(shader, pass, renderFlags);
            shader->currentRawTechnique = originalTechnique;
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    virtual void PostPostLoad() override
    {
        logger::info("[PBR Skin] PostPostLoad hooking");
        stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
    }
};