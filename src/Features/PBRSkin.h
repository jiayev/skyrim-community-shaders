#pragma once

#include "Buffer.h"
#include "State.h"
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
    virtual inline std::string_view GetShaderDefineName() override { return "PBR_SKIN"; }
    virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::Lighting;
	};

    virtual inline bool SupportsVR() { return true; }

    virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

    struct Settings
    {
        bool EnablePBRSkin = true;
		float SkinRoughnessScale = 0.6f;
		float SkinSpecularLevel = 0.0277f;
		float SkinSpecularTexMultiplier = 3.14f;
    } settings;

    struct alignas(16) PBRSkinData
    {
        float4 skinParams;
    };

    PBRSkinData GetCommonBufferData();
};