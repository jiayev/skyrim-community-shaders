#include "Skin.h"
#include <DirectXTex.h>
#include <imgui_stdlib.h>

#include <fstream>

#include "Deferred.h"
#include "Globals.h"
#include "Hooks.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/Form.h"
#include "Utils/UI.h"

#include "DynamicWetness_PublicAPI.h"
#include "I18n/I18n.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skin::SkinProfile,
	SkinMainRoughness,
	SkinSecondRoughness,
	SkinSpecularTexMultiplier,
	SecondarySpecularStrength,
	F0,
	BaseColorMultiplier,
	PhysicalMainRoughnessMultiplier,
	PhysicalSecondRoughnessMultiplier,
	PhysicalSpecularStrength,
	ExtraEdgeRoughness,
	EnableSkinDetail,
	SkinDetailStrength,
	SkinDetailTiling,
	BodyTilingMultiplier,
	Translucency,
	sssWidth,
	UseSSS,
	FuzzStrength,
	FuzzRoughness,
	FuzzF0);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skin::Settings,
	EnableSkin,
	ExtraSkinWetness,
	WetFadeTime,
	StartSweat,
	FullSweat,
	WetParams,
	UseDynamicWetness,
	DefaultProfile,
	Profiles,
	RaceProfiles);

namespace
{
	std::string MakeUniqueProfileName(const std::map<std::string, Skin::SkinProfile>& a_profiles, const std::string& a_name)
	{
		if (!a_profiles.contains(a_name))
			return a_name;
		for (uint32_t i = 2;; ++i) {
			auto candidate = std::format("{} {}", a_name, i);
			if (!a_profiles.contains(candidate))
				return candidate;
		}
	}

	/** @brief Lower-cases, forward-slashes, and strips an optional leading "meshes/" so NIF keys compare equal. */
	std::string NormalizeNifKey(std::string a_key)
	{
		std::transform(a_key.begin(), a_key.end(), a_key.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::replace(a_key.begin(), a_key.end(), '\\', '/');
		constexpr std::string_view kMeshesPrefix = "meshes/";
		if (a_key.starts_with(kMeshesPrefix))
			a_key.erase(0, kMeshesPrefix.size());
		return a_key;
	}

	/**
	 * @brief Derives the per-NIF identity for a geometry.
	 *
	 * Prefers the base object's MODL path (reliable for statics, armor, weapons);
	 * falls back to the geometry node name (e.g. body meshes whose MODL is empty).
	 */
	std::string DeriveNifKey(RE::BSGeometry* a_geometry)
	{
		std::string key;
		if (auto ref = a_geometry->GetUserData()) {
			if (auto base = ref->GetBaseObject()) {
				if (auto model = base->As<RE::TESModel>()) {
					if (const char* path = model->GetModel())
						key = path;
				}
			}
		}
		if (key.empty()) {
			if (const char* name = a_geometry->name.c_str())
				key = name;
		}
		return NormalizeNifKey(key);
	}

	/** @brief Normalizes a BaseID key: strips an optional 0x prefix and upper-cases hex digits. */
	std::string NormalizeBaseIdKey(std::string a_key)
	{
		if (a_key.size() >= 2 && a_key[0] == '0' && (a_key[1] == 'x' || a_key[1] == 'X'))
			a_key.erase(0, 2);
		std::transform(a_key.begin(), a_key.end(), a_key.begin(),
			[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		return a_key;
	}

	/** @brief Formats a form ID as the canonical 8-hex-digit BaseID key. */
	std::string FormatBaseIdKey(RE::FormID a_id)
	{
		return std::format("{:08X}", a_id);
	}

	/** @brief Derives the per-NPC-base identity for a geometry (empty for non-actors). */
	std::string DeriveBaseIdKey(RE::BSGeometry* a_geometry)
	{
		auto userData = a_geometry->GetUserData();
		if (userData && userData->formType == RE::FormType::ActorCharacter) {
			if (auto* npc = static_cast<RE::Character*>(userData)->GetActorBase())
				return FormatBaseIdKey(npc->formID);
		}
		return {};
	}

	/** @brief Returns the player's world-space NiCamera, or null. */
	RE::NiCamera* GetPlayerNiCamera()
	{
		auto* playerCamera = RE::PlayerCamera::GetSingleton();
		if (!playerCamera || !playerCamera->cameraRoot)
			return nullptr;
		for (auto& child : playerCamera->cameraRoot->GetChildren()) {
			if (auto* camera = netimmerse_cast<RE::NiCamera*>(child.get()))
				return camera;
		}
		return nullptr;
	}

	/** @brief Raycasts through the screen center and returns the hit reference, or null. */
	RE::TESObjectREFR* PickCrosshairRef()
	{
		auto* niCamera = GetPlayerNiCamera();
		if (!niCamera)
			return nullptr;

		const auto display = ImGui::GetIO().DisplaySize;
		if (display.x <= 0.0f || display.y <= 0.0f)
			return nullptr;

		RE::NiPoint3 origin, dir;
		if (!niCamera->WindowPointToRay(static_cast<std::int32_t>(display.x * 0.5f), static_cast<std::int32_t>(display.y * 0.5f),
				origin, dir, display.x, display.y))
			return nullptr;
		dir.Unitize();

		constexpr float kSkyrimToHavok = 0.0142875f;
		constexpr float kRayLength = 100000.0f;
		const RE::NiPoint3 end = origin + dir * kRayLength;

		RE::bhkPickData pick{};
		pick.rayInput.from = RE::hkVector4(origin.x * kSkyrimToHavok, origin.y * kSkyrimToHavok, origin.z * kSkyrimToHavok, 0.0f);
		pick.rayInput.to = RE::hkVector4(end.x * kSkyrimToHavok, end.y * kSkyrimToHavok, end.z * kSkyrimToHavok, 0.0f);

		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return nullptr;
		tes->Pick(pick);

		if (!pick.rayOutput.rootCollidable)
			return nullptr;
		return RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
	}
}

void Skin::DrawSettings()
{
	ImGui::Checkbox(T("feature.skin.enable_advanced_skin", "Enable Advanced Skin"), &settings.EnableSkin);

	ImGui::Text("%s", T("feature.skin.advanced_skin_shader_using_dual_specular_lobes", "Advanced Skin Shader using dual specular lobes."));

	ImGui::Spacing();

	if (ImGui::CollapsingHeader(T("feature.skin.profiles_section", "Skin Profiles"), ImGuiTreeNodeFlags_DefaultOpen))
		DrawProfileManager();

	if (ImGui::CollapsingHeader(T("feature.skin.race_bindings_section", "Race Bindings"), ImGuiTreeNodeFlags_DefaultOpen))
		DrawRaceBindings();

	if (ImGui::CollapsingHeader(T("feature.skin.global_section", "Wetness (Global)"), ImGuiTreeNodeFlags_DefaultOpen))
		DrawGlobalSettings();

	if (ImGui::CollapsingHeader(T("feature.skin.nif_overrides_section", "NIF Overrides"), ImGuiTreeNodeFlags_DefaultOpen))
		DrawNifOverrides();

	ImGui::Spacing();

	if (ImGui::Button(T("feature.skin.reload_skin_detail_texture", "Reload Skin Detail Texture"))) {
		ReloadSkinDetail();
	}

	BUFFER_VIEWER_NODE(texSkinDetail, 1.0f)
}

void Skin::DrawProfileManager()
{
	const char* defaultLabel = T("feature.skin.default_profile", "[Default]");

	if (!uiSelectedProfile.empty() && !settings.Profiles.contains(uiSelectedProfile))
		uiSelectedProfile.clear();

	if (ImGui::BeginCombo(T("feature.skin.editing_profile", "Editing Profile"), uiSelectedProfile.empty() ? defaultLabel : uiSelectedProfile.c_str())) {
		if (ImGui::Selectable(defaultLabel, uiSelectedProfile.empty()))
			uiSelectedProfile.clear();
		for (auto& [name, profile] : settings.Profiles) {
			if (ImGui::Selectable(name.c_str(), name == uiSelectedProfile))
				uiSelectedProfile = name;
		}
		ImGui::EndCombo();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.the_default_profile_is_used_by_every_race", "The default profile is used by every race without its own binding."));
	}

	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
	ImGui::InputTextWithHint("##SkinProfileName", T("feature.skin.profile_name_hint", "Profile name"), &uiProfileNameBuffer);

	ImGui::SameLine();
	ImGui::BeginDisabled(uiProfileNameBuffer.empty());
	if (ImGui::Button(T("feature.skin.add_profile", "Add"))) {
		auto name = MakeUniqueProfileName(settings.Profiles, uiProfileNameBuffer);
		settings.Profiles[name] = uiSelectedProfile.empty() ? settings.DefaultProfile : settings.Profiles[uiSelectedProfile];
		uiSelectedProfile = name;
		uiProfileNameBuffer.clear();
		InvalidateProfileBindings();
	}
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.creates_a_copy_of_the_profile_being_edited", "Creates a copy of the profile being edited."));
	}

	ImGui::SameLine();
	ImGui::BeginDisabled(uiProfileNameBuffer.empty() || uiSelectedProfile.empty());
	if (ImGui::Button(T("feature.skin.rename_profile", "Rename"))) {
		auto node = settings.Profiles.extract(uiSelectedProfile);
		auto newName = MakeUniqueProfileName(settings.Profiles, uiProfileNameBuffer);
		node.key() = newName;
		settings.Profiles.insert(std::move(node));
		for (auto& [race, profileName] : settings.RaceProfiles) {
			if (profileName == uiSelectedProfile)
				profileName = newName;
		}
		uiSelectedProfile = newName;
		uiProfileNameBuffer.clear();
		InvalidateProfileBindings();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::BeginDisabled(uiSelectedProfile.empty());
	if (ImGui::Button(T("feature.skin.delete_profile", "Delete"))) {
		settings.Profiles.erase(uiSelectedProfile);
		for (auto it = settings.RaceProfiles.begin(); it != settings.RaceProfiles.end();)
			it = it->second == uiSelectedProfile ? settings.RaceProfiles.erase(it) : std::next(it);
		uiSelectedProfile.clear();
		InvalidateProfileBindings();
	}
	ImGui::EndDisabled();

	ImGui::Separator();

	DrawProfileSettings(uiSelectedProfile.empty() ? settings.DefaultProfile : settings.Profiles[uiSelectedProfile]);
}

void Skin::DrawRaceBindings()
{
	if (raceList.empty())
		RefreshRaceList();

	if (settings.Profiles.empty()) {
		ImGui::Text("%s", T("feature.skin.create_a_profile_first_to_bind_it_to", "Create a profile first to bind it to a race."));
		return;
	}

	const char* raceComboId = T("feature.skin.race", "Race");
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
	if (ImGui::BeginCombo(raceComboId, uiPendingRace.empty() ? T("feature.skin.select_a_race", "Select a race") : uiPendingRace.c_str())) {
		auto searchText = Util::DrawComboSearchInput(raceComboId);
		for (auto& [editorID, displayName] : raceList) {
			if (searchText.empty() || Util::StringMatchesSearch(displayName, searchText)) {
				if (ImGui::Selectable(displayName.c_str(), editorID == uiPendingRace)) {
					uiPendingRace = editorID;
					Util::ClearComboSearch(raceComboId);
					break;
				}
			}
		}
		ImGui::EndCombo();
	} else {
		Util::ClearComboSearch(raceComboId);
	}

	ImGui::SameLine();
	ImGui::BeginDisabled(uiPendingRace.empty());
	if (ImGui::Button(T("feature.skin.bind_race", "Bind"))) {
		settings.RaceProfiles[uiPendingRace] = uiSelectedProfile.empty() ? settings.Profiles.begin()->first : uiSelectedProfile;
		uiPendingRace.clear();
		InvalidateProfileBindings();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.refresh_race_list", "Refresh Races")))
		RefreshRaceList();

	if (settings.RaceProfiles.empty())
		return;

	std::string pendingErase;
	if (ImGui::BeginTable("##SkinRaceBindings", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn(T("feature.skin.race", "Race"));
		ImGui::TableSetupColumn(T("feature.skin.profile", "Profile"));
		ImGui::TableSetupColumn("##SkinRaceBindingActions");
		ImGui::TableHeadersRow();

		for (auto& [raceEditorID, profileName] : settings.RaceProfiles) {
			ImGui::PushID(raceEditorID.c_str());
			ImGui::TableNextRow();

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(raceEditorID.c_str());

			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::BeginCombo("##SkinRaceProfile", profileName.c_str())) {
				for (auto& [name, profile] : settings.Profiles) {
					if (ImGui::Selectable(name.c_str(), name == profileName)) {
						profileName = name;
						InvalidateProfileBindings();
					}
				}
				ImGui::EndCombo();
			}

			ImGui::TableNextColumn();
			if (ImGui::Button(T("feature.skin.remove_binding", "Remove")))
				pendingErase = raceEditorID;

			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (!pendingErase.empty()) {
		settings.RaceProfiles.erase(pendingErase);
		InvalidateProfileBindings();
	}
}

void Skin::RefreshRaceList()
{
	raceList.clear();

	auto dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler)
		return;

	for (auto race : dataHandler->GetFormArray<RE::TESRace>()) {
		if (!race)
			continue;
		auto editorID = Util::GetFormEditorID(race);
		if (editorID.empty())
			continue;
		const char* fullName = race->GetFullName();
		raceList.emplace_back(editorID, fullName && *fullName ? std::format("{} ({})", fullName, editorID) : editorID);
	}

	std::sort(raceList.begin(), raceList.end(), [](const auto& a_lhs, const auto& a_rhs) { return a_lhs.second < a_rhs.second; });
}

void Skin::DrawProfileSettings(SkinProfile& a_profile)
{
	ImGui::SliderFloat(T("feature.skin.primary_roughness", "Primary Roughness"), &a_profile.SkinMainRoughness, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.controls_microscopic_roughness_of_stratum_corneum_layer", "Controls microscopic roughness of stratum corneum layer"));
	}

	ImGui::SliderFloat(T("feature.skin.secondary_roughness", "Secondary Roughness"), &a_profile.SkinSecondRoughness, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.smoothness_of_epidermal_cell_layer_reflections", "Smoothness of epidermal cell layer reflections"));
		ImGui::BulletText(T("feature.skin.should_be_30_50_lower_than_primary", "Should be 30-50%% lower than Primary"));
	}

	ImGui::SliderFloat(T("feature.skin.specular_texture_multiplier", "Specular Texture Multiplier"), &a_profile.SkinSpecularTexMultiplier, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.multiplier_for_specular_map", "Multiplier for specular map"));
		ImGui::BulletText("%s", T("feature.skin.a_multiplier_for_the_vanilla_specular_map_applied", "A multiplier for the vanilla specular map, applied to the first layer's roughness"));
	}

	ImGui::SliderFloat(T("feature.skin.secondary_specular_strength", "Secondary Specular Strength"), &a_profile.SecondarySpecularStrength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.intensity_of_secondary_specular_highlights", "Intensity of secondary specular highlights"));
	}

	ImGui::SliderFloat(T("feature.skin.fresnel_f0", "Fresnel F0"), &a_profile.F0, 0.0f, 0.1f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.fresnel_reflectance", "Fresnel reflectance"));
	}

	ImGui::SliderFloat(T("feature.skin.base_color_multiplier", "Base Color Multiplier"), &a_profile.BaseColorMultiplier, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.multiplier_for_the_base_color_texture", "Multiplier for the base color texture"));
	}

	ImGui::Spacing();
	ImGui::Text("%s", T("feature.skin.options_for_additional_roughness_and_specular_maps", "Options for additional roughness and specular maps."));

	ImGui::SliderFloat(T("feature.skin.physical_main_roughness_multiplier", "Physical Main Roughness Multiplier"), &a_profile.PhysicalMainRoughnessMultiplier, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat(T("feature.skin.physical_second_roughness_multiplier", "Physical Second Roughness Multiplier"), &a_profile.PhysicalSecondRoughnessMultiplier, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat(T("feature.skin.physical_specular_multiplier", "Physical Specular Multiplier"), &a_profile.PhysicalSpecularStrength, 0.0f, 2.0f, "%.2f");

	ImGui::Spacing();

	ImGui::SliderFloat(T("feature.skin.extra_edge_roughness", "Extra Edge Roughness"), &a_profile.ExtraEdgeRoughness, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.extra_roughness_at_the_edges_of_the_skin", "Extra roughness at the edges of the skin, to approximate peach fuzz on the face."));
	}

	ImGui::SliderFloat(T("feature.skin.fuzz_strength", "Fuzz Strength"), &a_profile.FuzzStrength, 0.0f, 2.0f, "%.2f");

	ImGui::SliderFloat(T("feature.skin.fuzz_roughness", "Fuzz Roughness"), &a_profile.FuzzRoughness, 0.1f, 1.0f, "%.2f");

	ImGui::SliderFloat(T("feature.skin.fuzz_f0", "Fuzz F0"), &a_profile.FuzzF0, 0.0f, 0.5f, "%.4f");

	ImGui::Spacing();

	ImGui::Checkbox(T("feature.skin.enable_sss_transmission", "Enable SSS Transmission"), &a_profile.UseSSS);

	ImGui::SliderFloat(T("feature.skin.translucency", "Translucency"), &a_profile.Translucency, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.translucency_of_the_sss_transmittance_effect", "Translucency of the SSS Transmittance effect"));
	}

	ImGui::SliderFloat(T("feature.skin.sss_width", "SSS Width"), &a_profile.sssWidth, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.width_of_the_sss_transmittance_effect", "Width of the SSS Transmittance effect"));
	}

	ImGui::Spacing();

	ImGui::Checkbox(T("feature.skin.enable_skin_detail", "Enable Skin Detail"), &a_profile.EnableSkinDetail);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.enable_skin_detail_texture", "Enable skin detail texture"));
	}

	ImGui::SliderFloat(T("feature.skin.skin_detail_strength", "Skin Detail Strength"), &a_profile.SkinDetailStrength, -2.0f, 2.0f);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.strength_of_skin_detail_texture", "Strength of skin detail texture"));
	}

	ImGui::SliderFloat(T("feature.skin.skin_detail_tiling", "Skin Detail Tiling"), &a_profile.SkinDetailTiling, 1.0f, 50.0f, "%1.f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.the_more_tiling_the_more_detailed_the_skin", "The more tiling, the more detailed the skin will be"));
	}

	ImGui::SliderFloat(T("feature.skin.body_tiling_multiplier", "Body Tiling Multiplier"), &a_profile.BodyTilingMultiplier, 0.5f, 5.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.multiply_the_tiling_for_the_body_to_match", "Multiply the tiling for the body to match the face"));
	}
}

void Skin::DrawGlobalSettings()
{
	ImGui::SliderFloat(T("feature.skin.extra_skin_wetness", "Extra Skin Wetness"), &settings.ExtraSkinWetness, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.adds_a_constant_layer_of_wetness_to_all", "Adds a constant layer of wetness to all skin, making it look slightly damp or sweaty at all times, even when not in water or exerting effort."));
	}

	ImGui::SliderFloat(T("feature.skin.wetness_fade_out_time", "Wetness Fade Out Time"), &settings.WetFadeTime, 0.0f, 50.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.how_many_seconds_it_takes_for_skin_to", "How many seconds it takes for skin to fully dry after leaving water. Higher values mean wetness lingers longer."));
	}

	if (isDynamicWetnessAvailable) {
		ImGui::Text("%s", T("feature.skin.dynamic_wetness_detected", "Dynamic Wetness detected."));
		ImGui::Checkbox(T("feature.skin.use_dynamic_wetness", "Use Dynamic Wetness"), &settings.UseDynamicWetness);
	} else {
		settings.UseDynamicWetness = false;
	}

	if (!settings.UseDynamicWetness) {
		ImGui::SliderFloat(T("feature.skin.stamina_threshold_for_sweat", "Stamina Threshold for Sweat"), &settings.StartSweat, 0.0f, 1.0f, "%.2f",
			ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(T("feature.skin.the_character_starts_sweating_when_their_stamina_drops", "The character starts sweating when their stamina drops below this percentage. For example, 0.75 means sweat appears below 75%% stamina."));
		}
		ImGui::SliderFloat(T("feature.skin.full_sweat_threshold", "Full Sweat Threshold"), &settings.FullSweat, 0.0f, 1.0f, "%.2f",
			ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(T("feature.skin.the_character_reaches_maximum_sweat_when_stamina_drops", "The character reaches maximum sweat when stamina drops below this percentage. For example, 0.15 means full sweat below 15%% stamina."));
		}
	}

	ImGui::SliderFloat(T("feature.skin.wetness_perlin_noise_scale", "Wetness Perlin Noise Scale"), &settings.WetParams.x, 0.0f, 1024.0f, "%1.f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.controls_the_size_of_the_wet_dry_pattern", "Controls the size of the wet/dry pattern on skin. Higher values create a finer, more detailed pattern; lower values produce larger, broader wet patches."));
	}
	ImGui::SliderFloat(T("feature.skin.wetness_perlin_noise_lacunarity", "Wetness Perlin Noise Lacunarity"), &settings.WetParams.y, 0.0f, 2.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.controls_how_much_fine_detail_is_added_to", "Controls how much fine detail is added to the wetness pattern. Higher values add more small-scale variation on top of the base pattern."));
	}
	ImGui::SliderFloat(T("feature.skin.wetness_perlin_noise_persistence", "Wetness Perlin Noise Persistence"), &settings.WetParams.z, 0.0f, 20.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.controls_the_overall_contrast_and_roughness_of_the", "Controls the overall contrast and roughness of the wetness pattern. Higher values make the pattern more pronounced and varied."));
	}
	ImGui::SliderFloat(T("feature.skin.wetness_normal_scale", "Wetness Normal Scale"), &settings.WetParams.w, 0.0f, 20.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.controls_how_bumpy_wet_skin_appears_higher_values", "Controls how bumpy wet skin appears. Higher values create more visible surface ripples and distortion on wet areas."));
	}
}

RE::TESRace* Skin::GetRaceForRef(RE::TESObjectREFR* a_ref) const
{
	if (a_ref && a_ref->formType == RE::FormType::ActorCharacter)
		return static_cast<RE::Character*>(a_ref)->GetRace();
	return nullptr;
}

bool Skin::ReferenceHasSkin(RE::TESObjectREFR* a_ref) const
{
	auto* root = a_ref ? a_ref->Get3D() : nullptr;
	if (!root)
		return false;

	bool hasSkin = false;
	RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
		auto& runtimeData = a_geometry->GetGeometryRuntimeData();
		if (runtimeData.skinInstance) {
			hasSkin = true;
			return RE::BSVisit::BSVisitControl::kStop;
		}
		if (auto* prop = runtimeData.shaderProperty.get()) {
			if (prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kSkinned)) {
				hasSkin = true;
				return RE::BSVisit::BSVisitControl::kStop;
			}
			if (prop->GetMaterialType() == RE::BSShaderMaterial::Type::kLighting) {
				if (auto* material = prop->material) {
					auto feature = material->GetFeature();
					if (feature == RE::BSShaderMaterial::Feature::kFaceGen || feature == RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
						hasSkin = true;
						return RE::BSVisit::BSVisitControl::kStop;
					}
				}
			}
		}
		return RE::BSVisit::BSVisitControl::kContinue;
	});
	return hasSkin;
}

std::string Skin::DeriveNifKeyForRef(RE::TESObjectREFR* a_ref) const
{
	std::string key;
	if (a_ref) {
		if (auto* base = a_ref->GetObjectReference()) {
			if (auto* model = base->As<RE::TESModel>()) {
				if (const char* path = model->GetModel())
					key = path;
			}
		}
	}
	if (key.empty()) {
		if (auto* root = a_ref ? a_ref->Get3D() : nullptr) {
			RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
				if (const char* name = a_geometry->name.c_str()) {
					key = name;
					return RE::BSVisit::BSVisitControl::kStop;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
	}
	return NormalizeNifKey(key);
}

json Skin::DiffProfile(const SkinProfile& a_base, const SkinProfile& a_full) const
{
	json baseJson = a_base;
	json fullJson = a_full;
	json partial = json::object();
	for (auto it = fullJson.begin(); it != fullJson.end(); ++it) {
		if (!baseJson.contains(it.key()) || baseJson[it.key()] != it.value())
			partial[it.key()] = it.value();
	}
	return partial;
}

void Skin::ResolveUiPick(RE::TESObjectREFR* a_ref)
{
	uiPickValid = false;
	uiPickHasSkin = false;
	uiPickKey.clear();
	uiPickBaseIdKey.clear();
	uiPickRefLabel.clear();
	uiPickMessage.clear();
	uiPickBase = settings.DefaultProfile;
	uiPickBaseLabel = "Default";

	if (!a_ref) {
		uiPickMessage = T("feature.skin.pick_no_target", "No target under the crosshair / no console selection.");
		return;
	}
	uiPickValid = true;

	if (auto* base = a_ref->GetObjectReference())
		uiPickRefLabel = clib_util::editorID::get_editorID(base);
	if (uiPickRefLabel.empty())
		uiPickRefLabel = std::format("0x{:08X}", a_ref->GetFormID());

	uiPickKey = DeriveNifKeyForRef(a_ref);
	uiPickHasSkin = ReferenceHasSkin(a_ref);

	if (a_ref->formType == RE::FormType::ActorCharacter) {
		if (auto* npc = static_cast<RE::Character*>(a_ref)->GetActorBase())
			uiPickBaseIdKey = FormatBaseIdKey(npc->formID);
	}

	if (uiPickHasSkin) {
		if (auto* race = GetRaceForRef(a_ref)) {
			auto editorID = Util::GetFormEditorID(race);
			if (!editorID.empty() && settings.RaceProfiles.contains(editorID)) {
				uint32_t idx = GetProfileIndexForRace(race);
				if (idx < profileBaseData.size()) {
					uiPickBase = profileBaseData[idx];
					uiPickBaseLabel = editorID;
				}
			}
		}
	}

	if (uiPickKey.empty() && uiPickBaseIdKey.empty())
		uiPickMessage = T("feature.skin.pick_no_key", "Could not derive a NIF or BaseID key for the target.");
}

void Skin::BeginOverrideEdit(OverrideKind a_kind, const std::string& a_key, const SkinProfile& a_base, const std::string& a_baseLabel, bool a_isNew)
{
	uiOverrideEditorOpen = true;
	uiOverrideEditorIsNew = a_isNew;
	uiOverrideEditorKind = a_kind;
	uiOverrideEditorKey = a_key;
	uiOverrideEditorBase = a_base;
	uiOverrideEditorBaseLabel = a_baseLabel;
	uiOverrideEditorProfile = a_base;

	if (!a_isNew) {
		if (const json* partial = overrideStore.Lookup(a_kind, a_key)) {
			json mergedJson = a_base;
			for (const auto& [field, value] : partial->items()) {
				if (!value.is_null())
					mergedJson[field] = value;
			}
			uiOverrideEditorProfile = mergedJson.get<SkinProfile>();
		}
	}
}

void Skin::DrawNifOverrides()
{
	const ImVec4 okColor{ 0.2f, 1.0f, 0.2f, 1.0f };
	const ImVec4 warnColor{ 1.0f, 0.7f, 0.2f, 1.0f };
	const ImVec4 errColor{ 1.0f, 0.3f, 0.3f, 1.0f };

	if (ImGui::Button(T("feature.skin.pick_crosshair", "Pick Crosshair")))
		ResolveUiPick(PickCrosshairRef());
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.pick_console_ref", "Use Console Selection"))) {
		auto selected = RE::Console::GetSelectedRef();
		ResolveUiPick(selected.get());
	}

	if (!uiPickMessage.empty())
		ImGui::TextColored(errColor, "%s", uiPickMessage.c_str());

	if (uiPickValid) {
		ImGui::Text("%s: %s", T("feature.skin.pick_target", "Target"), uiPickRefLabel.c_str());
		if (!uiPickKey.empty())
			ImGui::Text("%s: %s", T("feature.skin.pick_key", "NIF Key"), uiPickKey.c_str());
		if (!uiPickBaseIdKey.empty())
			ImGui::Text("%s: %s", T("feature.skin.ov_baseid_key", "BaseID"), uiPickBaseIdKey.c_str());
		if (uiPickHasSkin)
			ImGui::TextColored(okColor, "%s", T("feature.skin.pick_has_skin", "Skin: yes"));
		else
			ImGui::TextColored(warnColor, "%s", T("feature.skin.pick_no_skin", "Skin: no (target has no skin material)"));
	}

	ImGui::BeginDisabled(!uiPickValid || !uiPickHasSkin || uiPickKey.empty());
	if (ImGui::Button(T("feature.skin.add_nif_override", "New NIF Override")))
		BeginOverrideEdit(OverrideKind::Nif, uiPickKey, uiPickBase, uiPickBaseLabel, true);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!uiPickValid || !uiPickHasSkin || uiPickBaseIdKey.empty());
	if (ImGui::Button(T("feature.skin.add_baseid_override", "New BaseID Override")))
		BeginOverrideEdit(OverrideKind::BaseId, uiPickBaseIdKey, uiPickBase, uiPickBaseLabel, true);
	ImGui::EndDisabled();

	ImGui::Separator();

	if (overrideStore.Empty()) {
		ImGui::TextDisabled("%s", T("feature.skin.no_overrides", "No overrides loaded."));
	} else {
		OverrideKind eraseKind = OverrideKind::Nif;
		std::string eraseKey;
		OverrideKind editKind = OverrideKind::Nif;
		std::string editKey;

		auto renderRow = [&](OverrideKind a_kind, const std::string& key, const OverrideStore::Entry& entry) {
			ImGui::PushID(static_cast<int>(a_kind));
			ImGui::PushID(key.c_str());
			ImGui::TableNextRow();

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(key.c_str());

			ImGui::TableNextColumn();
			if (a_kind == OverrideKind::BaseId) {
				ImGui::TextUnformatted("BaseID");
			} else {
				const bool isPath = key.find('/') != std::string::npos || key.ends_with(".nif");
				ImGui::TextUnformatted(isPath ? "NIF" : "Node");
			}

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(entry.source.c_str());

			ImGui::TableNextColumn();
			std::string fields;
			for (auto it = entry.partial.begin(); it != entry.partial.end(); ++it) {
				if (!fields.empty())
					fields += ", ";
				fields += it.key();
			}
			ImGui::TextUnformatted(fields.empty() ? "-" : fields.c_str());

			ImGui::TableNextColumn();
			if (ImGui::SmallButton(T("feature.skin.ov_edit", "Edit"))) {
				editKind = a_kind;
				editKey = key;
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(!entry.isUser);
			if (ImGui::SmallButton(T("feature.skin.ov_delete", "Delete"))) {
				eraseKind = a_kind;
				eraseKey = key;
			}
			ImGui::EndDisabled();

			ImGui::PopID();
			ImGui::PopID();
		};

		if (ImGui::BeginTable("##SkinOverrides", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn(T("feature.skin.ov_key", "Key"));
			ImGui::TableSetupColumn(T("feature.skin.ov_type", "Type"));
			ImGui::TableSetupColumn(T("feature.skin.ov_source", "Source"));
			ImGui::TableSetupColumn(T("feature.skin.ov_fields", "Overridden Fields"));
			ImGui::TableSetupColumn("##SkinOverrideActions");
			ImGui::TableHeadersRow();

			for (auto& [key, entry] : overrideStore.nifOverrides)
				renderRow(OverrideKind::Nif, key, entry);
			for (auto& [key, entry] : overrideStore.baseIdOverrides)
				renderRow(OverrideKind::BaseId, key, entry);

			ImGui::EndTable();
		}

		if (!editKey.empty())
			BeginOverrideEdit(editKind, editKey, settings.DefaultProfile, "Default", false);
		if (!eraseKey.empty())
			overrideStore.RemoveOverride(eraseKind, eraseKey);
	}

	if (uiOverrideEditorOpen) {
		ImGui::Separator();
		ImGui::TextUnformatted(uiOverrideEditorIsNew ? T("feature.skin.ov_new_title", "New override") : T("feature.skin.ov_edit_title", "Edit override"));
		ImGui::Text("%s: %s",
			uiOverrideEditorKind == OverrideKind::BaseId ? T("feature.skin.ov_baseid_key", "BaseID") : T("feature.skin.pick_key", "NIF Key"),
			uiOverrideEditorKey.c_str());
		ImGui::Text("%s: %s", T("feature.skin.ov_base", "Base"), uiOverrideEditorBaseLabel.c_str());

		const char* kindName = uiOverrideEditorKind == OverrideKind::BaseId ? "BaseID" : "NIF";
		if (uiOverrideEditorBaseLabel == "Default")
			ImGui::TextColored(warnColor, "Chain: Default -> %s override", kindName);
		else
			ImGui::TextColored(warnColor, "Chain: Default -> Race(%s) -> %s override", uiOverrideEditorBaseLabel.c_str(), kindName);

		DrawProfileSettings(uiOverrideEditorProfile);

		json diff = DiffProfile(uiOverrideEditorBase, uiOverrideEditorProfile);
		std::string diffFields;
		for (auto it = diff.begin(); it != diff.end(); ++it) {
			if (!diffFields.empty())
				diffFields += ", ";
			diffFields += it.key();
		}
		ImGui::TextColored(warnColor, "%s: %s", T("feature.skin.ov_overridden", "Overridden fields"),
			diffFields.empty() ? T("feature.skin.ov_none", "(none)") : diffFields.c_str());

		if (ImGui::Button(T("feature.skin.ov_save", "Save Override"))) {
			if (!diff.empty())
				overrideStore.AddOverride(uiOverrideEditorKind, uiOverrideEditorKey, diff);
			else
				overrideStore.RemoveOverride(uiOverrideEditorKind, uiOverrideEditorKey);
			uiOverrideEditorOpen = false;
		}
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.ov_cancel", "Cancel")))
			uiOverrideEditorOpen = false;
	}
}

void Skin::LoadSkinDetailTexture()
{
	auto device = globals::d3d::device;

	DirectX::ScratchImage image;
	try {
		std::filesystem::path path{ "Data\\Shaders\\Skin\\skin_detail_n.dds" };
		DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
	} catch (const DX::com_exception& e) {
		logger::error("{}", e.what());
		return;
	}

	ID3D11Resource* pResource = nullptr;
	try {
		DX::ThrowIfFailed(CreateTexture(device,
			image.GetImages(), image.GetImageCount(),
			image.GetMetadata(), &pResource));
	} catch (const DX::com_exception& e) {
		logger::error("{}", e.what());
		return;
	}

	texSkinDetail = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texSkinDetail->desc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = {
			.MostDetailedMip = 0,
			.MipLevels = static_cast<UINT>(image.GetMetadata().mipLevels) }
	};
	texSkinDetail->CreateSRV(srvDesc);
}

void Skin::SetupResources()
{
	logger::debug("Loading skin detail texture...");
	LoadSkinDetailTexture();

	PerGeometryCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<PerGeometryData>());

	RebuildProfileData();
	overrideStore.Refresh();

	// Check for Dynamic Wetness availability
	isDynamicWetnessAvailable = SWE::API::Init();
}

void Skin::ReloadSkinDetail()
{
	logger::debug("Reloading skin detail texture...");
	LoadSkinDetailTexture();
}

void Skin::Prepass()
{
	auto context = globals::d3d::context;

	RebuildProfileData();

	const uint currentFrame = globals::state->frameCount;
	if (currentFrame - lastOverrideScanFrame >= 60) {
		lastOverrideScanFrame = currentFrame;
		overrideStore.Refresh();
	}

	if (texSkinDetail) {
		ID3D11ShaderResourceView* srv = texSkinDetail->srv.get();
		context->PSSetShaderResources(72, 1, &srv);
	}
}

struct SKIN_BSLightingShader_SetupMaterial
{
	static void thunk(RE::BSLightingShader* shader, RE::BSLightingShaderMaterialBase const* material)
	{
		func(shader, material);

		auto& skin = globals::features::skin;
		if (skin.loaded) {
			skin.BSLightingShader_SetupMaterial(material);
		}
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void Skin::PostPostLoad()
{
	logger::info("[Advanced Skin] Hooking BSLightingShader::SetupMaterial");
	stl::write_vfunc<0x4, SKIN_BSLightingShader_SetupMaterial>(RE::VTABLE_BSLightingShader[0]);
	Hooks::Install();
}

Skin::SkinData Skin::MakeProfileData(const SkinProfile& a_profile) const
{
	SkinData data{};
	data.skinParams = float4(a_profile.SkinMainRoughness, a_profile.SkinSecondRoughness, a_profile.SkinSpecularTexMultiplier, float(settings.EnableSkin));
	data.skinParams2 = float4(a_profile.SecondarySpecularStrength, settings.ExtraSkinWetness, a_profile.F0, a_profile.BaseColorMultiplier);
	data.skinDetailParams = float4(a_profile.SkinDetailTiling, a_profile.BodyTilingMultiplier, a_profile.SkinDetailStrength, float(a_profile.EnableSkinDetail && settings.EnableSkin));
	data.sssParams = float4(a_profile.Translucency, a_profile.sssWidth, 0.0f, float(a_profile.UseSSS));
	data.fuzzParams = float4(a_profile.FuzzStrength, a_profile.FuzzRoughness, a_profile.FuzzF0, a_profile.ExtraEdgeRoughness);
	data.physicalParams = float4(a_profile.PhysicalMainRoughnessMultiplier, a_profile.PhysicalSecondRoughnessMultiplier, a_profile.PhysicalSpecularStrength, 0.0f);
	data.wetParams = settings.WetParams;
	return data;
}

Skin::SkinData Skin::GetCommonBufferData()
{
	return MakeProfileData(settings.DefaultProfile);
}

void Skin::OverrideStore::Refresh()
{
	auto rootDir = Util::PathHelpers::GetShadersPath() / "Skin" / "Overrides";
	auto userDir = rootDir / "User";

	// Snapshot the current file set + mtimes for both the mod (root) and user (User) directories.
	std::unordered_map<std::string, std::filesystem::file_time_type> current;
	auto scanDir = [&](const std::filesystem::path& dir) {
		std::error_code ec;
		if (!std::filesystem::exists(dir, ec) || ec)
			return;
		for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
			const auto& entry = *it;
			if (!entry.is_regular_file(ec) || ec || entry.path().extension() != ".json")
				continue;
			auto mtime = std::filesystem::last_write_time(entry.path(), ec);
			if (!ec)
				current[entry.path().string()] = mtime;
		}
	};
	scanDir(rootDir);
	scanDir(userDir);

	if (current == fileTimes)
		return;

	// Parse mod (root) files first, then user files; user entries win on key conflicts.
	std::unordered_map<std::string, Entry> modNif;
	std::unordered_map<std::string, Entry> modBaseId;
	std::unordered_map<std::string, json> userNif;
	std::unordered_map<std::string, json> userBaseId;
	auto parseDir = [&](const std::filesystem::path& dir, bool isUser) {
		std::error_code ec;
		if (!std::filesystem::exists(dir, ec) || ec)
			return;
		for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
			const auto& dentry = *it;
			if (!dentry.is_regular_file(ec) || ec || dentry.path().extension() != ".json")
				continue;
			const std::string filePath = dentry.path().string();
			const std::string fileName = dentry.path().filename().string();
			try {
				std::ifstream file(filePath);
				if (!file.is_open())
					continue;
				json j;
				file >> j;
				if (!j.is_object())
					continue;
				if (j.contains("nif") && j["nif"].is_object()) {
					for (auto& [key, value] : j["nif"].items()) {
						if (!value.is_object())
							continue;
						auto nkey = NormalizeNifKey(key);
						if (isUser)
							userNif[nkey] = value;
						else
							modNif[nkey] = Entry{ fileName, false, value };
					}
				}
				if (j.contains("baseid") && j["baseid"].is_object()) {
					for (auto& [key, value] : j["baseid"].items()) {
						if (!value.is_object())
							continue;
						auto nkey = NormalizeBaseIdKey(key);
						if (isUser)
							userBaseId[nkey] = value;
						else
							modBaseId[nkey] = Entry{ fileName, false, value };
					}
				}
			} catch (const std::exception& e) {
				logger::error("[Advanced Skin] Failed to parse override file {}: {}", filePath, e.what());
			}
		}
	};
	parseDir(rootDir, false);
	parseDir(userDir, true);

	userNifOverrides = std::move(userNif);
	userBaseIdOverrides = std::move(userBaseId);

	nifOverrides.clear();
	for (auto& [key, entry] : modNif)
		nifOverrides[key] = std::move(entry);
	for (auto& [key, partial] : userNifOverrides)
		nifOverrides[key] = Entry{ "User", true, partial };

	baseIdOverrides.clear();
	for (auto& [key, entry] : modBaseId)
		baseIdOverrides[key] = std::move(entry);
	for (auto& [key, partial] : userBaseIdOverrides)
		baseIdOverrides[key] = Entry{ "User", true, partial };

	fileTimes = std::move(current);
	++revision;
	logger::info("[Advanced Skin] Loaded {} NIF / {} BaseID skin override(s) (revision {})", nifOverrides.size(), baseIdOverrides.size(), revision);
}

const json* Skin::OverrideStore::Lookup(OverrideKind a_kind, const std::string& a_key) const
{
	const auto& map = a_kind == OverrideKind::Nif ? nifOverrides : baseIdOverrides;
	auto it = map.find(a_key);
	return it != map.end() ? &it->second.partial : nullptr;
}

void Skin::OverrideStore::SaveUserOverrides()
{
	auto userDir = Util::PathHelpers::GetShadersPath() / "Skin" / "Overrides" / "User";
	std::error_code ec;
	auto userFile = userDir / "SkinOverrides.user.json";

	if (userNifOverrides.empty() && userBaseIdOverrides.empty()) {
		std::filesystem::remove(userFile, ec);
		return;
	}

	std::filesystem::create_directories(userDir, ec);
	json j;
	if (!userNifOverrides.empty()) {
		j["nif"] = json::object();
		for (const auto& [key, partial] : userNifOverrides)
			j["nif"][key] = partial;
	}
	if (!userBaseIdOverrides.empty()) {
		j["baseid"] = json::object();
		for (const auto& [key, partial] : userBaseIdOverrides)
			j["baseid"][key] = partial;
	}

	std::ofstream file(userFile);
	if (!file.is_open()) {
		logger::error("[Advanced Skin] Failed to write user override file: {}", userFile.string());
		return;
	}
	file << j.dump(1);
	logger::info("[Advanced Skin] Saved {} NIF / {} BaseID user override(s)", userNifOverrides.size(), userBaseIdOverrides.size());
}

void Skin::OverrideStore::AddOverride(OverrideKind a_kind, const std::string& a_key, const json& a_partial)
{
	if (a_kind == OverrideKind::Nif)
		userNifOverrides[a_key] = a_partial;
	else
		userBaseIdOverrides[a_key] = a_partial;
	SaveUserOverrides();
	Refresh();
}

void Skin::OverrideStore::RemoveOverride(OverrideKind a_kind, const std::string& a_key)
{
	const bool erased = a_kind == OverrideKind::Nif ? userNifOverrides.erase(a_key) : userBaseIdOverrides.erase(a_key);
	if (!erased)
		return;
	SaveUserOverrides();
	Refresh();
}

Skin::SkinData Skin::ApplyOverride(const SkinProfile& a_base, const json& a_override) const
{
	json mergedJson = a_base;
	for (const auto& [key, value] : a_override.items()) {
		if (!value.is_null())
			mergedJson[key] = value;
	}
	return MakeProfileData(mergedJson.get<SkinProfile>());
}

void Skin::RebuildProfileData()
{
	if (profileBindingsDirty) {
		profileNameToIndex.clear();
		raceProfileIndex.clear();
	}

	std::vector<SkinData> newData;
	std::vector<SkinProfile> newBaseData;
	newData.reserve(settings.Profiles.size() + 1);
	newBaseData.reserve(settings.Profiles.size() + 1);
	newData.push_back(MakeProfileData(settings.DefaultProfile));
	newBaseData.push_back(settings.DefaultProfile);

	for (const auto& [name, profile] : settings.Profiles) {
		if (profileBindingsDirty)
			profileNameToIndex[name] = static_cast<uint32_t>(newData.size());
		newData.push_back(MakeProfileData(profile));
		newBaseData.push_back(profile);
	}

	profileBindingsDirty = false;

	const bool changed = newData.size() != profileData.size() ||
	                     std::memcmp(newData.data(), profileData.data(), newData.size() * sizeof(SkinData)) != 0;
	profileData = std::move(newData);
	profileBaseData = std::move(newBaseData);
	if (changed)
		++profileDataRevision;
}

void Skin::InvalidateProfileBindings()
{
	profileBindingsDirty = true;
}

uint32_t Skin::GetProfileIndexForRace(const RE::TESRace* a_race)
{
	if (!a_race || settings.RaceProfiles.empty())
		return 0;

	if (auto cached = raceProfileIndex.find(a_race->formID); cached != raceProfileIndex.end())
		return cached->second;

	uint32_t index = 0;
	if (auto binding = settings.RaceProfiles.find(Util::GetFormEditorID(a_race)); binding != settings.RaceProfiles.end()) {
		if (auto profile = profileNameToIndex.find(binding->second); profile != profileNameToIndex.end())
			index = profile->second;
	}

	raceProfileIndex[a_race->formID] = index;
	return index;
}

void Skin::LoadSettings(json& o_json)
{
	settings = o_json;
	if (!o_json.contains("DefaultProfile"))  // legacy flat layout
		settings.DefaultProfile = o_json.get<SkinProfile>();
	InvalidateProfileBindings();
	RebuildProfileData();
}

void Skin::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Skin::RestoreDefaultSettings()
{
	settings = {};
	uiSelectedProfile.clear();
	uiPendingRace.clear();
	InvalidateProfileBindings();
	RebuildProfileData();
}

// By PO3
// https://github.com/powerof3/Splashes-of-Skyrim/blob/master/src/Manager.cpp
float Skin::GetWaterHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos)
{
	float waterHeight = -RE::NI_INFINITY;

	if (const auto waterManager = RE::TESWaterSystem::GetSingleton()) {
		waterHeight = a_ref->GetWaterHeight();

		if (waterHeight != -RE::NI_INFINITY) {
			return waterHeight;
		}

		const auto get_nearest_water_object_height = [&]() {
			for (const auto& waterObject : waterManager->waterObjects) {
				if (waterObject) {
					for (const auto& bound : waterObject->multiBounds) {
						if (bound) {
							if (auto size{ bound->size }; size.z <= 10.0f) {  //avoid sloped water
								auto center{ bound->center };
								const auto boundMin = center - size;
								const auto boundMax = center + size;
								if (!(a_pos.x < boundMin.x || a_pos.x > boundMax.x || a_pos.y < boundMin.y || a_pos.y > boundMax.y)) {
									return center.z;
								}
							}
						}
					}
				}
			}

			return -RE::NI_INFINITY;
		};

		waterHeight = get_nearest_water_object_height();
	}

	return waterHeight;
}

float4 Skin::GetWetness(RE::BSGeometry* geometry, uint32_t& a_profileIndex)
{
	float4 wetness = float4(0.0f, 0.0f, 0.0f, 0.0f);
	a_profileIndex = 0;
	auto userData = geometry->GetUserData();
	if (userData && userData->formType == RE::FormType::ActorCharacter) {
		auto actor = static_cast<RE::Character*>(userData);
		const uint32_t actorFormID = userData->formID;
		const uint currentFrame = globals::state->frameCount;

		if (actorWetnessMap.size() > 1024) {
			actorWetnessMap.clear();
		}

		auto [it, inserted] = actorWetnessMap.try_emplace(actorFormID);
		auto& cached = it->second;
		if (!inserted && cached.frameCount == currentFrame) {
			a_profileIndex = cached.profileIndex;
			return cached.wetness;
		}
		cached.frameCount = currentFrame;
		cached.profileIndex = GetProfileIndexForRace(actor->GetRace());
		a_profileIndex = cached.profileIndex;

		const float positionZ = actor->GetPositionZ();
		wetness.z = positionZ;
		if (settings.UseDynamicWetness && isDynamicWetnessAvailable) {
			float dynamicWetness = SWE::API::GetFinalWetness(actor);
			wetness.x = dynamicWetness;
		} else {
			const float stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
			const float permanentStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
			const float temporaryStamina = actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kStamina);
			const float maxStamina = std::max(permanentStamina + temporaryStamina, 1.0f);
			const float staminaPercentage = actor->IsDead() ? 1.0f : (stamina / maxStamina);
			const float sweatRange = settings.StartSweat - settings.FullSweat;
			wetness.x = (std::abs(sweatRange) < 1e-5f)             ? 0.0f :
			            (staminaPercentage >= settings.StartSweat) ? 0.0f :
			            (staminaPercentage <= settings.FullSweat)  ? 1.0f :
			                                                         (settings.StartSweat - staminaPercentage) / sweatRange;
		}
		if (actor->IsInWater()) {
			wetness.y = 2.0f;
			const float waterHeight = GetWaterHeight(userData, actor->GetPosition());
			wetness.w = std::max(0.0f, waterHeight - positionZ);
		} else {
			wetness.y = 0.0f;
			wetness.w = 0.0f;
		}

		if (inserted) {
			cached.wetness = wetness;
		} else {
			const float fadeTime = std::max(settings.WetFadeTime, 0.001f);
			if (cached.wetness.x < wetness.x) {
				cached.wetness.x = wetness.x;
			} else if (cached.wetness.x > wetness.x) {
				cached.wetness.x -= *globals::game::deltaTime / fadeTime;
				cached.wetness.x = std::max(cached.wetness.x, 0.0f);
			}
			wetness.x = cached.wetness.x;

			if (cached.wetness.y < wetness.y) {
				cached.wetness.y = wetness.y;
				if (cached.wetness.w < wetness.w) {
					cached.wetness.w = wetness.w;
				} else {
					wetness.w = cached.wetness.w;
				}
			} else if (cached.wetness.y > wetness.y) {
				cached.wetness.y -= *globals::game::deltaTime / fadeTime;
				cached.wetness.y = std::max(cached.wetness.y, 0.0f);
				wetness.y = cached.wetness.y;
				if (wetness.y == 0.0f) {
					wetness.w = 0.0f;
					cached.wetness.w = 0.0f;
				} else if (cached.wetness.w < wetness.w) {
					cached.wetness.w = wetness.w;
				} else {
					wetness.w = cached.wetness.w;
				}
			} else if (cached.wetness.w < wetness.w) {
				cached.wetness.w = wetness.w;
			} else {
				wetness.w = cached.wetness.w;
			}
			cached.wetness = wetness;
		}
	}
	return wetness;
}

struct SkinExtendedRendererState
{
	uint32_t PSResourceModifiedBits = 0;
	std::array<ID3D11ShaderResourceView*, 2> PSTexture;

	void SetExtraSkinPSTexture(RE::BSGraphics::Texture* newTexture, RE::BSGraphics::Texture* newTexture2)
	{
		{
			PSTexture = {
				newTexture ? newTexture->resourceView : nullptr,
				newTexture2 ? newTexture2->resourceView : nullptr
			};
			PSResourceModifiedBits = 1;
		}
	}

	SkinExtendedRendererState()
	{
		PSTexture.fill(nullptr);
	}
} skinExtendedRendererState;

void Skin::SetupExtraTexture(RE::BSLightingShaderMaterialBase const* material, RE::BSTextureSet* inTextureSet, uint32_t i_hashKey)
{
	if (!inTextureSet || material->normalTexture == nullptr) {
		logger::error("[Advanced Skin] SetupExtraTexture : Texture set is null for material: {}", i_hashKey);
		return;
	}

	uint32_t hashKey = 0;
	hashKey = material->hashKey;
	if (hashKey == 0 || hashKey != i_hashKey) {
		logger::error("[Advanced Skin] SetupExtraTexture : Invalid hash key for material: {}", i_hashKey);
		return;
	}

	const char extraTextureName[] = "_rfaos.dds";
	const char wetnessTextureName[] = "_wet.dds";
	const char* workingNormalPath = nullptr;
	const char* workingSpecularPath = nullptr;
	auto workingMaterial = static_cast<const RE::BSLightingShaderMaterialBase*>(material);
	auto hasSpecular = workingMaterial->specularBackLightingTexture != nullptr;

	auto graphicsState = globals::game::graphicsState;
	const auto& stateData = graphicsState->GetRuntimeData();

	if (hasSpecular) {
		if (auto specularPath = inTextureSet->GetTexturePath(RE::BSTextureSet::Texture::kSpecular)) {
			workingSpecularPath = specularPath;
		}
	}
	if (auto normalPath = inTextureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal)) {
		workingNormalPath = normalPath;
	} else {
		logger::error("[Advanced Skin] SetupExtraTexture : No specular or normal texture found in texture set from material: {}", hashKey);
		auto& workingExtraPtr = skinExtraTextures.try_emplace(hashKey).first->second;
		workingExtraPtr.rfaosTexture = stateData.defaultTextureBlack;
		workingExtraPtr.wetnessTexture = stateData.defaultTextureBlack;
		workingExtraPtr.extraTexturePath = "";
		workingExtraPtr.wetnessTexturePath = "";
		workingExtraPtr.hasExtraTexture = false;
		workingExtraPtr.hasWetnessTexture = false;
		return;
	}

	const char* foundPath = nullptr;
	std::string extraTexturePath = "";
	std::string wetnessTexturePath = "";

	auto findIgnoreCase = [](std::string_view str, std::string_view pattern) -> size_t {
		auto it = std::search(str.begin(), str.end(), pattern.begin(), pattern.end(),
			[](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); });
		return it == str.end() ? std::string_view::npos : std::distance(str.begin(), it);
	};

	auto tryReplaceSuffix = [&](const char* basePath, std::string_view suffix) -> bool {
		auto pos = findIgnoreCase(basePath, suffix);
		if (pos == std::string_view::npos)
			return false;
		extraTexturePath = std::string(basePath);
		wetnessTexturePath = std::string(basePath);
		extraTexturePath.replace(pos, suffix.size(), extraTextureName);
		wetnessTexturePath.replace(pos, suffix.size(), wetnessTextureName);
		foundPath = basePath;
		return true;
	};

	if (hasSpecular && workingSpecularPath) {
		tryReplaceSuffix(workingSpecularPath, "_s.dds");
	}

	if (!foundPath && workingNormalPath) {
		if (!tryReplaceSuffix(workingNormalPath, "_n.dds")) {
			if (!tryReplaceSuffix(workingNormalPath, "_msn.dds")) {
				tryReplaceSuffix(workingNormalPath, ".dds");
			}
		}
	}

	logger::debug("[Advanced Skin] SetupExtraTexture : Extra texture path: {} for {}", extraTexturePath, foundPath ? foundPath : "(none)");
	logger::debug("[Advanced Skin] SetupExtraTexture : Wetness texture path: {} for {}", wetnessTexturePath, foundPath ? foundPath : "(none)");

	auto& workingExtraPtr = skinExtraTextures.try_emplace(hashKey).first->second;
	workingExtraPtr.rfaosTexture = stateData.defaultTextureWhite;
	workingExtraPtr.wetnessTexture = stateData.defaultTextureWhite;
	workingExtraPtr.extraTexturePath = extraTexturePath;
	workingExtraPtr.wetnessTexturePath = wetnessTexturePath;

	inTextureSet->SetTexturePath(RE::BSTextureSet::Texture::kEnvironment, workingExtraPtr.extraTexturePath.c_str());
	inTextureSet->SetTexturePath(RE::BSTextureSet::Texture::kMultilayer, workingExtraPtr.wetnessTexturePath.c_str());
	inTextureSet->SetTexture(RE::BSTextureSet::Texture::kEnvironment, workingExtraPtr.rfaosTexture);
	inTextureSet->SetTexture(RE::BSTextureSet::Texture::kMultilayer, workingExtraPtr.wetnessTexture);

	workingExtraPtr.hasExtraTexture = workingExtraPtr.rfaosTexture != nullptr && !workingExtraPtr.extraTexturePath.empty() && workingExtraPtr.rfaosTexture != stateData.defaultTextureBlack;
	workingExtraPtr.hasWetnessTexture = workingExtraPtr.wetnessTexture != nullptr && !workingExtraPtr.wetnessTexturePath.empty() && workingExtraPtr.wetnessTexture != stateData.defaultTextureBlack;

	if (workingExtraPtr.hasExtraTexture || workingExtraPtr.hasWetnessTexture) {
		logger::debug("[Advanced Skin] SetupExtraTexture : Extra texture set with hash key: {}", hashKey);
	} else {
		logger::debug("[Advanced Skin] SetupExtraTexture : Failed to set extra texture for material: {}", hashKey);
	}
}

void Skin::BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material)
{
	auto materialFeature = material->GetFeature();
	if (materialFeature != RE::BSShaderMaterial::Feature::kFaceGen &&
		materialFeature != RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
		return;
	}

	auto materialTextureSet = material->textureSet.get();

	uint32_t hashKey = 0;
	hashKey = material->hashKey;
	if (hashKey == 0) {
		logger::error("[Advanced Skin] BSLightingShader_SetupMaterial : Invalid hash key for material: {}", static_cast<int>(materialFeature));
		return;
	}

	if (!skinExtraTextures.contains(hashKey)) {
		// logger::debug("[Advanced Skin] BSLightingShader_SetupMaterial : Setting up extra texture for material: {}", static_cast<int>(materialFeature));
		globals::features::skin.SetupExtraTexture(material, materialTextureSet, hashKey);
	}

	auto graphicsState = globals::game::graphicsState;
	const auto& workingExtraPtr = skinExtraTextures[hashKey];

	if (workingExtraPtr.hasExtraTexture || workingExtraPtr.hasWetnessTexture) {
		skinExtendedRendererState.SetExtraSkinPSTexture(workingExtraPtr.rfaosTexture->rendererTexture, workingExtraPtr.wetnessTexture->rendererTexture);
	} else {
		skinExtendedRendererState.SetExtraSkinPSTexture(graphicsState->GetRuntimeData().defaultTextureBlack->rendererTexture, graphicsState->GetRuntimeData().defaultTextureBlack->rendererTexture);
	}
}

void Skin::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	auto context = globals::d3d::context;

	if (settings.EnableSkin) {
		if (profileData.empty())
			RebuildProfileData();

		auto geometry = a_pass->geometry;
		uint32_t profileIndex = 0;
		float4 wetness = GetWetness(geometry, profileIndex);
		if (profileIndex >= profileData.size())
			profileIndex = 0;

		SkinData perGeometryProfile = profileData[profileIndex];

		if (!overrideStore.Empty()) {
			if (geometryOverrideCache.size() > 4096)
				geometryOverrideCache.clear();

			auto& entry = geometryOverrideCache[geometry];
			if (!entry.initialized) {
				entry.initialized = true;
				entry.nifKey = DeriveNifKey(geometry);
				entry.baseIdKey = DeriveBaseIdKey(geometry);
			}
			if (entry.profileIndex != profileIndex || entry.baseProfileRevision != profileDataRevision || entry.overrideRevision != overrideStore.revision) {
				entry.profileIndex = profileIndex;
				entry.baseProfileRevision = profileDataRevision;
				entry.overrideRevision = overrideStore.revision;
				entry.merged = profileData[profileIndex];

				const json* nifPartial = entry.nifKey.empty() ? nullptr : overrideStore.Lookup(OverrideKind::Nif, entry.nifKey);
				const json* baseIdPartial = entry.baseIdKey.empty() ? nullptr : overrideStore.Lookup(OverrideKind::BaseId, entry.baseIdKey);
				if ((nifPartial || baseIdPartial) && profileIndex < profileBaseData.size()) {
					// Combine both partials; BaseID (more specific) wins on field conflicts.
					json combined = json::object();
					if (nifPartial)
						combined = *nifPartial;
					if (baseIdPartial) {
						for (const auto& [field, value] : baseIdPartial->items()) {
							if (!value.is_null())
								combined[field] = value;
						}
					}
					entry.merged = ApplyOverride(profileBaseData[profileIndex], combined);
				}
			}
			perGeometryProfile = entry.merged;
		}

		if (currentWetness != wetness || currentProfileIndex != profileIndex ||
			currentProfileRevision != profileDataRevision || currentOverrideRevision != overrideStore.revision) {
			currentWetness = wetness;
			currentProfileIndex = profileIndex;
			currentProfileRevision = profileDataRevision;
			currentOverrideRevision = overrideStore.revision;
			PerGeometryData perGeometryData{};
			perGeometryData.skinPerGeometry = wetness;
			perGeometryData.profile = perGeometryProfile;
			PerGeometryCB->Update(perGeometryData);
		}

		ID3D11Buffer* buffer = { PerGeometryCB->CB() };
		context->PSSetConstantBuffers(7, 1, &buffer);
	}
}

void Skin::SetShaderResources(ID3D11DeviceContext* a_context)
{
	if (skinExtendedRendererState.PSResourceModifiedBits != 0) {
		a_context->PSSetShaderResources(71, 1, &skinExtendedRendererState.PSTexture.at(0));
		a_context->PSSetShaderResources(74, 1, &skinExtendedRendererState.PSTexture.at(1));
	}
	skinExtendedRendererState.PSResourceModifiedBits = 0;
}

void Skin::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto& skin = globals::features::skin;
	skin.BSLightingShader_SetupGeometry(Pass);
	return func(This, Pass, RenderFlags);
}
