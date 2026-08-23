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
	EnableLegacyExtraTextureDiscovery,
	ExtraSkinWetness,
	WetFadeTime,
	StartSweat,
	FullSweat,
	WetParams,
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
		constexpr std::string_view kDataMeshesPrefix = "data/meshes/";
		if (a_key.starts_with(kDataMeshesPrefix))
			a_key.erase(0, kDataMeshesPrefix.size());
		else if (a_key.starts_with(kMeshesPrefix))
			a_key.erase(0, kMeshesPrefix.size());
		else if (const auto pos = a_key.rfind("/meshes/"); pos != std::string::npos)
			a_key.erase(0, pos + std::string_view("/meshes/").size());
		return a_key;
	}

	std::string NormalizeIdentifier(std::string a_key)
	{
		std::transform(a_key.begin(), a_key.end(), a_key.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_key;
	}

	/** @brief Normalizes texture paths while retaining the textures/ prefix used by the engine. */
	std::string NormalizeTextureKey(std::string a_key)
	{
		a_key = NormalizeIdentifier(std::move(a_key));
		std::replace(a_key.begin(), a_key.end(), '\\', '/');
		constexpr std::string_view kDataPrefix = "data/";
		if (a_key.starts_with(kDataPrefix))
			a_key.erase(0, kDataPrefix.size());
		else if (const auto pos = a_key.rfind("/textures/"); pos != std::string::npos)
			a_key.erase(0, pos + 1);
		return a_key;
	}

	Skin::TextureChannel ParseTextureChannel(const json& a_object, const char* a_key, const std::string& a_source)
	{
		Skin::TextureChannel channel;
		if (!a_object.contains(a_key))
			return channel;
		const auto& value = a_object[a_key];
		if (value.is_null()) {
			channel.state = Skin::TextureChannel::State::Disabled;
		} else if (value.is_string()) {
			channel.path = NormalizeTextureKey(value.get<std::string>());
			channel.state = channel.path.empty() ? Skin::TextureChannel::State::Disabled : Skin::TextureChannel::State::Path;
		} else {
			logger::warn("[Advanced Skin] Ignoring non-string texture channel '{}': {}", a_key, a_source);
		}
		return channel;
	}

	Skin::TextureMaterial ParseTextureMaterial(const json& a_object, const std::string& a_source)
	{
		Skin::TextureMaterial material;
		if (!a_object.is_object())
			return material;
		for (auto it = a_object.begin(); it != a_object.end(); ++it) {
			if (it.key() != "rfaos" && it.key() != "wetness")
				logger::warn("[Advanced Skin] Ignoring unknown texture channel '{}': {}", it.key(), a_source);
		}
		material.rfaos = ParseTextureChannel(a_object, "rfaos", a_source);
		material.wetness = ParseTextureChannel(a_object, "wetness", a_source);
		return material;
	}

	void ApplyTextureChannel(std::string& a_path, const Skin::TextureChannel& a_channel)
	{
		if (a_channel.state == Skin::TextureChannel::State::Disabled)
			a_path.clear();
		else if (a_channel.state == Skin::TextureChannel::State::Path)
			a_path = a_channel.path;
	}

	bool GlobMatch(std::string_view a_pattern, std::string_view a_value)
	{
		size_t pattern = 0;
		size_t value = 0;
		size_t star = std::string_view::npos;
		size_t retry = 0;
		while (value < a_value.size()) {
			if (pattern < a_pattern.size() && (a_pattern[pattern] == '?' || a_pattern[pattern] == a_value[value])) {
				++pattern;
				++value;
			} else if (pattern < a_pattern.size() && a_pattern[pattern] == '*') {
				star = pattern++;
				retry = value;
			} else if (star != std::string_view::npos) {
				pattern = star + 1;
				value = ++retry;
			} else {
				return false;
			}
		}
		while (pattern < a_pattern.size() && a_pattern[pattern] == '*')
			++pattern;
		return pattern == a_pattern.size();
	}

	std::optional<Skin::OverrideStore::Domain> ParseDomain(std::string a_value)
	{
		a_value = NormalizeIdentifier(std::move(a_value));
		if (a_value == "surface")
			return Skin::OverrideStore::Domain::Surface;
		if (a_value == "appearance")
			return Skin::OverrideStore::Domain::Appearance;
		if (a_value == "local")
			return Skin::OverrideStore::Domain::Local;
		return std::nullopt;
	}

	std::string GetFormSelectorIdentity(const RE::TESForm* a_form)
	{
		if (!a_form)
			return {};
		std::string editorID = Util::GetFormEditorID(a_form);
		return editorID.empty() ? std::string{} : NormalizeIdentifier(std::move(editorID));
	}

	bool ScenegraphContains(RE::NiAVObject* a_root, RE::BSGeometry* a_geometry)
	{
		if (!a_root || !a_geometry)
			return false;
		for (RE::NiAVObject* current = a_geometry; current; current = current->parent) {
			if (current == a_root)
				return true;
		}
		return false;
	}

	/** @brief Cheap draw-time identity. Full strings/classifiers are rebuilt only when this changes. */
	uint64_t MakeGeometryContextIdentity(RE::BSGeometry* a_geometry, RE::BSLightingShaderMaterialBase const* a_material)
	{
		size_t hash = reinterpret_cast<size_t>(a_geometry);
		auto combine = [&](size_t value) {
			hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2);
		};
		combine(a_geometry ? reinterpret_cast<size_t>(a_geometry->parent) : 0);
		combine(reinterpret_cast<size_t>(a_material));
		combine(a_material ? a_material->hashKey : 0);
		combine(a_material ? reinterpret_cast<size_t>(a_material->textureSet.get()) : 0);
		combine(a_material ? reinterpret_cast<size_t>(a_material->diffuseTexture.get()) : 0);
		combine(a_material ? reinterpret_cast<size_t>(a_material->normalTexture.get()) : 0);
		combine(a_material ? reinterpret_cast<size_t>(a_material->specularBackLightingTexture.get()) : 0);
		if (auto* userData = a_geometry ? a_geometry->GetUserData() : nullptr) {
			combine(reinterpret_cast<size_t>(userData));
			combine(userData->formID);
			if (userData->formType == RE::FormType::ActorCharacter) {
				auto* actor = static_cast<RE::Character*>(userData);
				auto* npc = actor->GetActorBase();
				auto* race = actor->GetRace();
				combine(reinterpret_cast<size_t>(npc));
				combine(npc ? npc->formID : 0);
				combine(reinterpret_cast<size_t>(race));
				combine(race ? race->formID : 0);
				combine(npc ? static_cast<size_t>(npc->GetSex()) : 0);
				combine(reinterpret_cast<size_t>(actor->GetActorRuntimeData().biped.get()));
			}
		}
		return static_cast<uint64_t>(hash);
	}

	RE::BSLightingShaderMaterialBase const* GetLightingMaterial(RE::BSGeometry* a_geometry)
	{
		if (!a_geometry)
			return nullptr;
		auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
		if (!property)
			return nullptr;
		auto* material = property->GetBaseMaterial();
		if (!material || material->GetType() != RE::BSShaderMaterial::Type::kLighting)
			return nullptr;
		return static_cast<RE::BSLightingShaderMaterialBase const*>(material);
	}

	std::string GetTextureKey(RE::BSLightingShaderMaterialBase const* a_material, RE::BSTextureSet::Texture a_slot)
	{
		if (!a_material || !a_material->textureSet)
			return {};
		const char* path = a_material->textureSet->GetTexturePath(a_slot);
		return path ? NormalizeTextureKey(path) : std::string{};
	}

	std::string MakeGeometrySignature(RE::BSGeometry* a_geometry, RE::BSLightingShaderMaterialBase const* a_material)
	{
		if (!a_geometry)
			return {};
		if (!a_material)
			a_material = GetLightingMaterial(a_geometry);
		const char* name = a_geometry->name.c_str();
		return std::format("{}|{}|{}", NormalizeIdentifier(name ? name : ""),
			GetTextureKey(a_material, RE::BSTextureSet::Texture::kDiffuse),
			GetTextureKey(a_material, RE::BSTextureSet::Texture::kNormal));
	}

	std::string DeriveModelNifKey(RE::BSGeometry* a_geometry)
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
		return NormalizeNifKey(key);
	}

	/** @brief Legacy NIF identity: base object MODL, then geometry name. */
	std::string DeriveNifKey(RE::BSGeometry* a_geometry)
	{
		std::string key = DeriveModelNifKey(a_geometry);
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
	if (ImGui::Checkbox(T("feature.skin.enable_legacy_extra_texture_discovery", "Enable legacy _rfaos/_wet filename discovery"),
			&settings.EnableLegacyExtraTextureDiscovery)) {
		skinExtraTextures.clear();
		configuredExtraTextures.clear();
		geometryOverrideCache.clear();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextWrapped("%s", T("feature.skin.legacy_extra_texture_discovery_warning",
									 "Compatibility only. Disabled by default; prefer explicit Surface/Local material-instance textures."));
	}

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

	DrawProfileSettings(uiSelectedProfile.empty() ? settings.DefaultProfile : settings.Profiles[uiSelectedProfile], "Profile");
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

void Skin::DrawProfileSettings(SkinProfile& a_profile, const char* a_id)
{
	ImGui::PushID(a_id);

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

	ImGui::PopID();
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

std::vector<std::string> Skin::DeriveNifKeysForRef(RE::TESObjectREFR* a_ref) const
{
	std::vector<std::string> keys;
	auto addKey = [&](std::string a_key) {
		a_key = NormalizeNifKey(a_key);
		if (a_key.empty())
			return;
		if (std::find(keys.begin(), keys.end(), a_key) == keys.end())
			keys.push_back(std::move(a_key));
	};

	// The reference's own model path is the primary key for statics, armor, weapons...
	if (a_ref) {
		if (auto* base = a_ref->GetObjectReference()) {
			if (auto* model = base->As<RE::TESModel>()) {
				if (const char* path = model->GetModel())
					addKey(path);
			}
		}
	}

	// Actors are made of several geometries (body, hands, feet, armor addons, ...).
	// Prefer the captured source NIF, then retain the legacy MODL/shape key.
	if (auto* root = a_ref ? a_ref->Get3D() : nullptr) {
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
			addKey(ResolveNifOrigin(a_geometry, GetLightingMaterial(a_geometry)));
			addKey(DeriveNifKey(a_geometry));
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}

	return keys;
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
	uiPickNifKeys.clear();
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

	uiPickNifKeys = DeriveNifKeysForRef(a_ref);
	if (!uiPickNifKeys.empty())
		uiPickKey = uiPickNifKeys.front();
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

	uiOverridePreviewPartial = DiffProfile(uiOverrideEditorBase, uiOverrideEditorProfile);
	++uiOverridePreviewRevision;
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
		if (!uiPickNifKeys.empty()) {
			if (uiPickNifKeys.size() == 1) {
				ImGui::Text("%s: %s", T("feature.skin.pick_key", "NIF Key"), uiPickKey.c_str());
			} else {
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
				if (ImGui::BeginCombo(T("feature.skin.pick_key", "NIF Key"), uiPickKey.c_str())) {
					for (const auto& nifKey : uiPickNifKeys) {
						if (ImGui::Selectable(nifKey.c_str(), nifKey == uiPickKey))
							uiPickKey = nifKey;
					}
					ImGui::EndCombo();
				}
			}
		}
		if (!uiPickBaseIdKey.empty())
			ImGui::Text("%s: %s", T("feature.skin.ov_baseid_key", "BaseID"), uiPickBaseIdKey.c_str());
		if (uiPickHasSkin) {
			ImGui::TextColored(okColor, "%s", T("feature.skin.pick_has_skin", "Skin: yes"));
			ImGui::Text("%s: %s", T("feature.skin.ov_base", "Base"), uiPickBaseLabel.c_str());
		} else
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

		if (!editKey.empty()) {
			SkinProfile base = settings.DefaultProfile;
			std::string baseLabel = "Default";

			// Re-resolve the base for BaseID overrides from the NPC's race so the
			// editor matches the runtime merge chain instead of always assuming Default.
			if (editKind == OverrideKind::BaseId) {
				try {
					const RE::FormID formID = static_cast<RE::FormID>(std::stoul(editKey, nullptr, 16));
					if (auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(formID)) {
						if (auto* race = npc->race) {
							auto editorID = Util::GetFormEditorID(race);
							if (!editorID.empty() && settings.RaceProfiles.contains(editorID)) {
								const uint32_t idx = GetProfileIndexForRace(race);
								if (idx < profileBaseData.size()) {
									base = profileBaseData[idx];
									baseLabel = editorID;
								}
							}
						}
					}
				} catch (const std::exception&) {
					// Malformed key; fall back to Default.
				}
			}

			BeginOverrideEdit(editKind, editKey, base, baseLabel, false);
		}
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

		DrawProfileSettings(uiOverrideEditorProfile, "OverrideEditor");

		json diff = DiffProfile(uiOverrideEditorBase, uiOverrideEditorProfile);
		if (diff != uiOverridePreviewPartial) {
			uiOverridePreviewPartial = diff;
			++uiOverridePreviewRevision;
		}
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
			uiOverridePreviewPartial = json::object();
			++uiOverridePreviewRevision;
		}
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.ov_cancel", "Cancel"))) {
			uiOverrideEditorOpen = false;
			uiOverridePreviewPartial = json::object();
			++uiOverridePreviewRevision;
		}
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
		const uint32_t previousRevision = overrideStore.revision;
		overrideStore.Refresh();
		if (overrideStore.revision != previousRevision)
			configuredExtraTextures.clear();
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
	logger::info("[Advanced Skin] Hooking BSLightingShader and BSStream");
	stl::write_vfunc<0x4, SKIN_BSLightingShader_SetupMaterial>(RE::VTABLE_BSLightingShader[0]);
	Hooks::Install();
}

void Skin::RecordNifOrigins(RE::BSStream* a_stream, const char* a_path)
{
	if (!a_stream)
		return;
	const char* loadedPath = a_stream->inputFilePath[0] ? a_stream->inputFilePath : a_path;
	const std::string nifKey = loadedPath ? NormalizeNifKey(loadedPath) : std::string{};
	if (nifKey.empty())
		return;

	std::vector<std::pair<RE::BSGeometry*, std::string>> loadedGeometries;
	std::unordered_set<RE::BSGeometry*> visited;
	auto record = [&](RE::BSGeometry* a_geometry) {
		if (a_geometry && visited.insert(a_geometry).second)
			loadedGeometries.emplace_back(a_geometry, MakeGeometrySignature(a_geometry, nullptr));
	};
	for (auto& object : a_stream->topObjects) {
		if (!object)
			continue;
		if (auto* geometry = object->AsGeometry()) {
			record(geometry);
		} else if (auto* node = object->AsNode()) {
			RE::BSVisit::TraverseScenegraphGeometries(node, [&](RE::BSGeometry* a_geometry) {
				record(a_geometry);
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
	}

	std::unique_lock lock(geometryOriginMutex);
	if (geometryOrigins.size() > 65536)
		geometryOrigins.clear();
	if (signatureOrigins.size() + ambiguousOriginSignatures.size() > 65536) {
		signatureOrigins.clear();
		ambiguousOriginSignatures.clear();
	}
	for (auto& [geometry, signature] : loadedGeometries) {
		geometryOrigins[geometry] = GeometryOrigin{ nifKey, signature };
		if (signature.empty() || ambiguousOriginSignatures.contains(signature))
			continue;
		if (auto existing = signatureOrigins.find(signature); existing == signatureOrigins.end()) {
			signatureOrigins.emplace(signature, nifKey);
		} else if (existing->second != nifKey) {
			signatureOrigins.erase(existing);
			ambiguousOriginSignatures.insert(signature);
		}
	}
}

std::string Skin::ResolveNifOrigin(RE::BSGeometry* a_geometry, RE::BSLightingShaderMaterialBase const* a_material) const
{
	const std::string signature = MakeGeometrySignature(a_geometry, a_material);
	std::shared_lock lock(geometryOriginMutex);
	if (auto direct = geometryOrigins.find(a_geometry); direct != geometryOrigins.end() && direct->second.signature == signature)
		return direct->second.nifKey;
	if (!signature.empty() && !ambiguousOriginSignatures.contains(signature)) {
		if (auto unique = signatureOrigins.find(signature); unique != signatureOrigins.end())
			return unique->second;
	}
	return {};
}

bool Skin::Hooks::BSStream_Load3::thunk(RE::BSStream* a_stream, const char* a_path)
{
	const bool loaded = func(a_stream, a_path);
	if (loaded)
		globals::features::skin.RecordNifOrigins(a_stream, a_path);
	return loaded;
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

	std::unordered_map<std::string, std::filesystem::file_time_type> current;
	auto collectFiles = [&](const std::filesystem::path& dir) {
		std::vector<std::filesystem::path> files;
		std::error_code ec;
		if (!std::filesystem::exists(dir, ec) || ec)
			return files;
		for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
			const auto& entry = *it;
			auto extension = NormalizeIdentifier(entry.path().extension().string());
			if (!entry.is_regular_file(ec) || ec || extension != ".json")
				continue;
			auto mtime = std::filesystem::last_write_time(entry.path(), ec);
			if (!ec) {
				current[entry.path().string()] = mtime;
				files.push_back(entry.path());
			}
		}
		std::ranges::sort(files, [](const auto& a, const auto& b) {
			return NormalizeIdentifier(a.filename().string()) < NormalizeIdentifier(b.filename().string());
		});
		return files;
	};
	auto modFiles = collectFiles(rootDir);
	auto userFiles = collectFiles(userDir);

	if (current == fileTimes)
		return;

	std::unordered_map<std::string, Entry> modNif;
	std::unordered_map<std::string, Entry> modBaseId;
	std::unordered_map<std::string, json> userNif;
	std::unordered_map<std::string, json> userBaseId;
	std::unordered_map<std::string, Entry> parsedProfiles;
	std::unordered_map<std::string, TextureMaterial> parsedMaterials;
	std::vector<Rule> parsedRules;
	std::unordered_map<std::string, Entry> parsedBaseMaterials;
	std::unordered_map<std::string, MaterialInstance> parsedSurfaceInstances;
	std::unordered_map<std::string, MaterialInstance> parsedAppearanceInstances;
	std::unordered_map<std::string, MaterialInstance> parsedLocalInstances;
	std::unordered_map<std::string, json> parsedClassifiers;
	std::vector<Binding> parsedBindings;

	auto readSelectorString = [](const json& a_match, const char* a_key) -> std::string {
		if (!a_match.contains(a_key) || !a_match[a_key].is_string())
			return {};
		return a_match[a_key].get<std::string>();
	};
	auto parseSelector = [&](const json& a_match, Selector& a_selector, const std::string& a_source) {
		static constexpr std::array<std::string_view, 19> knownFields{
			"nif", "nifGlob", "shape", "shapeGlob", "baseid", "referenceid", "race", "normal", "normalPrefix", "normalGlob",
			"diffuse", "diffusePrefix", "diffuseGlob", "armor", "armorAddon", "slot", "sex", "shaderFeature", "tag"
		};
		for (const auto& [field, value] : a_match.items()) {
			if (std::ranges::find(knownFields, field) == knownFields.end() || !value.is_string()) {
				logger::warn("[Advanced Skin] Invalid selector field '{}' in {}", field, a_source);
				return false;
			}
		}
		a_selector.nif = NormalizeNifKey(readSelectorString(a_match, "nif"));
		a_selector.nifGlob = NormalizeNifKey(readSelectorString(a_match, "nifGlob"));
		a_selector.shape = NormalizeIdentifier(readSelectorString(a_match, "shape"));
		a_selector.shapeGlob = NormalizeIdentifier(readSelectorString(a_match, "shapeGlob"));
		a_selector.baseId = NormalizeBaseIdKey(readSelectorString(a_match, "baseid"));
		a_selector.referenceId = NormalizeBaseIdKey(readSelectorString(a_match, "referenceid"));
		a_selector.race = NormalizeIdentifier(readSelectorString(a_match, "race"));
		a_selector.normal = NormalizeTextureKey(readSelectorString(a_match, "normal"));
		a_selector.normalPrefix = NormalizeTextureKey(readSelectorString(a_match, "normalPrefix"));
		a_selector.normalGlob = NormalizeTextureKey(readSelectorString(a_match, "normalGlob"));
		a_selector.diffuse = NormalizeTextureKey(readSelectorString(a_match, "diffuse"));
		a_selector.diffusePrefix = NormalizeTextureKey(readSelectorString(a_match, "diffusePrefix"));
		a_selector.diffuseGlob = NormalizeTextureKey(readSelectorString(a_match, "diffuseGlob"));
		a_selector.armor = NormalizeIdentifier(readSelectorString(a_match, "armor"));
		a_selector.armorAddon = NormalizeIdentifier(readSelectorString(a_match, "armorAddon"));
		a_selector.slot = NormalizeIdentifier(readSelectorString(a_match, "slot"));
		a_selector.sex = NormalizeIdentifier(readSelectorString(a_match, "sex"));
		a_selector.shaderFeature = NormalizeIdentifier(readSelectorString(a_match, "shaderFeature"));
		a_selector.tag = NormalizeIdentifier(readSelectorString(a_match, "tag"));
		return true;
	};

	auto parseFiles = [&](const std::vector<std::filesystem::path>& files, bool isUser) {
		for (const auto& path : files) {
			const std::string filePath = path.string();
			const std::string fileName = path.filename().string();
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

				if (j.contains("profiles") && j["profiles"].is_object()) {
					for (const auto& [name, value] : j["profiles"].items()) {
						if (!value.is_object()) {
							logger::warn("[Advanced Skin] Ignoring invalid named profile '{}': {}", name, fileName);
							continue;
						}
						json validated = SkinProfile{};
						for (const auto& [field, fieldValue] : value.items()) {
							if (!fieldValue.is_null())
								validated[field] = fieldValue;
						}
						(void)validated.get<SkinProfile>();
						parsedProfiles[NormalizeIdentifier(name)] = Entry{ fileName, isUser, value };
					}
				}

				if (j.contains("materials") && j["materials"].is_object()) {
					for (const auto& [name, value] : j["materials"].items()) {
						if (!value.is_object()) {
							logger::warn("[Advanced Skin] Ignoring invalid material '{}': {}", name, fileName);
							continue;
						}
						const auto id = NormalizeIdentifier(name);
						if (value.contains("parameters")) {
							if (!value["parameters"].is_object()) {
								logger::warn("[Advanced Skin] Ignoring base material '{}' with invalid parameters: {}", name, fileName);
								continue;
							}
							json complete = SkinProfile{};
							for (const auto& [field, fieldValue] : value["parameters"].items()) {
								if (!fieldValue.is_null())
									complete[field] = fieldValue;
							}
							(void)complete.get<SkinProfile>();
							parsedBaseMaterials[id] = Entry{ fileName, isUser, std::move(complete) };
						} else {
							// Compatibility: the first rule format used materials as texture-only payloads.
							parsedMaterials[id] = ParseTextureMaterial(value, std::format("{}:materials.{}", fileName, name));
						}
					}
				}

				auto parseInstances = [&](const char* a_key, Domain a_domain, auto& a_output) {
					if (!j.contains(a_key) || !j[a_key].is_object())
						return;
					for (const auto& [name, value] : j[a_key].items()) {
						if (!value.is_object()) {
							logger::warn("[Advanced Skin] Ignoring invalid instance '{}': {}", name, fileName);
							continue;
						}
						MaterialInstance instance;
						instance.id = NormalizeIdentifier(name);
						instance.parent = NormalizeIdentifier(value.value("parent", std::string{}));
						instance.source = fileName;
						instance.isUser = isUser;
						instance.domain = a_domain;
						if (value.contains("parameters")) {
							if (!value["parameters"].is_object()) {
								logger::warn("[Advanced Skin] Ignoring instance '{}' with invalid parameters: {}", name, fileName);
								continue;
							}
							json validated = SkinProfile{};
							for (const auto& [field, fieldValue] : value["parameters"].items()) {
								if (!fieldValue.is_null())
									validated[field] = fieldValue;
							}
							(void)validated.get<SkinProfile>();
							instance.parameters = value["parameters"];
						}
						if (value.contains("textures"))
							instance.textures = ParseTextureMaterial(value["textures"], std::format("{}:{}.{}", fileName, a_key, name));
						a_output[instance.id] = std::move(instance);
					}
				};
				parseInstances("surfaceInstances", Domain::Surface, parsedSurfaceInstances);
				parseInstances("appearanceInstances", Domain::Appearance, parsedAppearanceInstances);
				parseInstances("localInstances", Domain::Local, parsedLocalInstances);

				if (j.contains("classifiers") && j["classifiers"].is_object()) {
					for (const auto& [tag, expression] : j["classifiers"].items()) {
						if (expression.is_object())
							parsedClassifiers[NormalizeIdentifier(tag)] = expression;
						else
							logger::warn("[Advanced Skin] Ignoring invalid classifier '{}': {}", tag, fileName);
					}
				}

				if (j.contains("rules") && j["rules"].is_array()) {
					const int32_t filePriority = j.value("priority", 0);
					uint32_t sourceOrder = 0;
					for (const auto& value : j["rules"]) {
						const uint32_t currentOrder = sourceOrder++;
						if (!value.is_object() || !value.contains("match") || !value["match"].is_object()) {
							logger::warn("[Advanced Skin] Ignoring rule without an object 'match': {}#{}", fileName, currentOrder);
							continue;
						}
						Rule rule;
						rule.source = NormalizeIdentifier(fileName);
						rule.isUser = isUser;
						rule.priority = value.value("priority", filePriority);
						rule.sourceOrder = currentOrder;
						rule.id = value.value("id", std::format("{}#{}", fileName, currentOrder));
						const auto& match = value["match"];
						if (!parseSelector(match, rule.match, std::format("rule '{}' ({})", rule.id, fileName)))
							continue;
						if (value.contains("profile") && value["profile"].is_string())
							rule.profile = NormalizeIdentifier(value["profile"].get<std::string>());
						if (value.contains("profileOverrides") && value["profileOverrides"].is_object()) {
							json validated = SkinProfile{};
							for (const auto& [field, fieldValue] : value["profileOverrides"].items()) {
								if (!fieldValue.is_null())
									validated[field] = fieldValue;
							}
							(void)validated.get<SkinProfile>();
							rule.profileOverrides = value["profileOverrides"];
						}
						if (value.contains("material") && value["material"].is_string())
							rule.material = NormalizeIdentifier(value["material"].get<std::string>());
						if (value.contains("textures") && value["textures"].is_object())
							rule.textures = ParseTextureMaterial(value["textures"], std::format("{}:rules.{}", fileName, rule.id));
						if (rule.profile.empty() && rule.profileOverrides.empty() && rule.material.empty() &&
							rule.textures.rfaos.state == TextureChannel::State::Inherit && rule.textures.wetness.state == TextureChannel::State::Inherit) {
							logger::warn("[Advanced Skin] Ignoring rule '{}' without a profile or texture payload: {}", rule.id, fileName);
							continue;
						}
						parsedRules.push_back(std::move(rule));
					}
				}

				if (j.contains("bindings") && j["bindings"].is_array()) {
					const int32_t filePriority = j.value("priority", 0);
					uint32_t sourceOrder = 0;
					for (const auto& value : j["bindings"]) {
						const uint32_t currentOrder = sourceOrder++;
						if (!value.is_object() || !value.contains("match") || !value["match"].is_object() ||
							!value.contains("domain") || !value["domain"].is_string() || !value.contains("use") || !value["use"].is_string()) {
							logger::warn("[Advanced Skin] Ignoring incomplete binding: {}#{}", fileName, currentOrder);
							continue;
						}
						auto domain = ParseDomain(value["domain"].get<std::string>());
						if (!domain) {
							logger::warn("[Advanced Skin] Ignoring binding with unknown domain: {}#{}", fileName, currentOrder);
							continue;
						}
						Binding binding;
						binding.id = value.value("id", std::format("{}#{}", fileName, currentOrder));
						binding.source = NormalizeIdentifier(fileName);
						binding.use = NormalizeIdentifier(value["use"].get<std::string>());
						binding.isUser = isUser;
						binding.domain = *domain;
						binding.priority = value.value("priority", filePriority);
						binding.sourceOrder = currentOrder;
						if (!parseSelector(value["match"], binding.match, std::format("binding '{}' ({})", binding.id, fileName)))
							continue;
						if (binding.domain == Domain::Local && (!binding.match.HasActorSelector() || !binding.match.HasSurfaceSelector())) {
							logger::warn("[Advanced Skin] Ignoring Local binding '{}' without both actor and surface selectors", binding.id);
							continue;
						}
						parsedBindings.push_back(std::move(binding));
					}
				}
			} catch (const std::exception& e) {
				logger::error("[Advanced Skin] Failed to parse override file {}: {}", filePath, e.what());
			}
		}
	};
	parseFiles(modFiles, false);
	parseFiles(userFiles, true);

	auto validateInstances = [&](auto& a_instances, Domain a_domain) {
		std::unordered_map<std::string, uint8_t> state;
		std::function<bool(const std::string&, uint32_t)> visit = [&](const std::string& id, uint32_t depth) {
			auto found = a_instances.find(id);
			if (found == a_instances.end())
				return false;
			auto& instance = found->second;
			if (state[id] == 2)
				return instance.valid;
			if (state[id] == 1 || depth > 32) {
				logger::error("[Advanced Skin] Invalid or excessively deep material-instance cycle at '{}'", id);
				instance.valid = false;
				return false;
			}
			state[id] = 1;
			if (!instance.parent.empty()) {
				const bool rootParent = a_domain == Domain::Surface &&
				                        (instance.parent == "advanced-skin:default" || parsedBaseMaterials.contains(instance.parent));
				if (!rootParent && !visit(instance.parent, depth + 1)) {
					logger::error("[Advanced Skin] Instance '{}' has a missing, invalid, or cross-domain parent '{}'", id, instance.parent);
					instance.valid = false;
				}
			}
			state[id] = 2;
			return instance.valid;
		};
		for (auto& [id, instance] : a_instances)
			visit(id, 0);
	};
	validateInstances(parsedSurfaceInstances, Domain::Surface);
	validateInstances(parsedAppearanceInstances, Domain::Appearance);
	validateInstances(parsedLocalInstances, Domain::Local);

	auto bindingTargetExists = [&](const Binding& binding) {
		const auto* instances = binding.domain == Domain::Surface    ? &parsedSurfaceInstances :
		                        binding.domain == Domain::Appearance ? &parsedAppearanceInstances :
		                                                               &parsedLocalInstances;
		auto found = instances->find(binding.use);
		return found != instances->end() && found->second.valid;
	};
	std::erase_if(parsedBindings, [&](const Binding& binding) {
		if (bindingTargetExists(binding)) {
			if (binding.domain == Domain::Appearance && !binding.match.HasSurfaceSelector()) {
				const auto& instance = parsedAppearanceInstances.at(binding.use);
				if (instance.textures.rfaos.state != TextureChannel::State::Inherit ||
					instance.textures.wetness.state != TextureChannel::State::Inherit) {
					logger::warn("[Advanced Skin] Appearance binding '{}' sets textures without a surface selector; it affects every matched skin part",
						binding.id);
				}
			}
			return false;
		}
		logger::error("[Advanced Skin] Binding '{}' references missing or invalid instance '{}'", binding.id, binding.use);
		return true;
	});

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

	namedProfiles = std::move(parsedProfiles);
	materials = std::move(parsedMaterials);
	std::ranges::stable_sort(parsedRules, [](const Rule& a, const Rule& b) {
		return std::tuple{ a.isUser, a.priority, a.match.Specificity(), a.source, a.sourceOrder } <
		       std::tuple{ b.isUser, b.priority, b.match.Specificity(), b.source, b.sourceOrder };
	});
	rules = std::move(parsedRules);
	baseMaterials = std::move(parsedBaseMaterials);
	surfaceInstances = std::move(parsedSurfaceInstances);
	appearanceInstances = std::move(parsedAppearanceInstances);
	localInstances = std::move(parsedLocalInstances);
	classifiers = std::move(parsedClassifiers);
	std::ranges::stable_sort(parsedBindings, [](const Binding& a, const Binding& b) {
		return std::tuple{ a.isUser, a.priority, a.match.Specificity(), a.source, a.sourceOrder } <
		       std::tuple{ b.isUser, b.priority, b.match.Specificity(), b.source, b.sourceOrder };
	});
	bindings = std::move(parsedBindings);

	fileTimes = std::move(current);
	++revision;
	logger::info(
		"[Advanced Skin] Loaded {} NIF / {} BaseID legacy override(s), {} legacy profile(s), {} legacy texture material(s), "
		"{} base / {} surface / {} appearance / {} local material(s), {} classifier(s), {} binding(s), and {} legacy rule(s) (revision {})",
		nifOverrides.size(), baseIdOverrides.size(), namedProfiles.size(), materials.size(), baseMaterials.size(), surfaceInstances.size(),
		appearanceInstances.size(), localInstances.size(), classifiers.size(), bindings.size(), rules.size(), revision);
}

uint32_t Skin::OverrideStore::Selector::Specificity() const
{
	return static_cast<uint32_t>(!nif.empty()) + static_cast<uint32_t>(!nifGlob.empty()) +
	       static_cast<uint32_t>(!shape.empty()) + static_cast<uint32_t>(!shapeGlob.empty()) +
	       static_cast<uint32_t>(!baseId.empty()) + static_cast<uint32_t>(!referenceId.empty()) +
	       static_cast<uint32_t>(!race.empty()) + static_cast<uint32_t>(!normal.empty()) +
	       static_cast<uint32_t>(!normalPrefix.empty()) + static_cast<uint32_t>(!normalGlob.empty()) +
	       static_cast<uint32_t>(!diffuse.empty()) + static_cast<uint32_t>(!diffusePrefix.empty()) +
	       static_cast<uint32_t>(!diffuseGlob.empty()) + static_cast<uint32_t>(!armor.empty()) +
	       static_cast<uint32_t>(!armorAddon.empty()) + static_cast<uint32_t>(!slot.empty()) +
	       static_cast<uint32_t>(!sex.empty()) + static_cast<uint32_t>(!shaderFeature.empty()) + static_cast<uint32_t>(!tag.empty());
}

bool Skin::OverrideStore::Selector::HasActorSelector() const
{
	return !baseId.empty() || !referenceId.empty() || !race.empty() || !sex.empty();
}

bool Skin::OverrideStore::Selector::HasSurfaceSelector() const
{
	return !nif.empty() || !nifGlob.empty() || !shape.empty() || !shapeGlob.empty() || !normal.empty() ||
	       !normalPrefix.empty() || !normalGlob.empty() || !diffuse.empty() || !diffusePrefix.empty() ||
	       !diffuseGlob.empty() || !armor.empty() || !armorAddon.empty() || !slot.empty() || !shaderFeature.empty() || !tag.empty();
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

Skin::SkinProfile Skin::ApplyProfileOverride(const SkinProfile& a_base, const json& a_override) const
{
	json mergedJson = a_base;
	for (const auto& [key, value] : a_override.items()) {
		if (!value.is_null())
			mergedJson[key] = value;
	}
	return mergedJson.get<SkinProfile>();
}

Skin::GeometryContext Skin::BuildGeometryContext(RE::BSGeometry* a_geometry, RE::BSLightingShaderMaterialBase const* a_material) const
{
	GeometryContext context;
	if (!a_geometry)
		return context;
	context.nif = ResolveNifOrigin(a_geometry, a_material);
	if (context.nif.empty())
		context.nif = DeriveModelNifKey(a_geometry);
	context.legacyNif = DeriveNifKey(a_geometry);
	const char* name = a_geometry->name.c_str();
	context.shape = NormalizeIdentifier(name ? name : "");
	context.normal = GetTextureKey(a_material, RE::BSTextureSet::Texture::kNormal);
	context.diffuse = GetTextureKey(a_material, RE::BSTextureSet::Texture::kDiffuse);
	if (a_material) {
		switch (a_material->GetFeature()) {
		case RE::BSShaderMaterial::Feature::kFaceGen:
			context.shaderFeature = "facegen";
			break;
		case RE::BSShaderMaterial::Feature::kFaceGenRGBTint:
			context.shaderFeature = "facegenrgbtint";
			break;
		default:
			context.shaderFeature = std::format("{}", static_cast<uint32_t>(a_material->GetFeature()));
			break;
		}
	}

	auto* userData = a_geometry->GetUserData();
	if (userData) {
		context.referenceId = FormatBaseIdKey(userData->formID);
		if (userData->formType == RE::FormType::ActorCharacter) {
			auto* actor = static_cast<RE::Character*>(userData);
			if (auto* npc = actor->GetActorBase()) {
				context.baseId = FormatBaseIdKey(npc->formID);
				context.sex = npc->GetSex() == RE::SEX::kFemale ? "female" : "male";
			}
			if (auto* race = actor->GetRace())
				context.race = NormalizeIdentifier(Util::GetFormEditorID(race));

			static constexpr std::array<std::string_view, RE::BIPED_OBJECTS::kTotal> slotNames{
				"head", "hair", "body", "hands", "forearms", "amulet", "ring", "feet", "calves", "shield", "tail", "longhair",
				"circlet", "ears", "modmouth", "modneck", "modchestprimary", "modback", "modmisc1", "modpelvisprimary", "decapitatehead",
				"decapitate", "modpelvissecondary", "modlegright", "modlegleft", "modfacejewelry", "modchestsecondary", "modshoulder",
				"modarmleft", "modarmright", "modmisc2", "fx01", "handtohandmelee", "onehandsword", "onehanddagger", "onehandaxe",
				"onehandmace", "twohandmelee", "bow", "staff", "crossbow", "quiver"
			};
			if (auto biped = actor->GetActorRuntimeData().biped) {
				for (uint32_t slot = 0; slot < RE::BIPED_OBJECTS::kTotal; ++slot) {
					const auto& object = biped->objects[slot];
					if (!object.partClone || !ScenegraphContains(object.partClone.get(), a_geometry))
						continue;
					context.slot = std::string(slotNames[slot]);
					context.armor = GetFormSelectorIdentity(object.item);
					context.armorFormId = object.item ? FormatBaseIdKey(object.item->formID) : std::string{};
					context.armorAddon = GetFormSelectorIdentity(object.addon);
					context.armorAddonFormId = object.addon ? FormatBaseIdKey(object.addon->formID) : std::string{};
					break;
				}
			}
		}
	}

	// Classifiers may reference already-derived tags. Iterate to a fixed point so simple tag composition works.
	for (size_t pass = 0; pass <= overrideStore.classifiers.size(); ++pass) {
		bool changed = false;
		for (const auto& [tag, expression] : overrideStore.classifiers) {
			if (!context.tags.contains(tag) && MatchClassifier(expression, context)) {
				context.tags.insert(tag);
				changed = true;
			}
		}
		if (!changed)
			break;
	}

	size_t hash = 0;
	auto combine = [&](const std::string& value) {
		hash ^= std::hash<std::string>{}(value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
	};
	combine(context.nif);
	combine(context.legacyNif);
	combine(context.shape);
	combine(context.baseId);
	combine(context.referenceId);
	combine(context.race);
	combine(context.normal);
	combine(context.diffuse);
	combine(context.armor);
	combine(context.armorFormId);
	combine(context.armorAddon);
	combine(context.armorAddonFormId);
	combine(context.slot);
	combine(context.sex);
	combine(context.shaderFeature);
	std::vector<std::string> sortedTags(context.tags.begin(), context.tags.end());
	std::ranges::sort(sortedTags);
	for (const auto& tag : sortedTags)
		combine(tag);
	context.fingerprint = static_cast<uint64_t>(hash);
	return context;
}

bool Skin::MatchSelector(const OverrideStore::Selector& match, const GeometryContext& context) const
{
	auto formMatches = [](const std::string& selector, const std::string& editorID, const std::string& formID) {
		return selector.empty() || selector == editorID || NormalizeBaseIdKey(selector) == formID;
	};
	return (match.nif.empty() || match.nif == context.nif || match.nif == context.legacyNif) &&
	       (match.nifGlob.empty() || GlobMatch(match.nifGlob, context.nif) || GlobMatch(match.nifGlob, context.legacyNif)) &&
	       (match.shape.empty() || match.shape == context.shape) &&
	       (match.shapeGlob.empty() || GlobMatch(match.shapeGlob, context.shape)) &&
	       (match.baseId.empty() || match.baseId == context.baseId) &&
	       (match.referenceId.empty() || match.referenceId == context.referenceId) &&
	       (match.race.empty() || match.race == context.race) &&
	       (match.normal.empty() || match.normal == context.normal) &&
	       (match.normalPrefix.empty() || context.normal.starts_with(match.normalPrefix)) &&
	       (match.normalGlob.empty() || GlobMatch(match.normalGlob, context.normal)) &&
	       (match.diffuse.empty() || match.diffuse == context.diffuse) &&
	       (match.diffusePrefix.empty() || context.diffuse.starts_with(match.diffusePrefix)) &&
	       (match.diffuseGlob.empty() || GlobMatch(match.diffuseGlob, context.diffuse)) &&
	       formMatches(match.armor, context.armor, context.armorFormId) &&
	       formMatches(match.armorAddon, context.armorAddon, context.armorAddonFormId) &&
	       (match.slot.empty() || match.slot == context.slot) &&
	       (match.sex.empty() || match.sex == context.sex) &&
	       (match.shaderFeature.empty() || match.shaderFeature == context.shaderFeature) &&
	       (match.tag.empty() || context.tags.contains(match.tag));
}

bool Skin::MatchClassifier(const json& expression, const GeometryContext& context, uint32_t depth) const
{
	if (!expression.is_object() || depth > 32)
		return false;
	if (expression.contains("all")) {
		if (!expression["all"].is_array())
			return false;
		for (const auto& child : expression["all"]) {
			if (!MatchClassifier(child, context, depth + 1))
				return false;
		}
		return true;
	}
	if (expression.contains("any")) {
		if (!expression["any"].is_array())
			return false;
		for (const auto& child : expression["any"]) {
			if (MatchClassifier(child, context, depth + 1))
				return true;
		}
		return false;
	}
	if (expression.contains("not"))
		return !MatchClassifier(expression["not"], context, depth + 1);

	auto get = [&](const char* key) {
		return expression.contains(key) && expression[key].is_string() ? expression[key].get<std::string>() : std::string{};
	};
	OverrideStore::Selector selector;
	selector.nif = NormalizeNifKey(get("nif"));
	selector.nifGlob = NormalizeNifKey(get("nifGlob"));
	selector.shape = NormalizeIdentifier(get("shape"));
	selector.shapeGlob = NormalizeIdentifier(get("shapeGlob"));
	selector.baseId = NormalizeBaseIdKey(get("baseid"));
	selector.referenceId = NormalizeBaseIdKey(get("referenceid"));
	selector.race = NormalizeIdentifier(get("race"));
	selector.normal = NormalizeTextureKey(get("normal"));
	selector.normalPrefix = NormalizeTextureKey(get("normalPrefix"));
	selector.normalGlob = NormalizeTextureKey(get("normalGlob"));
	selector.diffuse = NormalizeTextureKey(get("diffuse"));
	selector.diffusePrefix = NormalizeTextureKey(get("diffusePrefix"));
	selector.diffuseGlob = NormalizeTextureKey(get("diffuseGlob"));
	selector.armor = NormalizeIdentifier(get("armor"));
	selector.armorAddon = NormalizeIdentifier(get("armorAddon"));
	selector.slot = NormalizeIdentifier(get("slot"));
	selector.sex = NormalizeIdentifier(get("sex"));
	selector.shaderFeature = NormalizeIdentifier(get("shaderFeature"));
	selector.tag = NormalizeIdentifier(get("tag"));
	return selector.Specificity() > 0 && MatchSelector(selector, context);
}

const Skin::OverrideStore::MaterialInstance* Skin::FindInstance(OverrideStore::Domain domain, const std::string& id) const
{
	const auto* instances = domain == OverrideStore::Domain::Surface    ? &overrideStore.surfaceInstances :
	                        domain == OverrideStore::Domain::Appearance ? &overrideStore.appearanceInstances :
	                                                                      &overrideStore.localInstances;
	auto found = instances->find(id);
	return found != instances->end() && found->second.valid ? &found->second : nullptr;
}

void Skin::ApplyInstanceChain(OverrideStore::Domain domain, const std::string& leaf, SkinProfile& profile,
	std::string& rfaosPath, std::string& wetnessPath) const
{
	std::vector<const OverrideStore::MaterialInstance*> chain;
	std::string current = leaf;
	for (uint32_t depth = 0; !current.empty() && depth <= 32; ++depth) {
		auto* instance = FindInstance(domain, current);
		if (!instance) {
			if (domain == OverrideStore::Domain::Surface) {
				if (auto root = overrideStore.baseMaterials.find(current); root != overrideStore.baseMaterials.end())
					profile = ApplyProfileOverride(profile, root->second.partial);
			}
			break;
		}
		chain.push_back(instance);
		current = instance->parent;
	}
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		profile = ApplyProfileOverride(profile, (*it)->parameters);
		ApplyTextureChannel(rfaosPath, (*it)->textures.rfaos);
		ApplyTextureChannel(wetnessPath, (*it)->textures.wetness);
	}
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
	skinExtraTextures.clear();
	configuredExtraTextures.clear();
	geometryOverrideCache.clear();
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

Skin::ExtraTextures Skin::LoadConfiguredExtraTextures(const std::string& a_rfaosPath, const std::string& a_wetnessPath)
{
	const std::string rfaosPath = NormalizeTextureKey(a_rfaosPath);
	const std::string wetnessPath = NormalizeTextureKey(a_wetnessPath);
	const std::string cacheKey = std::format("{}\n{}", rfaosPath, wetnessPath);
	if (auto cached = configuredExtraTextures.find(cacheKey); cached != configuredExtraTextures.end())
		return cached->second;

	const auto& stateData = globals::game::graphicsState->GetRuntimeData();
	ExtraTextures result;
	result.extraTexturePath = rfaosPath;
	result.wetnessTexturePath = wetnessPath;
	result.rfaosTexture = stateData.defaultTextureBlack;
	result.wetnessTexture = stateData.defaultTextureBlack;

	auto load = [](const std::string& a_path, RE::NiSourceTexturePtr& a_output) {
		if (a_path.empty())
			return false;
		// BSShaderManager::GetTexture may return a valid placeholder for a missing path.
		// Probe Skyrim's resource system first so loose files and BSA resources are both covered.
		std::string resourcePath = a_path;
		std::replace(resourcePath.begin(), resourcePath.end(), '/', '\\');
		RE::BSResourceNiBinaryStream resource(resourcePath);
		if (!resource.good() || !resource.stream || resource.stream->totalSize == 0)
			return false;
		RE::NiPointer<RE::NiTexture> texture;
		RE::BSShaderManager::GetTexture(a_path.c_str(), true, texture, false);
		if (!texture || texture->GetRTTI() != globals::rtti::NiSourceTextureRTTI.get())
			return false;
		auto* source = static_cast<RE::NiSourceTexture*>(texture.get());
		if (!source->rendererTexture || !source->rendererTexture->resourceView)
			return false;
		a_output = RE::NiPointer(source);
		return true;
	};

	result.hasExtraTexture = load(rfaosPath, result.rfaosTexture);
	result.hasWetnessTexture = load(wetnessPath, result.wetnessTexture);
	if (!rfaosPath.empty() && !result.hasExtraTexture)
		logger::warn("[Advanced Skin] Failed to load configured RFAOS texture: {}", rfaosPath);
	if (!wetnessPath.empty() && !result.hasWetnessTexture)
		logger::warn("[Advanced Skin] Failed to load configured wetness texture: {}", wetnessPath);

	configuredExtraTextures.emplace(cacheKey, result);
	return result;
}

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

	// Compatibility discovery is a synthetic low-priority Surface binding. Never write
	// the discovered paths into the engine's shared BSTextureSet/material.
	auto workingExtraPtr = LoadConfiguredExtraTextures(extraTexturePath, wetnessTexturePath);
	skinExtraTextures[hashKey] = workingExtraPtr;

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
	if (!settings.EnableLegacyExtraTextureDiscovery) {
		const auto& stateData = globals::game::graphicsState->GetRuntimeData();
		skinExtendedRendererState.SetExtraSkinPSTexture(
			stateData.defaultTextureBlack->rendererTexture, stateData.defaultTextureBlack->rendererTexture);
		return;
	}

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

	if (settings.EnableSkin && a_pass && a_pass->geometry) {
		if (profileData.empty())
			RebuildProfileData();

		auto geometry = a_pass->geometry;
		RE::BSLightingShaderMaterialBase const* material = nullptr;
		if (auto* property = a_pass->shaderProperty) {
			if (auto* baseMaterial = property->GetBaseMaterial(); baseMaterial && baseMaterial->GetType() == RE::BSShaderMaterial::Type::kLighting)
				material = static_cast<RE::BSLightingShaderMaterialBase const*>(baseMaterial);
		}
		const bool isSkinMaterial = material &&
		                            (material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGen || material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint);
		// This hook sees every Lighting draw. Non-skin materials neither consume b7 nor
		// the extra SRVs, so do not construct actor/NIF/classifier context for them.
		if (!isSkinMaterial)
			return;

		uint32_t profileIndex = 0;
		float4 wetness = GetWetness(geometry, profileIndex);
		if (profileIndex >= profileData.size())
			profileIndex = 0;

		if (geometryOverrideCache.size() > 4096)
			geometryOverrideCache.clear();
		auto& entry = geometryOverrideCache[geometry];
		const uint32_t materialHash = material ? material->hashKey : 0;
		const uint64_t contextIdentity = MakeGeometryContextIdentity(geometry, material);
		const bool rebuildContext = !entry.initialized || entry.materialHash != materialHash ||
		                            entry.contextIdentity != contextIdentity || entry.overrideRevision != overrideStore.revision;
		if (rebuildContext) {
			entry.context = BuildGeometryContext(geometry, material);
			entry.contextIdentity = contextIdentity;
			entry.contextFingerprint = entry.context.fingerprint;
		}
		const auto& geometryContext = entry.context;
		if (!entry.initialized || rebuildContext ||
			entry.profileIndex != profileIndex || entry.baseProfileRevision != profileDataRevision ||
			entry.overrideRevision != overrideStore.revision || entry.previewRevision != uiOverridePreviewRevision) {
			entry.initialized = true;
			entry.materialHash = materialHash;
			entry.contextFingerprint = geometryContext.fingerprint;
			entry.nifKey = geometryContext.nif;
			entry.legacyNifKey = geometryContext.legacyNif;
			entry.shapeKey = geometryContext.shape;
			entry.baseIdKey = geometryContext.baseId;
			entry.raceKey = geometryContext.race;
			entry.normalKey = geometryContext.normal;
			entry.diffuseKey = geometryContext.diffuse;
			entry.profileIndex = profileIndex;
			entry.baseProfileRevision = profileDataRevision;
			entry.overrideRevision = overrideStore.revision;
			entry.previewRevision = uiOverridePreviewRevision;

			SkinProfile resolvedProfile = settings.DefaultProfile;
			if (auto root = overrideStore.baseMaterials.find("advanced-skin:default"); root != overrideStore.baseMaterials.end())
				resolvedProfile = ApplyProfileOverride(resolvedProfile, root->second.partial);

			ExtraTextures legacyTextures;
			const auto& stateData = globals::game::graphicsState->GetRuntimeData();
			legacyTextures.rfaosTexture = stateData.defaultTextureBlack;
			legacyTextures.wetnessTexture = stateData.defaultTextureBlack;
			if (settings.EnableLegacyExtraTextureDiscovery && isSkinMaterial && materialHash != 0) {
				if (!skinExtraTextures.contains(materialHash))
					SetupExtraTexture(material, material->textureSet.get(), materialHash);
				if (auto legacy = skinExtraTextures.find(materialHash); legacy != skinExtraTextures.end())
					legacyTextures = legacy->second;
			}
			std::string rfaosPath = NormalizeTextureKey(legacyTextures.extraTexturePath);
			std::string wetnessPath = NormalizeTextureKey(legacyTextures.wetnessTexturePath);

			std::array<std::string, 3> winners;
			for (const auto& binding : overrideStore.bindings) {
				if (MatchSelector(binding.match, geometryContext))
					winners[static_cast<size_t>(binding.domain)] = binding.use;
			}

			auto applyLegacyRule = [&](const OverrideStore::Rule& rule) {
				if (!rule.profile.empty()) {
					if (auto named = overrideStore.namedProfiles.find(rule.profile); named != overrideStore.namedProfiles.end()) {
						resolvedProfile = ApplyProfileOverride(resolvedProfile, named->second.partial);
					} else {
						for (const auto& [name, profile] : settings.Profiles) {
							if (NormalizeIdentifier(name) == rule.profile) {
								resolvedProfile = profile;
								break;
							}
						}
					}
				}
				if (!rule.profileOverrides.empty())
					resolvedProfile = ApplyProfileOverride(resolvedProfile, rule.profileOverrides);
				if (!rule.material.empty()) {
					if (auto named = overrideStore.materials.find(rule.material); named != overrideStore.materials.end()) {
						ApplyTextureChannel(rfaosPath, named->second.rfaos);
						ApplyTextureChannel(wetnessPath, named->second.wetness);
					}
				}
				ApplyTextureChannel(rfaosPath, rule.textures.rfaos);
				ApplyTextureChannel(wetnessPath, rule.textures.wetness);
			};
			auto applyLegacyDomain = [&](OverrideStore::Domain domain) {
				for (const auto& rule : overrideStore.rules) {
					if (!MatchSelector(rule.match, geometryContext))
						continue;
					const bool actor = rule.match.HasActorSelector();
					const bool surface = rule.match.HasSurfaceSelector();
					const auto ruleDomain = actor && surface ? OverrideStore::Domain::Local :
					                        actor            ? OverrideStore::Domain::Appearance :
					                                           OverrideStore::Domain::Surface;
					if (ruleDomain == domain)
						applyLegacyRule(rule);
				}
			};

			// Surface: legacy auto-discovery and NIF/rule compatibility are low-priority synthetic inputs.
			const json* nifPartial = geometryContext.nif.empty() ? nullptr : overrideStore.Lookup(OverrideKind::Nif, geometryContext.nif);
			if (!nifPartial && geometryContext.legacyNif != geometryContext.nif)
				nifPartial = overrideStore.Lookup(OverrideKind::Nif, geometryContext.legacyNif);
			if (uiOverrideEditorOpen && uiOverrideEditorKind == OverrideKind::Nif &&
				(uiOverrideEditorKey == geometryContext.nif || uiOverrideEditorKey == geometryContext.legacyNif))
				nifPartial = &uiOverridePreviewPartial;
			if (nifPartial)
				resolvedProfile = ApplyProfileOverride(resolvedProfile, *nifPartial);
			applyLegacyDomain(OverrideStore::Domain::Surface);
			ApplyInstanceChain(OverrideStore::Domain::Surface, winners[static_cast<size_t>(OverrideStore::Domain::Surface)],
				resolvedProfile, rfaosPath, wetnessPath);

			// Appearance: old race profiles and BaseID overrides remain partial compatibility layers.
			if (profileIndex < profileBaseData.size() && profileIndex != 0)
				resolvedProfile = ApplyProfileOverride(resolvedProfile, json(profileBaseData[profileIndex]));
			const json* baseIdPartial = geometryContext.baseId.empty() ? nullptr : overrideStore.Lookup(OverrideKind::BaseId, geometryContext.baseId);
			if (uiOverrideEditorOpen && uiOverrideEditorKind == OverrideKind::BaseId && uiOverrideEditorKey == geometryContext.baseId)
				baseIdPartial = &uiOverridePreviewPartial;
			if (baseIdPartial)
				resolvedProfile = ApplyProfileOverride(resolvedProfile, *baseIdPartial);
			applyLegacyDomain(OverrideStore::Domain::Appearance);
			ApplyInstanceChain(OverrideStore::Domain::Appearance, winners[static_cast<size_t>(OverrideStore::Domain::Appearance)],
				resolvedProfile, rfaosPath, wetnessPath);

			applyLegacyDomain(OverrideStore::Domain::Local);
			ApplyInstanceChain(OverrideStore::Domain::Local, winners[static_cast<size_t>(OverrideStore::Domain::Local)],
				resolvedProfile, rfaosPath, wetnessPath);

			entry.merged = MakeProfileData(resolvedProfile);
			if (rfaosPath == NormalizeTextureKey(legacyTextures.extraTexturePath) && wetnessPath == NormalizeTextureKey(legacyTextures.wetnessTexturePath))
				entry.textures = legacyTextures;
			else
				entry.textures = LoadConfiguredExtraTextures(rfaosPath, wetnessPath);
			entry.materialFlags = float4(entry.textures.hasExtraTexture ? 1.0f : 0.0f,
				entry.textures.hasWetnessTexture ? 1.0f : 0.0f, 0.0f, 0.0f);
		}

		SkinData perGeometryProfile = entry.merged;
		const auto& stateData = globals::game::graphicsState->GetRuntimeData();
		auto* rfaos = entry.textures.rfaosTexture ? entry.textures.rfaosTexture.get() : stateData.defaultTextureBlack.get();
		auto* wetnessTexture = entry.textures.wetnessTexture ? entry.textures.wetnessTexture.get() : stateData.defaultTextureBlack.get();
		skinExtendedRendererState.SetExtraSkinPSTexture(rfaos->rendererTexture, wetnessTexture->rendererTexture);

		// PerGeometryCB is shared across all geometry passes, so it must be refreshed
		// for every pass. A global "last update" guard is not valid here because two
		// geometries can share the same wetness/profile/revision and still need
		// different merged profile data (different NIF/BaseID overrides).
		PerGeometryData perGeometryData{};
		perGeometryData.skinPerGeometry = wetness;
		perGeometryData.materialFlags = entry.materialFlags;
		perGeometryData.profile = perGeometryProfile;
		PerGeometryCB->Update(perGeometryData);

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
