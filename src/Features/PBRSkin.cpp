#include "PBRSkin.h"

#include "Hooks.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    PBRSkin::Settings,
    EnablePBRSkin,
    SkinRoughnessScale,
    SkinSpecularLevel,
    SkinSpecularTexMultiplier
)

void PBRSkin::DrawSettings()
{
    ImGui::Checkbox("Enable PBR Skin", &settings.EnablePBRSkin);
    ImGui::SliderFloat("Skin Roughness", &settings.SkinRoughnessScale, 0.f, 1.f, "%.3f");
    ImGui::SliderFloat("Skin Specular Level", &settings.SkinSpecularLevel, 0.f, 0.1f, "%.4f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("This \"Specular\" here is the same as the \"Specular\" in the PBR material object data. 0.028 is the default value for skin.");
    ImGui::SliderFloat("Skin Specular Texture Multiplier", &settings.SkinSpecularTexMultiplier, 0.f, 10.f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("A multiplier for the vanilla specular texture. It will be used to modify the roughness of the skin.");
}

PBRSkin::PBRSkinData PBRSkin::GetCommonBufferData()
{
    PBRSkinData data{};
    data.skinParams = float4(settings.SkinRoughnessScale, settings.SkinSpecularLevel, settings.SkinSpecularTexMultiplier, float(settings.EnablePBRSkin));
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