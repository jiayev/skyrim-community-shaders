#include "Skin.h"

#include "Hooks.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    Skin::Settings,
    EnableSkin,
    SkinMainRoughness,
    SkinSecondRoughness,
    SkinSpecularTexMultiplier
)

void Skin::DrawSettings()
{
    ImGui::Checkbox("Enable Skin", &settings.EnableSkin);

    ImGui::Text("Advanced Skin Shader using dual specular lobes.");

    ImGui::Spacing();
    ImGui::SliderFloat("Primary Roughness", &settings.SkinMainRoughness, 0.0f, 1.0f);
    if (auto _tt = Util::HoverTooltipWrapper()) {
        ImGui::Text("Controls microscopic roughness of stratum corneum layer");
        ImGui::BulletText("0.4-0.6 : Normal skin (forehead/cheek)");
        ImGui::BulletText("0.2-0.3 : Oily areas (nose bridge)");
        ImGui::BulletText("0.7+ : Dry skin (elbows)");
    }

    ImGui::SliderFloat("Secondary Roughness", &settings.SkinSecondRoughness, 0.0f, 1.0f);
    if (auto _tt = Util::HoverTooltipWrapper()) {
        ImGui::Text("Smoothness of epidermal cell layer reflections");
        ImGui::BulletText("Should be 30-50%% lower than Primary");
        ImGui::BulletText("0.2-0.3 : Young skin");
        ImGui::BulletText("0.4+ : Aged/wrinkled skin");
    }

    ImGui::SliderFloat("Specular Texture Multiplier", &settings.SkinSpecularTexMultiplier, 0.0f, 10.0f);
    if (auto _tt = Util::HoverTooltipWrapper()) {
        ImGui::Text("Multiplier for specular map");
        ImGui::BulletText("A multiplier for the vanilla specular map, applied to the first layer's roughness");
    }

    ImGui::SliderFloat("Secondary Specular Strength", &settings.SecondarySpecularStrength, 0.0f, 1.0f);
    if (auto _tt = Util::HoverTooltipWrapper()) {
        ImGui::Text("Intensity of secondary specular highlights");
        ImGui::BulletText("0.3-0.5 : Moist skin");
        ImGui::BulletText("0.1-0.2 : Matte finish");
        ImGui::BulletText("Combine with curvature map for pores");
    }

    ImGui::SliderFloat("Thickness", &settings.Thickness, 0.0f, 1.0f);
    if (auto _tt = Util::HoverTooltipWrapper()) {
        ImGui::Text("Optical thickness for energy compensation");
        ImGui::BulletText("0.0 : Thin areas (ears/nose tip)");
        ImGui::BulletText("0.5 : Average facial skin");
        ImGui::BulletText("1.0 : Thick areas (palms/heels)");
    }
}

Skin::SkinData Skin::GetCommonBufferData()
{
    SkinData data{};
    data.skinParams = float4(settings.SkinMainRoughness, settings.SkinSecondRoughness, settings.SkinSpecularTexMultiplier, float(settings.EnableSkin));
    data.skinParams2 = float4(settings.SecondarySpecularStrength, settings.Thickness, 0.0f, 0.0f);
    return data;
}

void Skin::LoadSettings(json& o_json)
{
    settings = o_json;
}

void Skin::SaveSettings(json& o_json)
{
    o_json = settings;
}

void Skin::RestoreDefaultSettings()
{
    settings = {};
}