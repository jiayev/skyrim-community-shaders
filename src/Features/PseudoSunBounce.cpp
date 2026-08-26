#include "PseudoSunBounce.h"
#include "I18n/I18n.h"

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
	ImGui::ColorEdit3(T("feature.pseudo_sun_bounce.ground_albedo", "Ground Albedo"), &settings.groundAlbedo.x);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.pseudo_sun_bounce.reflectance_color_of_the_ground_for_bounced_light", "Reflectance color of the ground for bounced light calculation."));
	ImGui::ColorEdit3(T("feature.pseudo_sun_bounce.wall_albedo", "Wall Albedo"), &settings.wallAlbedo.x);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.pseudo_sun_bounce.reflectance_color_of_the_wall_for_bounced_light", "Reflectance color of the wall for bounced light calculation."));
	ImGui::SliderFloat(T("feature.pseudo_sun_bounce.intensity", "Intensity"), &settings.intensity, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T("feature.pseudo_sun_bounce.window_width", "Window Width"), &settings.windowWidth, 1.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.pseudo_sun_bounce.hanning_window_width_for_cosine_lobe_convolution_preventing", "Hanning window width for cosine lobe convolution, preventing negative SH values. Smaller value gives flatter results."));
}