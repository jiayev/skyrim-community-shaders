#include "Skin.h"
#include "Skin/Editor/Files.h"

#include <cmath>
#include <imgui_stdlib.h>

void Skin::LoadSettings(json& a_json)
{
	std::lock_guard lock(mutex);
	Settings next;
	try {
		next.EnableSkin = a_json.value("EnableSkin", true);
		next.EnableDetail = a_json.value("EnableDetail", true);
		next.DetailTexture = a_json.value("DetailTexture", next.DetailTexture);
		auto number = [&](const char* name, float& value, float minimum, float maximum) {
			const float input = a_json.value(name, value);
			if (!std::isfinite(input))
				throw std::runtime_error(std::string(T("feature.skin.invalid_setting", "Invalid setting: ")) + name);
			value = std::clamp(input, minimum, maximum);
		};
		number("DetailStrength", next.DetailStrength, 0, 1);
		number("DetailTiling", next.DetailTiling, 0.1f, 100);
		number("BodyTiling", next.BodyTiling, 0.1f, 5);
		number("ExtraSkinWetness", next.ExtraSkinWetness, 0, 2);
		number("WetFadeTime", next.WetFadeTime, 0, 50);
		number("StartSweat", next.StartSweat, 0, 1);
		number("FullSweat", next.FullSweat, 0, 1);
		if (a_json.contains("WetParams")) {
			next.WetParams = a_json.at("WetParams").get<float4>();
			const auto& w = next.WetParams;
			if (!std::isfinite(w.x) || !std::isfinite(w.y) || !std::isfinite(w.z) || !std::isfinite(w.w) ||
				w.x < 0 || w.x > 1024 || w.y < 0 || w.y > 2 || w.z < 0 || w.z > 20 || w.w < 0 || w.w > 20)
				throw std::runtime_error(T("feature.skin.invalid_global_wetness_parameters", "Invalid global wetness parameters."));
		}
		settings = next;
	} catch (const std::exception& e) {
		logger::warn("[Advanced Skin] Global settings retained: {}", e.what());
	}
	Invalidate(true);
}

void Skin::SaveSettings(json& a_json)
{
	std::lock_guard lock(mutex);
	a_json = { { "EnableSkin", settings.EnableSkin }, { "EnableDetail", settings.EnableDetail },
		{ "DetailStrength", settings.DetailStrength }, { "DetailTiling", settings.DetailTiling },
		{ "BodyTiling", settings.BodyTiling }, { "DetailTexture", settings.DetailTexture },
		{ "ExtraSkinWetness", settings.ExtraSkinWetness }, { "WetFadeTime", settings.WetFadeTime },
		{ "StartSweat", settings.StartSweat }, { "FullSweat", settings.FullSweat }, { "WetParams", settings.WetParams } };
}

void Skin::RestoreDefaultSettings()
{
	std::lock_guard lock(mutex);
	settings = {};
	Invalidate(true);
}

void Skin::DrawSettings()
{
	std::lock_guard lock(mutex);
	bool changed = ImGui::Checkbox(T("feature.skin.enable_advanced_skin", "Enable Advanced Skin"), &settings.EnableSkin);
	changed |= ImGui::Checkbox(T("feature.skin.enable_skin_detail", "Enable skin detail"), &settings.EnableDetail);
	ImGui::TextWrapped("%s", T("feature.skin.global_detail_applies_when_a_material_has_no_detail_texture", "Global detail applies when a material has no detail texture. Its alpha mask uses the model's main UV, independently of tiling."));
	changed |= ImGui::SliderFloat(T("feature.skin.global_detail_strength", "Global detail strength"), &settings.DetailStrength, 0, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	changed |= ImGui::SliderFloat(T("feature.skin.global_detail_tiling", "Global detail tiling"), &settings.DetailTiling, 0.1f, 100, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	changed |= ImGui::SliderFloat(T("feature.skin.global_body_tiling_multiplier", "Global body tiling multiplier"), &settings.BodyTiling, 0.1f, 5, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	changed |= ImGui::InputText(T("feature.skin.global_detail_dds", "Global detail DDS"), &settings.DetailTexture);
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.choose_dds", "Choose DDS"))) {
		try {
			if (const auto path = SkinEditor::ChooseFile(false, SkinEditor::DDSFilter, L"dds")) {
				const auto imported = SkinEditor::ImportTexture(*path);
				if (!imported.empty()) {
					settings.DetailTexture = imported;
					changed = true;
				}
			}
		} catch (const std::exception& error) {
			editor.error = error.what();
		}
	}
	if (!editor.error.empty())
		ImGui::TextWrapped("%s", editor.error.c_str());
	if (ImGui::Button(T("feature.skin.reload_skin_resources", "Reload skin resources")))
		Invalidate(true);
	DrawGlobalSettings();
	if (changed)
		Invalidate();
}

void Skin::DrawGlobalSettings()
{
	bool changed = ImGui::SliderFloat(T("feature.skin.extra_skin_wetness", "Extra skin wetness"), &settings.ExtraSkinWetness, 0, 2, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	changed |= ImGui::SliderFloat(T("feature.skin.wetness_fade_time", "Wetness fade time"), &settings.WetFadeTime, 0, 50, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	changed |= ImGui::SliderFloat(T("feature.skin.sweat_starts_below_stamina", "Sweat starts below stamina"), &settings.StartSweat, 0, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	changed |= ImGui::SliderFloat(T("feature.skin.full_sweat_below_stamina", "Full sweat below stamina"), &settings.FullSweat, 0, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	if (changed)
		Invalidate();
}
