#include "PseudoSunBounce.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    PseudoSunBounce::Settings,
    groundAlbedo,
    intensity,
    wallAlbedo,
    windowWidth)

void PseudoSunBounce::LoadSettings(json& o_json)
{
    settings = o_json;
}

void PseudoSunBounce::SaveSettings(json& o_json)
{
    o_json = settings;
}

void PseudoSunBounce::RestoreDefaultSettings()
{
    settings = {};
}

void PseudoSunBounce::DrawSettings()
{
    ImGui::ColorEdit3("Ground Albedo", &settings.groundAlbedo.x);
    ImGui::ColorEdit3("Wall Albedo", &settings.wallAlbedo.x);
    ImGui::SliderFloat("Intensity", &settings.intensity, 0.0f, 10.0f, "%.2f");
    ImGui::SliderFloat("Window Width", &settings.windowWidth, 1.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Adjust the properties of the pseudo sun bounce light to achieve the desired indirect lighting effect.");
}