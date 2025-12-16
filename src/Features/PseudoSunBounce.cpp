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
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Reflectance color of the ground for bounced light calculation.");
    ImGui::ColorEdit3("Wall Albedo", &settings.wallAlbedo.x);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Reflectance color of the wall for bounced light calculation.");
    ImGui::SliderFloat("Intensity", &settings.intensity, 0.0f, 10.0f, "%.2f");
    ImGui::SliderFloat("Window Width", &settings.windowWidth, 1.0f, 10.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Hanning window width for cosine lobe convolution, preventing negative SH values. Smaller value gives flatter results.");
}