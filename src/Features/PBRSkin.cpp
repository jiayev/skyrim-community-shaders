#include "PBRSkin.h"

#include "State.h"
#include "Util.h"
#include "ShaderCache.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    PBRSkin::Settings,
    EnablePBRSkin,
    EnablePBRHair,
    SkinRoughnessScale,
    SkinSpecularLevel,
    SkinSpecularTexMultiplier,
    HairRoughnessScale,
    HairSpecularLevel,
    HairSpecularTexMultiplier
)

void PBRSkin::DrawSettings()
{
    ImGui::Checkbox("Enable Skin", &settings.EnablePBRSkin);
    ImGui::SliderFloat("Skin Roughness", &settings.SkinRoughnessScale, 0.f, 1.f, "%.3f");
    ImGui::SliderFloat("Skin Specular Level", &settings.SkinSpecularLevel, 0.f, 0.1f, "%.4f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("This \"Specular\" here is the same as the \"Specular\" in the PBR material object data. 0.028 is the default value for skin.");
    ImGui::SliderFloat("Skin Specular Texture Multiplier", &settings.SkinSpecularTexMultiplier, 0.f, 10.f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("A multiplier for the vanilla specular texture. It will be used to modify the roughness of the skin.");

    ImGui::Checkbox("Enable Hair", &settings.EnablePBRHair);
    ImGui::SliderFloat("Hair Roughness", &settings.HairRoughnessScale, 0.f, 1.f, "%.3f");
    ImGui::SliderFloat("Hair Specular Level", &settings.HairSpecularLevel, 0.f, 0.1f, "%.4f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("This \"Specular\" here is the same as the \"Specular\" in the PBR material object data. 0.045 is the default value for hair.");
    ImGui::SliderFloat("Hair Specular Texture Multiplier", &settings.HairSpecularTexMultiplier, 0.f, 10.f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("A multiplier for the vanilla specular texture. It will be used to modify the roughness of the hair.");
}

PBRSkin::PBRSkinData PBRSkin::GetCommonBufferData()
{
    PBRSkinData data{};
    data.skinParams = float4(settings.SkinRoughnessScale, settings.SkinSpecularLevel, settings.SkinSpecularTexMultiplier, float(settings.EnablePBRSkin));
    data.hairParams = float4(settings.HairRoughnessScale, settings.HairSpecularLevel, settings.HairSpecularTexMultiplier, float(settings.EnablePBRHair));
    return data;
}

void PBRSkin::LoadSettings(json& o_json)
{
    settings = o_json;
}

void PBRSkin::SaveSettings(json& o_json)
{
    o_json = settings;
}

void PBRSkin::RestoreDefaultSettings()
{
    settings = {};
}