#include "EnvironmentProbeFeature.h"
#include "Globals.h"
#include "Rendering/EnvironmentProbe/CameraPass.h"
#include "State.h"
#include <bit>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EnvironmentProbeFeature::Settings, Enabled, UpdateMode, AnchorDistance, SupplementalSunShadows)

void EnvironmentProbeFeature::LoadSettings(json& value)
{
	settings = value;
	settings.UpdateMode = std::clamp(settings.UpdateMode, 0, 1);
	settings.AnchorDistance = std::isfinite(settings.AnchorDistance) ? std::clamp(settings.AnchorDistance, 0.0f, 128.0f) : 64.0f;
}
void EnvironmentProbeFeature::SaveSettings(json& value) { value = settings; }
void EnvironmentProbeFeature::RestoreDefaultSettings() { settings = {}; }
void EnvironmentProbeFeature::DataLoaded()
{
	RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(this);
}
RE::BSEventNotifyControl EnvironmentProbeFeature::ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (event && event->menuName == RE::LoadingMenu::MENU_NAME && !event->opening)
		EnvironmentProbe::GetCameraPass().Reset();
	return RE::BSEventNotifyControl::kContinue;
}
void EnvironmentProbeFeature::Render()
{
	auto& probe = EnvironmentProbe::GetCameraPass();
	const bool enabled = loaded && settings.Enabled;
	if (probe.enabled != enabled)
		probe.Reset();
	probe.enabled = enabled;
	probe.mode = static_cast<EnvironmentProbe::CameraPass::Mode>(settings.UpdateMode);
	probe.anchorDistance = settings.AnchorDistance;
	probe.supplementalSunShadows = settings.SupplementalSunShadows;
	probe.Render();
	if (enabled && !probe.enabled)
		settings.Enabled = false;
}
void EnvironmentProbeFeature::DrawSettings()
{
	auto& probe = EnvironmentProbe::GetCameraPass();
	ImGui::Checkbox(T("feature.environment_probe.update", "Render every frame"), &settings.Enabled);
	ImGui::Checkbox(T("feature.environment_probe.coarse_sun", "Coarse off-screen sun shadows"), &settings.SupplementalSunShadows);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T("feature.environment_probe.coarse_sun_help", "Adds low-resolution static sun shadows outside CSM coverage, fitted to the octants captured this frame. Excludes small and dynamic objects. Covers nearby receivers up to 5000 game units."));
	const char* modes[] = { T("feature.environment_probe.all_faces", "All eight faces every frame"), T("feature.environment_probe.one_face", "One face per frame, rotating") };
	int mode = settings.UpdateMode;
	if (ImGui::Combo(T("feature.environment_probe.mode", "Capture mode"), &mode, modes, 2))
		settings.UpdateMode = mode;
	ImGui::TextWrapped("%s", T("feature.environment_probe.description", "Captures near the world camera, including dynamic objects. Rotating mode updates one octahedron face per frame and preserves the other faces. Continues updating while this menu is closed. Material shaders compile in the background."));
	ImGui::SliderFloat(T("feature.environment_probe.anchor_distance", "Anchor distance"), &settings.AnchorDistance, 0.0f, 128.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T("feature.environment_probe.anchor_help", "Prefers free space in front of the camera, with a dead zone and smooth movement to complement screen visibility. Distance is in game units; zero follows the camera. Collision checks shorten the offset near walls."));
	if (probe.result == EnvironmentProbe::Result::ShadersPending)
		ImGui::TextWrapped("%s", T("feature.environment_probe.preparing", "Preparing probe material shaders in the background..."));
	else if (probe.result == EnvironmentProbe::Result::ShaderUnavailable)
		ImGui::TextWrapped("%s", T("feature.environment_probe.shader_failed", "Capture stopped because a shader could not compile. Check the shader log, then enable rendering to retry."));
	else if (probe.result != EnvironmentProbe::Result::Success)
		ImGui::TextWrapped("%s", T("feature.environment_probe.unavailable", "No complete capture available. Requires an active SE/AE world view with Lighting shaders enabled."));

	ImGui::Text(T("feature.environment_probe.valid_faces", "Valid octants: %u / 8"), std::popcount(probe.ValidFaces()));
	const float size = std::max(1.0f, std::min(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().IndentSpacing, ImGui::GetFontSize() * 24.0f));
	const float scale = size / EnvironmentProbe::CameraPass::Resolution;
	ImGui::SetNextItemOpen(true, ImGuiCond_Once);
	preview.Draw(probe.Radiance(), "EnvironmentProbeRadiance", T("feature.environment_probe.radiance", "Radiance (linear)"), scale);
	ImGui::SetNextItemOpen(true, ImGuiCond_Once);
	preview.Draw(probe.Depth(), "EnvironmentProbeDepth", T("feature.environment_probe.depth", "Radial depth (game units)"), scale, true, { 0.0f, EnvironmentProbe::CameraPass::FarDistance });
}
