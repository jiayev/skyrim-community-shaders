#include "PhysicalSky.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <imgui_stdlib.h>

#include "CSEditor/SceneManager/SceneSettingsContextRules.h"
#include "CSEditor/SceneManager/SceneWidgetInterceptor.h"
#include "CloudShadows.h"
#include "Deferred.h"
#include "I18n/I18n.h"
#include "LinearLighting.h"
#include "PostProcessing/ColorSpace.h"
#include "SkySync.h"
#include "TerrainShadows.h"
#include "VolumetricShadows.h"

#include "I18n/I18n.h"
#include "State.h"

#define I18N_KEY_PREFIX "feature.physical_sky."
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalSky::WorldspaceInfo,
	zBottom)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TexNdfSettings,
	heightPath,
	modelingPath)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NdfNoiseParameters,
	type, seed, frequency, octaves, persistence, lacunarity, contrast, bias, repetitions, responseExponent)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudNoiseBand,
	frequency, octaves, persistence, exponent, contrast, bias, perlinMix)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudNoiseSettings,
	procedural, seed, shape, warp, warpCorrelation)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NdfNoiseInput,
	parameters, texturePath)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NdfNoiseLayer,
	noise, frequency, exponent, offset, range)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NdfGenerationParameters,
	primary, secondary, coverageGain, modeling, modelingGain, heightVariation,
	heightAuxiliaryRange, bottomTypeRange, baseHeight, bottomTypeExponent, heightFromCoverage,
	localBlendMode, localModelingWeight, localHeightWeight, localWindScale, windOffset)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ProceduralNdfSettings,
	parameters, noise, local, localMaskPath)

void to_json(nlohmann::json& j, const NdfSettings& value)
{
	j = { { "version", 2 }, { "type", value.type }, { "texture", value.texture }, { "procedural", value.procedural } };
}

void from_json(const nlohmann::json& j, NdfSettings& value)
{
	value = {};
	if (j.value("version", 0u) != 2u)
		return;
	value.type = j.value("type", NdfType::Procedural) == NdfType::Texture ? NdfType::Texture : NdfType::Procedural;
	value.texture = j.value("texture", TexNdfSettings{});
	value.procedural = j.value("procedural", ProceduralNdfSettings{});
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LowCloudSettings,
	ndfAltitudeOffset,
	ndfAltitudeScale,
	ndfScale,
	coverageBottomPower,
	coverageHeightRange,
	bottomDensityPower,
	bottomDensityWidth,
	topExpansion,
	densityScale)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CirrusSettings,
	enabled, altitude, patternScale, densityScale, lightingScale, weatherPath, patternsPath,
	weather, patternSeed, patternWarp, patternDetail)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudLightingSettings,
	lightingScale,
	sunExtinction,
	phaseForwardG,
	phaseBackwardG,
	phaseForwardWeight,
	phaseBackwardWeight,
	scatterVolumeStrength,
	scatterVolumeDepth,
	scatterVolumeHeight,
	softScatteringStrength,
	powderStrength,
	ambientStrength,
	ambientFloor,
	ambientDensity,
	ambientBase)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudWindSettings,
	lowVelocity, highVelocity, development, disturbance)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudLayer,
	low,
	cirrus,
	lighting,
	wind)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalSky::Settings,
	enabled,
	enableAllExteriorCells,
	forceEnableAllInteriorCells,
	overrideDirLight,
	lightSkyStatics,
	skyStaticsBrightness,
	halfResApShadow,
	vanillaMix,
	trMix,
	apLumMix,
	apTrMix,
	cloudShadowRemapRange,
	sunlightColor,
	masserColor,
	secundaColor,
	proceduralSun,
	sunDiskRad,

	worldspaceWhitelist,
	groundAlbedo,
	planetRadius,
	atmosphereRadius,
	rayleighFalloff,
	rayleighScatter,
	rayleighScatterAP1,
	aerosolType,
	aerosolLoading,
	aerosolHumidity,
	aerosolFalloff,
	aerosolPhaseG,
	aerosolScatter,
	aerosolAbsorption,
	ozoneAltitude,
	ozoneThickness,
	ozoneAbsorption,
	ozoneAbsorptionAP1,
	fallbackZBottom,
	enableVanillaClouds,
	cloudRelightMix,
	cloudOriginalMix,
	silverLiningMix,
	silverLiningSpread,
	enableVolumetricClouds,
	rayMarchRange,
	shadowVolumeRange,
	marchStepScale,
	cloudMap,
	cloudLayer,
	cloudNoise)

namespace
{
	RE::TESWorldSpace* GetCurrentWorldspace()
	{
		auto* tes = globals::game::tes ? globals::game::tes : RE::TES::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;

		auto* worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		if (!worldspace && cell)
			worldspace = cell->GetRuntimeData().worldSpace;

		return worldspace;
	}

	bool IsCurrentCellInterior()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		return cell && cell->IsInteriorCell();
	}

	std::string GetCurrentWorldspaceEditorID()
	{
		auto* worldspace = GetCurrentWorldspace();
		if (!worldspace)
			return {};

		return worldspace->GetFormEditorID();
	}

	std::string TrimEditorID(std::string editorID)
	{
		const auto first = std::ranges::find_if_not(editorID, [](unsigned char c) {
			return std::isspace(c);
		});
		const auto last = std::find_if_not(editorID.rbegin(), editorID.rend(), [](unsigned char c) {
			return std::isspace(c);
		}).base();

		if (first >= last)
			return {};

		return { first, last };
	}

	void InfoBox(const char* str)
	{
		if (ImGui::BeginTable("Info", 1, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
			ImGui::TableNextColumn();
			ImGui::TextWrapped(str);
			ImGui::EndTable();
		}
	}

	// Transform a float3 color/coefficient by a 3x3 color space matrix stored in a 4x4 Matrix
	float3 TransformColor(const float3& v, const DirectX::SimpleMath::Matrix& m)
	{
		return {
			v.x * m._11 + v.y * m._12 + v.z * m._13,
			v.x * m._21 + v.y * m._22 + v.z * m._23,
			v.x * m._31 + v.y * m._32 + v.z * m._33
		};
	}

	////////////////////////////////////////////////////////////////////////////////////////////////
	// Scene-scoped aerosol authoring.
	//
	// The scene layer blends numeric addresses, so a scene can only carry the aerosol appearance as
	// the coefficient triple the shader reads: the mixture, loading and humidity are authoring
	// inputs that derive that triple, and a discrete mixture cannot be interpolated at all. In a
	// scene page the feature therefore draws the picker below, which writes the derived coefficients
	// into the armed context's own entries, and leaves the authoring knobs to the main menu.

	constexpr std::string_view kSceneFeatureShortName = "PhysicalSky";

	/// Session UI state of the scene picker: the mixture a scene page is about to write, and the
	/// context it was seeded for. Not persisted, and deliberately outside `settings`.
	struct SceneAerosolPicker
	{
		Aerosol::Type type = Aerosol::Type::Custom;
		float loading = 1.f;
		float humidity = 50.f;
		SceneSettingsManager::SceneContextId context;
		bool seeded = false;
	};

	SceneAerosolPicker sceneAerosolPicker;

	/// One scene entry behind the aerosol optics the shader reads.
	struct AerosolOpticsAddress
	{
		std::string_view settingPath;  // a vector member carries its own path; the scalar has none
		std::string_view settingKey;
		const float* value;
	};

	/// The seven addresses a mixture resolves to. A vector's components are separate entries, keyed
	/// by component, which is how the catalog describes them.
	std::array<AerosolOpticsAddress, 7> GetAerosolOpticsAddresses(const Aerosol::Optics& a_optics)
	{
		return { { { "aerosolScatter", "x", &a_optics.scatter.x },
			{ "aerosolScatter", "y", &a_optics.scatter.y },
			{ "aerosolScatter", "z", &a_optics.scatter.z },
			{ "aerosolAbsorption", "x", &a_optics.absorption.x },
			{ "aerosolAbsorption", "y", &a_optics.absorption.y },
			{ "aerosolAbsorption", "z", &a_optics.absorption.z },
			{ "", "aerosolPhaseG", &a_optics.phaseG } } };
	}

	std::vector<std::string> GetAerosolOpticsSettingPath(const AerosolOpticsAddress& a_address)
	{
		std::vector<std::string> path;
		if (!a_address.settingPath.empty())
			path.emplace_back(a_address.settingPath);
		return path;
	}

	/// Whether an entry this context owns holds a value, rather than being a tombstone.
	bool IsAuthoredAerosolEntry(std::span<const SceneSettingsManager::SettingEntry> a_entries,
		std::optional<size_t> a_index)
	{
		return a_index && *a_index < a_entries.size() && !a_entries[*a_index].deleted;
	}

	/// Writes the coefficients a mixture resolves to into the armed context's own entries, so a
	/// weather or time-of-day transition interpolates the optics rather than switching the mixture.
	/// @return How many addresses the context now authors.
	size_t WriteSceneAerosolOptics(const SceneSettingsManager::SceneContextId& a_context,
		const Aerosol::Optics& a_optics)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto rules = SceneSettingsContextRules::GetSceneContextRules(a_context);
		const auto featureName = std::string(kSceneFeatureShortName);
		const auto addresses = GetAerosolOpticsAddresses(a_optics);

		// Structure first: clearing a tombstone or adding an entry renumbers the entries behind it,
		// so no index may be carried across this pass (the interceptor's guard resolves the same way).
		for (const auto& address : addresses) {
			const auto path = GetAerosolOpticsSettingPath(address);
			const auto key = std::string(address.settingKey);
			if (!SceneSettingsManager::IsSettingAllowedForType(rules.sceneType, featureName, path, key,
					rules.requireNumeric))
				continue;
			auto index = manager->FindContextUserEntry(a_context, featureName, path, key);
			if (index && !IsAuthoredAerosolEntry(manager->GetContextEntries(a_context), index)) {
				// A tombstone holds no value, so it goes before the new one lands.
				manager->ClearContextTombstone(a_context, featureName, path, key);
				index = std::nullopt;
			}
			if (!index)
				manager->AddContextSetting(a_context, featureName, path, key, true);
		}

		// Value pass: every address the context can hold now resolves to a live entry.
		std::array<SceneSettingsManager::EntryValueUpdate, 7> updates{};
		size_t count = 0;
		for (const auto& address : addresses) {
			const auto path = GetAerosolOpticsSettingPath(address);
			const auto key = std::string(address.settingKey);
			const auto index = manager->FindContextUserEntry(a_context, featureName, path, key);
			if (!index)
				continue;
			updates[count++] = { *index, json(*address.value) };
		}
		if (count == 0)
			return 0;

		// One atomic update, saved and reapplied once: the adds above only deferred their own save.
		manager->UpdateContextEntryValues(a_context, std::span(updates.data(), count));
		return count;
	}

	/// How many of the coefficients in a context come from the context's own entries.
	size_t CountSceneAerosolOptics(const SceneSettingsManager::SceneContextId& a_context)
	{
		const Aerosol::Optics none{};
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto featureName = std::string(kSceneFeatureShortName);
		const auto entries = manager->GetContextEntries(a_context);
		size_t authored = 0;
		for (const auto& address : GetAerosolOpticsAddresses(none)) {
			if (IsAuthoredAerosolEntry(entries,
					manager->FindContextUserEntry(a_context, featureName,
						GetAerosolOpticsSettingPath(address), std::string(address.settingKey))))
				++authored;
		}
		return authored;
	}

	/// Drops every coefficient this context authored, restoring inheritance from the layers below.
	void ClearSceneAerosolOptics(const SceneSettingsManager::SceneContextId& a_context)
	{
		const Aerosol::Optics none{};
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto featureName = std::string(kSceneFeatureShortName);
		for (const auto& address : GetAerosolOpticsAddresses(none)) {
			const auto path = GetAerosolOpticsSettingPath(address);
			const auto key = std::string(address.settingKey);
			const auto index = manager->FindContextUserEntry(a_context, featureName, path, key);
			if (index && IsAuthoredAerosolEntry(manager->GetContextEntries(a_context), index))
				manager->RemoveContextSetting(a_context, *index);
		}
	}

	/// The scene page's substitute for the authoring knobs: pick a mixture, and its coefficients
	/// become this context's own. The knobs themselves hold no scene value, which is why the scene
	/// policy bars them.
	void DrawSceneAerosolPicker(const SceneSettingsManager::SceneContextId& a_context,
		const char* const a_typeNames[], int a_typeCount)
	{
		const auto rules = SceneSettingsContextRules::GetSceneContextRules(a_context);
		// Judged on the scalar coefficient, which every context that can hold the aerosol look accepts.
		const bool allowed = SceneSettingsManager::IsSettingAllowedForType(rules.sceneType,
			std::string(kSceneFeatureShortName), {}, "aerosolPhaseG", rules.requireNumeric);

		ImGui::TextWrapped("%s", T(TKEY("aerosol_scene_desc"),
									 "Scenes blend the optical coefficients below, so a mixture is written as the coefficients it resolves to. Particle loading and relative humidity are authoring inputs and are not stored per scene."));

		ImGui::BeginDisabled(!allowed);

		bool changed = false;
		int type = static_cast<int>(sceneAerosolPicker.type);
		if (ImGui::Combo(T(TKEY("aerosol_type"), "Aerosol Type"), &type, a_typeNames, a_typeCount)) {
			sceneAerosolPicker.type = type > 0 && type < static_cast<int>(Aerosol::Type::Count) ?
			                              static_cast<Aerosol::Type>(type) :
			                              Aerosol::Type::Custom;
			changed = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("aerosol_type_desc"), "OPAC mixtures: continental clean, maritime clean, urban, and desert. Marine particles respond strongly to humidity; urban pollution includes absorbing soot. Dust also contains a water-soluble fraction."));

		if (sceneAerosolPicker.type != Aerosol::Type::Custom) {
			changed |= ImGui::SliderFloat(T(TKEY("aerosol_loading"), "Particle Loading"), &sceneAerosolPicker.loading, 0.f, 10.f, "%.2f x", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("aerosol_loading_desc"), "Multiplier of the selected type's reference dry particle number densities. 1x uses the OPAC reference mixture; 0 removes aerosols. Humidity changes particle size, not this loading. This is not AQI or PM2.5."));
			changed |= ImGui::SliderFloat(T(TKEY("aerosol_humidity"), "Relative Humidity"), &sceneAerosolPicker.humidity, 0.f, 99.f, "%.1f %%", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("aerosol_humidity_desc"), "Effective relative humidity throughout the aerosol layer. Uses OPAC hygroscopic growth data from 0 to 99% RH. Higher humidity usually increases scattering; absorption and forward scattering follow the selected mixture. Does not simulate condensation or fog droplets."));
		}

		if (changed && sceneAerosolPicker.type != Aerosol::Type::Custom) {
			WriteSceneAerosolOptics(a_context,
				Aerosol::Evaluate(sceneAerosolPicker.type, sceneAerosolPicker.loading, sceneAerosolPicker.humidity));
		}

		const auto authored = CountSceneAerosolOptics(a_context);
		if (authored > 0) {
			ImGui::Text(T(TKEY("aerosol_scene_authored"), "Aerosol coefficients stored for this scene: %zu of 7."), authored);
			ImGui::SameLine();
			if (ImGui::Button(T(TKEY("aerosol_scene_clear"), "Clear"))) {
				ClearSceneAerosolOptics(a_context);
				changed = true;
			}
		} else {
			ImGui::TextDisabled("%s", T(TKEY("aerosol_scene_inherited"), "Aerosol coefficients: inherited from the settings below the scene."));
		}

		ImGui::EndDisabled();

		if (!allowed)
			Util::AddTooltip(T(TKEY("aerosol_scene_unsupported"), "This place cannot hold the aerosol coefficients."), Util::kTooltipWhenDisabled);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void PhysicalSky::DataLoaded()
{
	if (!globals::features::skySync.loaded) {
		failedLoadedMessage = T("feature.physical_sky.sky_sync_required", "Sky Sync is required for Physical Sky to function.");
		loaded = false;
	}
}

void PhysicalSky::RestoreDefaultSettings()
{
	settings = {};
	// The scene picker mirrors the inputs it seeds from, so it goes back with them.
	sceneAerosolPicker = {};
}

void PhysicalSky::LoadSettings(json& o_json)
{
	// The aerosol mixture, loading and humidity are authoring inputs: the value the shader reads and
	// the scene layer blends is the coefficient triple they resolve to. A document that moves one of
	// the three (a config, a preset, a feature override) therefore has to re-derive that triple,
	// while a document carrying only the scene layer's own values leaves the inputs alone - which is
	// what keeps a blend of the inputs from overwriting the blend of the coefficients.
	const auto loadedType = settings.aerosolType;
	const auto loadedLoading = settings.aerosolLoading;
	const auto loadedHumidity = settings.aerosolHumidity;
	settings = o_json;
	if (settings.aerosolType != loadedType ||
		settings.aerosolLoading != loadedLoading ||
		settings.aerosolHumidity != loadedHumidity)
		settings.ApplyAerosolOptics();
}

void PhysicalSky::SaveSettings(json& o_json)
{
	o_json = settings;
}

Aerosol::Optics PhysicalSky::Settings::GetAerosolOptics() const
{
	if (aerosolType > Aerosol::Type::Custom && aerosolType < Aerosol::Type::Count)
		return Aerosol::Evaluate(aerosolType, aerosolLoading, aerosolHumidity);
	return { aerosolScatter, aerosolAbsorption, aerosolPhaseG };
}

void PhysicalSky::Settings::ApplyAerosolOptics()
{
	// Custom already answers with the members, so this leaves an authored coefficient set untouched.
	const auto optics = GetAerosolOptics();
	aerosolScatter = optics.scatter;
	aerosolAbsorption = optics.absorption;
	aerosolPhaseG = optics.phaseG;
}

void PhysicalSky::DrawSettings()
{
	if (SceneWidgetInterceptor::IsArmed())
		ImGui::TextWrapped("%s", T(TKEY("scene_settings_hint"), "Scene settings control sunlight, atmosphere, cloud motion, shape, density and distribution. Cloud lighting response, noise sources, textures, worldspace setup and rendering quality remain global. Interior settings require Physical Sky to be enabled for interiors."));

	if (ImGui::BeginTabBar("##PHYSSKY")) {
		if (ImGui::BeginTabItem(T(TKEY("general"), "General"))) {
			SettingsGeneral();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T(TKEY("celestials"), "Celestials"))) {
			SettingsCelestials();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T(TKEY("atmosphere"), "Atmosphere"))) {
			SettingsAtmosphere();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T(TKEY("clouds"), "Clouds"))) {
			SettingsClouds();
			ImGui::EndTabItem();
		}
		if (!SceneWidgetInterceptor::IsArmed() && ImGui::BeginTabItem(T(TKEY("debug"), "Debug"))) {
			SettingsDebug();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

void PhysicalSky::SettingsGeneral()
{
	const auto currentWorldspaceName = GetCurrentWorldspaceEditorID();
	const bool inInterior = IsCurrentCellInterior();

	if (ImGui::BeginTable("Info", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
		ImGui::TableNextColumn();
		ImGui::Text("%s", T(TKEY("shader_status"), "Shader Status: "));
		ImGui::TableNextColumn();
		if (ShadersOK())
			ImGui::TextColored({ 0, 1, 0, 1 }, T("feature.physical_sky.ok", "OK"));
		else
			ImGui::TextColored({ 1, 0, 0, 1 }, T("feature.physical_sky.error", "ERROR"));

		ImGui::TableNextColumn();
		ImGui::Text("%s", T(TKEY("worldspace"), "Worldspace: "));
		ImGui::TableNextColumn();

		if (inInterior) {
			if (settings.forceEnableAllInteriorCells)
				ImGui::Text("%s", T(TKEY("interior_enabled_forced"), "Interior (Enabled, Forced)"));
			else
				ImGui::Text("%s", T(TKEY("interior_disabled"), "Interior (Disabled)"));
		} else if (!currentWorldspaceName.empty()) {
			if (settings.worldspaceWhitelist.contains(currentWorldspaceName)) {
				ImGui::Text(T(TKEY("enabled_whitelist"), "%s (Enabled, Whitelist)"), currentWorldspaceName.c_str());
			} else if (settings.enableAllExteriorCells) {
				ImGui::Text(T(TKEY("enabled_fallback_z_bottom"), "%s (Enabled, Fallback Z Bottom)"), currentWorldspaceName.c_str());
			} else {
				ImGui::Text(T(TKEY("disabled"), "%s (Disabled)"), currentWorldspaceName.c_str());
			}
		} else {
			ImGui::Text("%s", T(TKEY("unknown_worldspace_unavailable"), "Unknown (Worldspace Unavailable)"));
		}

		ImGui::EndTable();
	}

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.enabled);
	ImGui::SameLine();
	ImGui::Checkbox(T(TKEY("enable_all_exterior_cells"), "Enable All Exterior Cells"), &settings.enableAllExteriorCells);
	ImGui::SameLine();
	ImGui::Checkbox(T(TKEY("force_enable_all_interior_cells"), "Force Enable All Interior Cells"), &settings.forceEnableAllInteriorCells);
	if (settings.enableAllExteriorCells || settings.forceEnableAllInteriorCells) {
		ImGui::InputFloat(T(TKEY("fallback_z_bottom"), "Fallback Z Bottom"), &settings.fallbackZBottom, 10.f, 100.f, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("used_when_current_worldspace_is_not_in_whitelist"), "Used when current worldspace is not in whitelist (or worldspace data is unavailable), including forced interiors."));
	}

	if (!SceneWidgetInterceptor::IsArmed()) {
		ImGui::SeparatorText(T(TKEY("worldspace_whitelist"), "Worldspace Whitelist"));
		static std::string newWorldspaceEditorID;
		static float newWorldspaceZBottom = -14500.f;

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
		ImGui::InputTextWithHint(T(TKEY("editor_id"), "Editor ID"), "Tamriel", &newWorldspaceEditorID);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::InputFloat(T(TKEY("z_bottom"), "Z Bottom"), &newWorldspaceZBottom, 10.f, 100.f, "%.1f");

		const auto addOrUpdateWorldspace = [&](std::string editorID, float zBottom) {
			editorID = TrimEditorID(std::move(editorID));
			if (!editorID.empty())
				settings.worldspaceWhitelist[editorID].zBottom = zBottom;
		};

		if (ImGui::Button(T(TKEY("add_update"), "Add / Update"))) {
			addOrUpdateWorldspace(newWorldspaceEditorID, newWorldspaceZBottom);
		}

		if (!currentWorldspaceName.empty()) {
			ImGui::SameLine();
			const auto currentIt = settings.worldspaceWhitelist.find(currentWorldspaceName);
			const bool currentWhitelisted = currentIt != settings.worldspaceWhitelist.end();
			if (ImGui::Button(currentWhitelisted ? T(TKEY("remove_current_worldspace"), "Remove Current Worldspace") : T(TKEY("add_current_worldspace"), "Add Current Worldspace"))) {
				if (currentWhitelisted) {
					settings.worldspaceWhitelist.erase(currentIt);
				} else {
					addOrUpdateWorldspace(currentWorldspaceName, newWorldspaceZBottom);
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", currentWorldspaceName.c_str());
		}

		if (ImGui::BeginTable("WorldspaceWhitelist", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, { -1, 0 })) {
			ImGui::TableSetupColumn(T(TKEY("editor_id"), "Editor ID"));
			ImGui::TableSetupColumn(T(TKEY("z_bottom"), "Z Bottom"), ImGuiTableColumnFlags_WidthFixed, 160.f);
			ImGui::TableSetupColumn(T(TKEY("action"), "Action"), ImGuiTableColumnFlags_WidthFixed, 90.f);
			ImGui::TableHeadersRow();

			std::string removeEditorID;
			for (auto& [editorID, info] : settings.worldspaceWhitelist) {
				ImGui::PushID(editorID.c_str());
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(editorID.c_str());

				ImGui::TableSetColumnIndex(1);
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputFloat("##ZBottom", &info.zBottom, 10.f, 100.f, "%.1f");

				ImGui::TableSetColumnIndex(2);
				if (ImGui::Button(T(TKEY("remove"), "Remove"), { -1, 0 }))
					removeEditorID = editorID;

				ImGui::PopID();
			}

			if (!removeEditorID.empty())
				settings.worldspaceWhitelist.erase(removeEditorID);

			ImGui::EndTable();
		}
	}

	ImGui::SeparatorText(T(TKEY("post_processing"), "Post Processing"));
	{
		ImGui::SliderFloat(T(TKEY("vanilla_mix"), "Vanilla Mix"), &settings.vanillaMix, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("blend_in_vanilla_sky_color"), "Blend in vanilla sky color."));
	}
}

void PhysicalSky::SettingsCelestials()
{
	InfoBox(T(TKEY("the_sun_and_moons_and_their_lights"), "The sun and moons, and their lights."));

	ImGui::Checkbox(T(TKEY("override_directional_light"), "Override Directional Light"), &settings.overrideDirLight);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("overrides_the_color_of_directional_light_linear_tonemapper"), "Overrides the color of directional light. A 1.0 transmittance mix is recommended."));
	ImGui::SliderFloat(T(TKEY("transmittance_mix"), "Transmittance Mix"), &settings.trMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("apply_additional_atmospheric_tranmisttance_on_the_directional_light"),
							  "Apply additional atmospheric tranmisttance on the directional light.\n"
							  "Introduces natural yellowening at sunset with white sunlight."));

	ImGui::SeparatorText(T(TKEY("sky_statics"), "Sky Statics"));
	{
		ImGui::Checkbox(T(TKEY("light_sky_statics"), "Light Sky Statics"), &settings.lightSkyStatics);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("treat_sky_statics_as_albedo_and_tint_them"), "Treat sky statics as albedo and tint them with directional lighting."));

		ImGui::BeginDisabled(!settings.lightSkyStatics);
		ImGui::SliderFloat(T(TKEY("brightness"), "Brightness"), &settings.skyStaticsBrightness, 0.f, 4.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("multiplies_directional_lighting_applied_to_sky_statics"), "Multiplies directional lighting applied to sky statics."));
		ImGui::EndDisabled();
	}

	ImGui::SeparatorText(T(TKEY("sun"), "Sun"));
	{
		ImGui::PushID("Sun");
		ImGui::ColorEdit3(T(TKEY("light_color"), "Light Color"), &settings.sunlightColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("light_color_hint"), "This sets the light color BEFORE it goes through the atmosphere i.e. extraterrestrial radiance."));
		ImGui::Checkbox(T(TKEY("procedural_sun"), "Procedural Sun"), &settings.proceduralSun);
		ImGui::SliderAngle(T(TKEY("sun_disk_angular_radius"), "Sun Disk Angular Radius"), &settings.sunDiskRad, 0.f, 10.f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("real_world_sun_disk_angular_radius_is_about"), "Real world sun disk angular radius is about 0.27 degrees."));
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("masser"), "Masser"));
	{
		ImGui::PushID("Masser");
		ImGui::ColorEdit3(T(TKEY("light_color"), "Light Color"), &settings.masserColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("light_color_hint"), "This sets the light color BEFORE it goes through the atmosphere i.e. extraterrestrial radiance."));
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("secunda"), "Secunda"));
	{
		ImGui::PushID("Secunda");
		ImGui::ColorEdit3(T(TKEY("light_color"), "Light Color"), &settings.secundaColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("light_color_hint"), "This sets the light color BEFORE it goes through the atmosphere i.e. extraterrestrial radiance."));
		ImGui::PopID();
	}
}

void PhysicalSky::SettingsAtmosphere()
{
	InfoBox(T(TKEY("the_composition_and_physical_properties_of_the_atmosphere"), "The composition and physical properties of the atmosphere."));

	ImGui::SliderFloat(T(TKEY("ap_luminance_mix"), "AP Luminance Mix"), &settings.apLumMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("add_light_scattered_by_air_aerial_perspective_to"), "Add light scattered by air (Aerial Perspective) to the scene."));
	ImGui::SliderFloat(T(TKEY("ap_transmittance_mix"), "AP Transmittance Mix"), &settings.apTrMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("remove_light_absorbed_by_air_aerial_perspective_from"), "Remove light absorbed by air (Aerial Perspective) from the scene."));

	ImGui::Checkbox(T(TKEY("half_resolution_cloud_shadow"), "Half Resolution Cloud Shadow"), &settings.halfResApShadow);

	ImGui::SliderFloat2(T(TKEY("cloud_shadow_remap"), "Cloud Shadow Remap"), &settings.cloudShadowRemapRange.x, 0.f, 1.f, "%.2f");

	ImGui::SeparatorText(T(TKEY("air_molecules_rayleigh"), "Air Molecules (Rayleigh)"));
	{
		ImGui::PushID("Rayleigh");
		ImGui::TextWrapped("%s", T(TKEY("particles_much_smaller_than_the_wavelength_of_light"),
									 "Particles much smaller than the wavelength of light. They have almost complete symmetry in forward and backward scattering. "
									 "On earth, they are what makes the sky blue and, at sunset, red. Usually needs no extra change."));

		ImGui::ColorEdit3(T(TKEY("scatter_srgb"), "Scatter (sRGB)"), &settings.rayleighScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit3(T(TKEY("scatter_ap1"), "Scatter (AP1)"), &settings.rayleighScatterAP1.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("band_averaged_coefficients_for_acescg_ap1_primaries_computed"), "Band-averaged coefficients for ACEScg (AP1) primaries, computed via CIE 1931 spectral integration."));
		ImGui::SliderFloat(T(TKEY("falloff"), "Falloff"), &settings.rayleighFalloff, 0.f, 2.f, "%.2f km^-1");
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("aerosol_mie"), "Aerosol (Mie)"));
	{
		ImGui::PushID("Mie");
		ImGui::TextWrapped("%s", T(TKEY("aerosol_model_desc"), "Suspended particles scatter and absorb light. Choose an aerosol type to control particle loading and relative humidity, or Custom to edit optical coefficients."));
		const char* types[] = {
			T(TKEY("aerosol_custom"), "Custom"),
			T(TKEY("aerosol_continental"), "Continental background"),
			T(TKEY("aerosol_maritime"), "Marine"),
			T(TKEY("aerosol_urban"), "Urban pollution"),
			T(TKEY("aerosol_desert"), "Desert dust")
		};
		// The proxy stays a plain cast of the stored enum: the scene settings catalog
		// resolves a combo's backing setting through exactly this shape, and a ternary
		// here would leave the setting out of the catalog.
		int type = static_cast<int>(settings.aerosolType);

		// A scene page authors the coefficients the shader reads, because that is the only part of
		// the aerosol look a transition can interpolate; the main menu authors the mixture those
		// coefficients come from. Both end up at the same three addresses, so the widgets below are
		// drawn once and the mixture never has to be an interpolated value.
		const auto* scene = SceneWidgetInterceptor::GetArmedContext();
		const bool sceneScoped = scene && !scene->baseline;
		if (sceneScoped) {
			// Seed once per context: a scene page opens on the look the author already sees.
			if (!sceneAerosolPicker.seeded || sceneAerosolPicker.context != scene->contextId) {
				sceneAerosolPicker.type = settings.aerosolType < Aerosol::Type::Count ?
				                              settings.aerosolType :
				                              Aerosol::Type::Custom;
				sceneAerosolPicker.loading = settings.aerosolLoading;
				sceneAerosolPicker.humidity = settings.aerosolHumidity;
				sceneAerosolPicker.context = scene->contextId;
				sceneAerosolPicker.seeded = true;
			}
			DrawSceneAerosolPicker(scene->contextId, types, IM_ARRAYSIZE(types));
		} else {
			if (type < 0 || type >= static_cast<int>(Aerosol::Type::Count))
				type = 0;
			if (ImGui::Combo(T(TKEY("aerosol_type"), "Aerosol Type"), &type, types, IM_ARRAYSIZE(types))) {
				// Selecting a mixture re-derives the coefficients; switching back to Custom keeps
				// them, because a physical mixture had already written them, so the look is
				// continuous either way.
				settings.aerosolType = static_cast<Aerosol::Type>(type);
				settings.ApplyAerosolOptics();
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("aerosol_type_desc"), "OPAC mixtures: continental clean, maritime clean, urban, and desert. Marine particles respond strongly to humidity; urban pollution includes absorbing soot. Dust also contains a water-soluble fraction."));

			if (type != 0) {
				if (ImGui::SliderFloat(T(TKEY("aerosol_loading"), "Particle Loading"), &settings.aerosolLoading, 0.f, 10.f, "%.2f x", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
					settings.ApplyAerosolOptics();
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("aerosol_loading_desc"), "Multiplier of the selected type's reference dry particle number densities. 1x uses the OPAC reference mixture; 0 removes aerosols. Humidity changes particle size, not this loading. This is not AQI or PM2.5."));
				if (ImGui::SliderFloat(T(TKEY("aerosol_humidity"), "Relative Humidity"), &settings.aerosolHumidity, 0.f, 99.f, "%.1f %%", ImGuiSliderFlags_AlwaysClamp))
					settings.ApplyAerosolOptics();
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("aerosol_humidity_desc"), "Effective relative humidity throughout the aerosol layer. Uses OPAC hygroscopic growth data from 0 to 99% RH. Higher humidity usually increases scattering; absorption and forward scattering follow the selected mixture. Does not simulate condensation or fog droplets."));
			}
		}
		// A scene page shows the coefficients whatever mixture they came from: they are what that
		// page stores, and the picker above is only a shortcut for writing them.
		if (sceneScoped || type == 0) {
			ImGui::SliderFloat(T(TKEY("anisotropy"), "Anisotropy"), &settings.aerosolPhaseG, -0.999f, 0.999f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::ColorEdit3(T(TKEY("scatter"), "Scatter"), &settings.aerosolScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
			ImGui::ColorEdit3(T(TKEY("absorption"), "Absorption"), &settings.aerosolAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		}
		if (ImGui::TreeNode(T(TKEY("aerosol_vertical_distribution"), "Vertical Distribution"))) {
			ImGui::SliderFloat(T(TKEY("falloff"), "Falloff"), &settings.aerosolFalloff, 0.f, 2.f, "%.2f km^-1");
			ImGui::TreePop();
		}
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("ozone"), "Ozone"));
	{
		ImGui::PushID("Ozone");
		ImGui::TextWrapped("%s", T(TKEY("the_ozone_layer_high_up_in_the_sky"),
									 "The ozone layer high up in the sky that mainly absorbs light of certain wavelength. "
									 "It keeps the zenith sky blue, especially at sunrise or sunset."));

		ImGui::ColorEdit3(T(TKEY("absorption_srgb"), "Absorption (sRGB)"), &settings.ozoneAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit3(T(TKEY("absorption_ap1"), "Absorption (AP1)"), &settings.ozoneAbsorptionAP1.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("band_averaged_coefficients_for_acescg_ap1_primaries_computed_2"), "Band-averaged coefficients for ACEScg (AP1) primaries, computed via CIE 1931 spectral integration."));
		ImGui::DragFloat(T(TKEY("mean_altitude"), "Mean Altitude"), &settings.ozoneAltitude, .1f, 0.f, 100.f, "%.3f km");
		ImGui::DragFloat(T(TKEY("layer_thickness"), "Layer Thickness"), &settings.ozoneThickness, .1f, 0.f, 50.f, "%.3f km");
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("planetary_parameters"), "Planetary Parameters"));
	{
		ImGui::InputFloat(T(TKEY("planet_radius"), "Planet Radius"), &settings.planetRadius, 1.f, 100000.f, "%.1f km");
		ImGui::InputFloat(T(TKEY("atmosphere_radius"), "Atmosphere Radius"), &settings.atmosphereRadius, 1.f, 100000.f, "%.1f km");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("planet_radius_is_the_distance_from_the_planet"),
								  "Planet radius is the distance from the planet center to sea level.\n"
								  "Atmosphere radius is the distance from the planet center to the top of atmosphere.\n"
								  "On Earth, they are about 6360 km and 6420 km respectively."));
	}
}

void PhysicalSky::SettingsClouds()
{
	ImGui::Checkbox(T(TKEY("enable_vanilla_clouds"), "Enable Vanilla Clouds"), &settings.enableVanillaClouds);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("enable_vanilla_clouds_tooltip"), "Enable vanilla cloud geometry rendering with Physical Sky relighting."));

	ImGui::BeginDisabled(!settings.enableVanillaClouds);
	{
		ImGui::SliderFloat(T(TKEY("vanilla_mix"), "Vanilla Mix"), &settings.cloudOriginalMix, 0.f, 2.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("relight_mix"), "Relight Mix"), &settings.cloudRelightMix, 0.f, 2.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("silver_lining_accent"), "Silver Lining Accent"), &settings.silverLiningMix, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("silver_lining_spread"), "Silver Lining Spread"), &settings.silverLiningSpread, -0.99f, 0.99f, "%.2f");
	}
	ImGui::EndDisabled();

	ImGui::Separator();

	ImGui::Checkbox(T(TKEY("enable_volumetric_clouds"), "Enable Volumetric Clouds"), &settings.enableVolumetricClouds);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("enable_volumetric_clouds_tooltip"), "Enable ray-marched volumetric clouds with NDF-based cloud shapes."));

	ImGui::BeginDisabled(!settings.enableVolumetricClouds);
	SettingsVolumetricClouds();
	ImGui::EndDisabled();
}

void PhysicalSky::SettingsVolumetricClouds()
{
	auto& low = settings.cloudLayer.low;
	auto& cirrus = settings.cloudLayer.cirrus;
	auto& lighting = settings.cloudLayer.lighting;

	ImGui::SeparatorText(T(TKEY("performance"), "Performance"));
	{
		ImGui::SliderFloat(T(TKEY("ray_march_range"), "Ray March Range"), &settings.rayMarchRange, 1.f, 64.f, "%.1f km");
		ImGui::SliderFloat(T(TKEY("shadow_volume_range"), "Shadow Volume Range"), &settings.shadowVolumeRange, 1.f, 16.f, "%.1f km");
		ImGui::SliderFloat(T(TKEY("cloud_march_step_scale"), "Cloud March Step Scale"), &settings.marchStepScale, 0.125f, 2.f, "%.3f", ImGuiSliderFlags_Logarithmic);
	}

	ImGui::SeparatorText(T(TKEY("composition"), "Low Clouds"));
	{
		ImGui::DragFloat(T(TKEY("ndf_altitude_offset"), "NDF Base Altitude"), &low.ndfAltitudeOffset, 1.f, 0.f, 20000.f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		ImGui::DragFloat(T(TKEY("ndf_altitude_scale"), "NDF Height Span"), &low.ndfAltitudeScale, 1.f, 1.f, 20000.f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("ndf_altitude_encoding"), "Altitude = base altitude + NDF height * height span. Applies to both generated and imported maps."));
		ImGui::SliderFloat2(T(TKEY("ndf_scale"), "NDF Scale"), &low.ndfScale.x, 1.f, 50.f, "%.2f km");
		ImGui::SliderFloat(T(TKEY("cloud_coverage_bottom_power"), "Coverage Bottom Power"), &low.coverageBottomPower, 0.01f, 4.f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_coverage_height_range"), "Coverage Height Range"), &low.coverageHeightRange, 0.001f, 1.f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_bottom_density_power"), "Bottom Density Power"), &low.bottomDensityPower, 0.f, 10.f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_bottom_density_width"), "Bottom Density Width"), &low.bottomDensityWidth, 1.f, 10.f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_top_expansion"), "Top Expansion"), &low.topExpansion, 0.f, 1.f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_density_scale"), "Density Scale"), &low.densityScale, 0.f, 1.f, "%.3f 1/m");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("cloud_density_scale_desc"), "Converts reconstructed density to extinction per metre. Shared by view opacity, light sampling and cloud shadows. NDF heights stay fixed."));
	}

	ImGui::SeparatorText(T(TKEY("cloud_motion"), "Cloud Motion"));
	{
		auto& wind = settings.cloudLayer.wind;
		ImGui::SliderFloat2(T(TKEY("cloud_low_wind_velocity"), "Low Cloud Velocity (X/Y)"), &wind.lowVelocity.x, -80.f, 80.f, "%.1f m/s", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat2(T(TKEY("cloud_high_wind_velocity"), "High Cloud Velocity (X/Y)"), &wind.highVelocity.x, -80.f, 80.f, "%.1f m/s", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("cloud_wind_velocity_desc"), "Horizontal velocity in world X/Y axes. Scene transitions blend the components, including across direction changes. The resulting travel speed is capped at 80 m/s."));
		ImGui::SliderFloat(T(TKEY("cloud_development"), "Development Speed"), &wind.development, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("cloud_development_desc"), "Controls internal low-cloud evolution. Zero freezes development while wind continues to carry the clouds."));
		ImGui::SliderFloat(T(TKEY("cloud_disturbance"), "Disturbance Strength"), &wind.disturbance, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("cloud_disturbance_desc"), "Controls local low-cloud deformation. High and low wind differences also produce a gradual cloud lean."));
	}

	CirrusMapManager::DrawSettings(cirrus, ndfTexManager);

	ImGui::SeparatorText(T(TKEY("lighting"), "Lighting"));
	{
		ImGui::SliderFloat(T(TKEY("cloud_lighting_scale"), "Lighting Response Scale"), &lighting.lightingScale, 0.0f, 4.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_sun_extinction"), "Sun Extinction Scale"), &lighting.sunExtinction, 0.0f, 4.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_phase_forward_g"), "Forward Phase G"), &lighting.phaseForwardG, 0.0f, 0.95f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_phase_backward_g"), "Second Phase G"), &lighting.phaseBackwardG, -0.95f, 0.95f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_phase_forward_weight"), "Forward Phase Weight"), &lighting.phaseForwardWeight, 0.0f, 1.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_phase_backward_weight"), "Second Phase Weight"), &lighting.phaseBackwardWeight, 0.0f, 1.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_scatter_volume_strength"), "Scattering Volume Strength"), &lighting.scatterVolumeStrength, 0.0f, 1.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_scatter_volume_depth"), "Scattering Volume Depth"), &lighting.scatterVolumeDepth, 0.001f, 1.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_scatter_volume_height"), "Scattering Volume Height"), &lighting.scatterVolumeHeight, 0.0f, 4.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_soft_scattering_strength"), "Soft Scattering Strength"), &lighting.softScatteringStrength, 0.0f, 1.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_powder_strength"), "Powder Strength"), &lighting.powderStrength, 0.0f, 1.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_ambient_strength"), "Ambient Response Strength"), &lighting.ambientStrength, 0.0f, 8.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_ambient_floor"), "Ambient Response Floor"), &lighting.ambientFloor, 0.0f, 1.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_ambient_density"), "Ambient Density Response"), &lighting.ambientDensity, 0.0f, 1.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_ambient_base"), "Ambient Base Response"), &lighting.ambientBase, 0.0f, 1.0f, "%.3f");
	}

	ImGui::SeparatorText(T(TKEY("cloud_map"), "Cloud Map"));
	ndfManager.DrawNdfSettings(settings.cloudMap, ndfTexManager);
	if (!SceneWidgetInterceptor::IsArmed()) {
		cloudNoiseGenerator.DrawSettings(settings.cloudNoise);
		if (ImGui::Button(T(TKEY("reload_cloud_textures"), "Reload Cloud Textures"), { -FLT_MIN, 0 }))
			LoadCloudTextures();
		if (baseShapeNoiseSrv && cloudProfileLutSrv && cloudAdjustmentLutSrv)
			ImGui::TextColored({ 0, 1, 0, 1 }, "%s", T(TKEY("cloud_textures_loaded"), "Cloud Textures: Loaded"));
		else
			ImGui::TextColored({ 1, 0.7f, 0.2f, 1 }, "%s", T(TKEY("cloud_textures_preparing"), "Cloud Textures: Preparing"));
	}
}

void PhysicalSky::SettingsDebug()
{
	InfoBox(T(TKEY("beep_boop"), "Beep Boop."));

	if (ImGui::Button(T(TKEY("recompile_shaders"), "Recompile Shaders")))
		ClearShaderCache();

	ImGui::SeparatorText(T(TKEY("values"), "Values"));
	{
		ImGui::InputFloat3(T(TKEY("sun_direction"), "Sun Direction"), &cbData.sunDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		ImGui::InputFloat3(T(TKEY("masser_direction"), "Masser Direction"), &cbData.masserDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		ImGui::InputFloat3(T(TKEY("secunda_direction"), "Secunda Direction"), &cbData.secundaDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
	}

	ImGui::SeparatorText(T(TKEY("textures"), "Textures"));
	{
		static float debugScale = 0.2f;
		ImGui::SliderFloat(T(TKEY("view_scale"), "View Scale"), &debugScale, 0.1f, 1.f);

		static const char* cubeFaces[] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
		if (ImGui::SliderInt(T(TKEY("cubemap_face"), "Cubemap Face"), &debugCubeFace, 0, 5, cubeFaces[debugCubeFace]))
			debugCubeFaceSrvs.clear();

		if (ImGui::TreeNode(T(TKEY("sky"), "Sky"))) {
			BUFFER_VIEWER_NODE(texTrLut, debugScale);
			BUFFER_VIEWER_NODE(texMsLut, debugScale);
			BUFFER_VIEWER_NODE(texSvLut, debugScale);
			BUFFER_VIEWER_NODE(texApShadow, debugScale);
			DrawDebugVolume(texApLut ? texApLut->srv.get() : nullptr, "texApLut", debugApSlice, debugScale);
			ImGui::TreePop();
		}

		if (ImGui::TreeNode(T(TKEY("cloud_shape"), "Cloud Shape"))) {
			ndfManager.DrawPreview();
			const auto ndf = ndfManager.GetNdf(settings.cloudMap, ndfTexManager);
			DrawDebugCloudTexture(ndf.height, "ndfHeight", T(TKEY("debug_ndf_height"), "NDF height / shaping start"), debugScale);
			DrawDebugCloudTexture(ndf.modeling, "ndfModeling", T(TKEY("debug_ndf_modeling"), "NDF coverage / types"), debugScale);
			if (ImGui::TreeNode(T(TKEY("debug_weather_noise"), "Shared weather noise inputs"))) {
				const char* labels[] = {
					T(TKEY("debug_weather_noise_0"), "Noise input 0"),
					T(TKEY("debug_weather_noise_1"), "Noise input 1"),
					T(TKEY("debug_weather_noise_2"), "Noise input 2"),
					T(TKEY("debug_weather_noise_3"), "Noise input 3")
				};
				for (uint32_t i = 0; i < 4; ++i) {
					const auto id = std::format("weatherNoise{}", i);
					DrawDebugCloudTexture(ndfManager.GetNoiseInputs()[i], id.c_str(), labels[i], debugScale, true);
				}
				ImGui::TreePop();
			}
			DrawDebugCloudTexture(baseShapeNoiseSrv.get(), "activeShape", T(TKEY("debug_active_shape"), "Active cloud shape noise"), debugScale);
			DrawDebugCloudTexture(cloudProfileLutSrv.get(), "activeProfile", T(TKEY("debug_active_profile"), "Active vertical profile LUT"), debugScale);
			DrawDebugCloudTexture(cloudAdjustmentLutSrv.get(), "activeAdjustment", T(TKEY("debug_active_adjustment"), "Active top expansion / warp LUT"), debugScale);
			const auto cirrus = cirrusMapManager.GetTextures();
			DrawDebugCloudTexture(cirrus.weather, "cirrusWeather", T(TKEY("debug_cirrus_weather"), "Active cirrus coverage / type"), debugScale);
			DrawDebugCloudTexture(cirrus.patterns, "cirrusPatterns", T(TKEY("debug_cirrus_patterns"), "Active cirrus patterns"), debugScale);
			if (ImGui::TreeNode(T(TKEY("debug_noise_sources"), "Generated and source textures"))) {
				DrawDebugCloudTexture(cloudNoiseGenerator.Shape(), "generatedShape", T(TKEY("debug_generated_shape"), "Generated cloud shape noise"), debugScale);
				DrawDebugCloudTexture(cloudNoiseGenerator.Adjustment(), "generatedAdjustment", T(TKEY("debug_generated_adjustment"), "Generated top expansion / warp LUT"), debugScale);
				DrawDebugCloudTexture(importedShapeNoiseSrv.get(), "importedShape", T(TKEY("debug_imported_shape"), "Cloud shape DDS input"), debugScale);
				DrawDebugCloudTexture(importedAdjustmentLutSrv.get(), "sourceAdjustment", T(TKEY("debug_source_adjustment"), "Adjustment source (DDS or fallback)"), debugScale);
				ImGui::TreePop();
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNode(T(TKEY("cloud_trace"), "Cloud Trace"))) {
			BUFFER_VIEWER_NODE(texVolLowTr, debugScale);
			BUFFER_VIEWER_NODE(texVolLowLum, debugScale);
			BUFFER_VIEWER_NODE(texVolLowAux, debugScale);
			ImGui::TreePop();
		}

		if (ImGui::TreeNode(T(TKEY("cloud_reproject"), "Cloud Reproject"))) {
			BUFFER_VIEWER_NODE(texVolTr, debugScale);
			BUFFER_VIEWER_NODE(texVolLum, debugScale);
			BUFFER_VIEWER_NODE(texVolAux, debugScale);
			BUFFER_VIEWER_NODE(texVolFilteredTr, debugScale);
			BUFFER_VIEWER_NODE(texVolFilteredLum, debugScale);
			ImGui::TreePop();
		}

		if (ImGui::TreeNode(T(TKEY("cloud_history"), "Cloud History"))) {
			BUFFER_VIEWER_NODE(texVolHistoryTr, debugScale);
			BUFFER_VIEWER_NODE(texVolHistoryLum, debugScale);
			BUFFER_VIEWER_NODE(texVolHistoryAux, debugScale);
			ImGui::TreePop();
		}

		if (ImGui::TreeNode(T(TKEY("cloud_cubemap"), "Cloud Cubemap"))) {
			DrawDebugCube(texVolCubeTraceTr.get(), "texVolCubeTraceTr", debugScale);
			DrawDebugCube(texVolCubeTraceLum.get(), "texVolCubeTraceLum", debugScale);
			DrawDebugCube(texVolCubeTraceAux.get(), "texVolCubeTraceAux", debugScale);
			DrawDebugCube(texVolCubeTr.get(), "texVolCubeTr", debugScale);
			DrawDebugCube(texVolCubeLum.get(), "texVolCubeLum", debugScale);
			DrawDebugCube(texVolCubeAux.get(), "texVolCubeAux", debugScale);
			DrawDebugCube(texVolCubeHistoryTr.get(), "texVolCubeHistoryTr", debugScale);
			DrawDebugCube(texVolCubeHistoryLum.get(), "texVolCubeHistoryLum", debugScale);
			DrawDebugCube(texVolCubeHistoryAux.get(), "texVolCubeHistoryAux", debugScale);
			ImGui::TreePop();
		}

		if (ImGui::TreeNode(T(TKEY("cloud_lighting"), "Cloud Lighting"))) {
			BUFFER_VIEWER_NODE(texVolCloudAmbientSH, 30.f);
			DrawDebugVolume(texShadowVolume ? texShadowVolume->srv.get() : nullptr, "texShadowVolume", debugShadowVolumeSlice, debugScale);
			ImGui::TreePop();
		}
	}
}

#undef I18N_KEY_PREFIX

void PhysicalSky::SetupResources()
{
	auto device = globals::d3d::device;

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampTr.put()));

		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampSv.put()));

		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampNoise.put()));
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC tex2dDesc{
			.Width = kTrLutW,
			.Height = kTrLutH,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = tex2dDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = tex2dDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texTrLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texTrLut->CreateSRV(srvDesc);
		texTrLut->CreateUAV(uavDesc);

		tex2dDesc.Width = kMsLutW;
		tex2dDesc.Height = kMsLutH;

		texMsLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texMsLut->CreateSRV(srvDesc);
		texMsLut->CreateUAV(uavDesc);

		tex2dDesc.Width = kSvLutW;
		tex2dDesc.Height = kSvLutH;

		texSvLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texSvLut->CreateSRV(srvDesc);
		texSvLut->CreateUAV(uavDesc);

		D3D11_TEXTURE3D_DESC tex3dDesc{
			.Width = kApLutW,
			.Height = kApLutH,
			.Depth = kApLutD,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 };
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
		uavDesc.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = kApLutD };

		texApLut = eastl::make_unique<Texture3D>(tex3dDesc);
		texApLut->CreateSRV(srvDesc);
		texApLut->CreateUAV(uavDesc);
	}
	{
		D3D11_TEXTURE2D_DESC texDesc;
		auto mainTex = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);
		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MipLevels = 1;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texApShadow = eastl::make_unique<Texture2D>(texDesc);
		texApShadow->CreateSRV(srvDesc);
		texApShadow->CreateUAV(uavDesc);
	}

	CompileShaders();

	// Volumetric cloud resources
	SetupVolumetricResources();
}

void PhysicalSky::ClearShaderCache()
{
	CompileShaders();
	CompileVolumetricShaders();
}

void PhysicalSky::CompileShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* csPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines = {};
		std::string_view entry = "main";
	};

	std::vector<ShaderCompileInfo> shaderInfos = {
		{ &csTrLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "0" } } },
		{ &csMsLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "1" } } },
		{ &csSvLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "2" } } },
		{ &csApLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "3" } } },
		{ &csShadowAccum, "ShadowAccum.cs.hlsl", {} },
		{ &csShadowAccumHalfRes, "ShadowAccum.cs.hlsl", { { "HALF_RES", "" } } }
	};

	for (auto& info : shaderInfos) {
		*info.csPtr = nullptr;
		auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.data())))
			info.csPtr->attach(rawPtr);
	}
}

bool PhysicalSky::ShadersOK()
{
	bool baseShadersOk = csTrLutGen && csMsLutGen && csSvLutGen && csApLutGen && csShadowAccum && csShadowAccumHalfRes &&
	                     texTrLut && texSvLut && texApLut && texApShadow;
	// The cloud maps themselves are created lazily by the first generation
	// dispatch, so readiness is a property of the generation shaders. The render
	// path still verifies every texture before binding.
	const bool ndfReady = (ndfManager.texHeight && ndfManager.texModeling && ndfManager.generatorProgram && ndfManager.noiseProgram);
	const bool cirrusMapsReady = !settings.cloudLayer.cirrus.enabled || cirrusMapManager.ShadersReady(settings.cloudLayer.cirrus);
	bool volumetricShadersOk = !settings.enableVolumetricClouds ||
	                           (vsCloudBoundary && psCloudBoundary && texCloudBoundary && cloudBoundaryCB && cloudBoundaryIndices && cloudBoundaryBlend && cloudBoundaryRasterizer && cloudBoundaryDepth &&
								   csCloudHeightBounds && csCloudHeightBoundsReduce && texCloudHeightBounds && texCloudHeightBoundsTiles &&
								   csVolMainView && csVolFilter && csVolReproject && csVolCubeReproject && csVolShadowVolume && csVolShadowResample && texShadowVolumeRaw && csVolCubemap && csVolAmbientSH && texVolCloudAmbientSH &&
								   texVolTr && texVolLum && texVolAux && texVolLowTr && texVolLowLum && texVolLowAux &&
								   texVolFilteredTr && texVolFilteredLum && texVolFilteredAux &&
								   texVolHistoryTr && texVolHistoryLum && texVolHistoryAux && texVolCubeTr && texVolCubeLum &&
								   texVolCubeAux && texVolCubeHistoryTr && texVolCubeHistoryLum && texVolCubeHistoryAux &&
								   texVolCubeTraceTr && texVolCubeTraceLum && texVolCubeTraceAux &&
								   texShadowVolume && baseShapeNoiseSrv && cloudProfileLutSrv && cloudAdjustmentLutSrv && ndfReady && cirrusMapsReady);
	return baseShadersOk && volumetricShadersOk;
}

void PhysicalSky::Reset()
{
	UpdateCloudWind();
	const float2 lowAltitudeRange = settings.cloudLayer.low.GetNdfAltitudeRangeKm();
	const float lowCloudBaseKm = lowAltitudeRange.x;
	const float lowCloudTopKm = lowAltitudeRange.y;
	const float traceBottomKm = settings.cloudLayer.cirrus.enabled ? std::min(lowCloudBaseKm, settings.cloudLayer.cirrus.GetAltitudeKm()) : lowCloudBaseKm;
	const float traceTopKm = settings.cloudLayer.cirrus.enabled ? std::max(lowCloudTopKm, settings.cloudLayer.cirrus.GetAltitudeKm()) : lowCloudTopKm;
	const float lowCloudThicknessKm = lowCloudTopKm - lowCloudBaseKm;
	auto& skySync = globals::features::skySync;
	skySync.workingLightColors = std::nullopt;

	auto& linearLighting = globals::features::linearLighting;

	bool allGood = settings.enabled && ShadersOK() && skySync.loaded && skySync.settings.Enabled;

	// check worldspace
	bool worldspaceEnabled = false;
	bool inInterior = false;
	bool inMainLoadingMenu = globals::game::ui && (globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || globals::game::ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));

	std::map<std::string, WorldspaceInfo>::const_iterator worldspaceIt = settings.worldspaceWhitelist.end();
	float zBottom = settings.fallbackZBottom;
	auto* tes = globals::game::tes ? globals::game::tes : RE::TES::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	inInterior = cell && cell->IsInteriorCell();
	if (tes) {
		auto* worldspace = tes->GetRuntimeData2().worldSpace;
		if (!worldspace && cell)
			worldspace = cell->GetRuntimeData().worldSpace;

		if (worldspace) {
			std::string worldspaceName = worldspace->GetFormEditorID();
			worldspaceIt = settings.worldspaceWhitelist.find(worldspaceName);
			worldspaceEnabled = worldspaceIt != settings.worldspaceWhitelist.end();
			if (worldspaceEnabled)
				zBottom = worldspaceIt->second.zBottom;
		}
	}
	bool allowForcedInterior = inInterior && settings.forceEnableAllInteriorCells;
	allGood &= (worldspaceEnabled || settings.enableAllExteriorCells || allowForcedInterior) && (!inInterior || allowForcedInterior) && !inMainLoadingMenu;

	if (!allGood) {
		if (skySync.loaded && skySync.settings.Enabled)
			skySync.workingLightColors = std::nullopt;
		cbData.enabled = allGood;
		volMainHistoryValid = false;
		return;
	}

	// resolution
	float2 res{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = { floor(dynres.x), floor(dynres.y) };

	auto sunDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)];
	auto masserDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)];
	auto secundaDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)];

	// Wide gamut support: when ACEScg is active, transform color parameters
	// from sRGB gamut to AP1 gamut so that the LUTs are natively generated in AP1 space.
	// Spectral scattering/absorption coefficients use pre-computed AP1 band-averaged
	// values (from CIE 1931 spectral integration) rather than color-space matrix
	// transforms, since they represent wavelength-dependent physical quantities,
	// not tristimulus colors.
	bool wideGamut = linearLighting.IsACEScgActive();
	auto sRGBToWorkingGamut = [wideGamut](float3 v) -> float3 {
		if (!wideGamut)
			return v;
		static auto mat = getWhiteAdaptedRGBMatrix("sRGB", "ACEScg");
		return TransformColor(v, mat);
	};

	cbData = {
		.texDim = res,
		.rcpTexDim = float2(1.0f) / res,
		.frameDim = dynres,
		.rcpFrameDim = float2(1.0f) / dynres,
		.sunDir = { sunDir.x, sunDir.y, sunDir.z },
		.sunlightColor = sRGBToWorkingGamut(settings.sunlightColor),
		.trMix = settings.trMix,
		.masserDir = { masserDir.x, masserDir.y, masserDir.z },
		.apLumMix = settings.apLumMix,
		.masserColor = sRGBToWorkingGamut(settings.masserColor),
		.apTrMix = settings.apTrMix,
		.secundaDir = { secundaDir.x, secundaDir.y, secundaDir.z },
		.sunDiskCos = cos(settings.sunDiskRad) * (settings.proceduralSun ? 1.f : 0.f),
		.secundaColor = sRGBToWorkingGamut(settings.secundaColor),
		.enabled = allGood,
		.vanillaMix = settings.vanillaMix,
		.zBottom = zBottom,
		.rPlanet = settings.planetRadius / Util::Units::GAME_UNIT_TO_KM,
		.rAtmosphere = settings.atmosphereRadius / Util::Units::GAME_UNIT_TO_KM,
		.groundAlbedo = sRGBToWorkingGamut(settings.groundAlbedo),
		.cloudShadowRemapRange = settings.cloudShadowRemapRange,
		.aerosolFalloff = settings.aerosolFalloff * Util::Units::GAME_UNIT_TO_KM,
		// The coefficient members are what the scene layer blends: a weather or time-of-day
		// transition interpolates the optics a mixture resolved to, never the discrete mixture.
		.aerosolPhaseG = settings.aerosolPhaseG,
		.aerosolScatter = settings.aerosolScatter * 1e-3f * Util::Units::GAME_UNIT_TO_KM,
		.halfResApShadow = settings.halfResApShadow ? 1u : 0u,
		.aerosolAbsorption = settings.aerosolAbsorption * 1e-3f * Util::Units::GAME_UNIT_TO_KM,
		.rayleighFalloff = settings.rayleighFalloff * Util::Units::GAME_UNIT_TO_KM,
		.rayleighScatter = (wideGamut ? settings.rayleighScatterAP1 : settings.rayleighScatter) * 1e-3f * Util::Units::GAME_UNIT_TO_KM,
		.ozoneAltitude = settings.ozoneAltitude / Util::Units::GAME_UNIT_TO_KM,
		.ozoneThickness = settings.ozoneThickness / Util::Units::GAME_UNIT_TO_KM,
		.ozoneAbsorption = (wideGamut ? settings.ozoneAbsorptionAP1 : settings.ozoneAbsorption) * 1e-3f * Util::Units::GAME_UNIT_TO_KM,
		.enableVanillaClouds = settings.enableVanillaClouds ? 1u : 0u,
		.cloudRelightMix = settings.cloudRelightMix,
		.cloudOriginalMix = settings.cloudOriginalMix,
		.silverLiningMix = settings.silverLiningMix,
		.silverLiningSpread = settings.silverLiningSpread,
		.enableVolumetricClouds = settings.enableVolumetricClouds ? 1u : 0u,
		.shadowVolumeRange = settings.shadowVolumeRange / Util::Units::GAME_UNIT_TO_KM,
		.lowestCloudAltitude = traceBottomKm / Util::Units::GAME_UNIT_TO_KM,
		.highestCloudAltitude = traceTopKm / Util::Units::GAME_UNIT_TO_KM,
		.volCloudScatter = float3(std::max(settings.cloudLayer.lighting.sunExtinction, 0.f) * Util::Units::GAME_UNIT_TO_M),
		.volCloudUseSun = 0u,
		.volCloudAbsorption = float3(0.f),
		.volCloudLowBottom = lowCloudBaseKm / Util::Units::GAME_UNIT_TO_KM,
		.volCloudLowThickness = lowCloudThicknessKm / Util::Units::GAME_UNIT_TO_KM,
		.lightSkyStatics = settings.lightSkyStatics ? 1u : 0u,
		.skyStaticsBrightness = settings.skyStaticsBrightness,
	};

	if (settings.overrideDirLight) {
		const float pbrCompensationMult = linearLighting.IsLinearLightingActive() ? 1.0f : RE::NI_PI;  // Colors should match PBR values when not using linear lighting
		auto LightConvFn = [pbrCompensationMult](float3 color) {
			color /= pbrCompensationMult;
			return RE::NiColor(color.x, color.y, color.z);
		};
		skySync.workingLightColors = std::array{
			LightConvFn(cbData.sunlightColor),
			LightConvFn(cbData.masserColor),
			LightConvFn(cbData.secundaColor)
		};
	}

	RE::NiPoint3 posCam = { 0, 0, 0 };
	if (auto cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
		posCam = cam->cameraRoot->world.translate;
		cbData.zCameraPlanet = posCam.z - cbData.zBottom + cbData.rPlanet;
	}
	// Keep the astronomical sun as the cloud source until it is below the
	// horizon everywhere in the traced cloud region, not just at the camera.
	// The remaining night path retains the scene's lunar directional light.
	const float planetRadius = std::max(cbData.rPlanet, 1.0f);
	const float cloudRadius = std::max(planetRadius + cbData.highestCloudAltitude, planetRadius);
	const float horizonDip = std::acos(std::clamp(planetRadius / cloudRadius, 0.0f, 1.0f));
	// March range now starts at each cloud shell, so it cannot bound the visible
	// region's local-up tilt. Use the two planet-tangent horizon angles instead.
	const float observerRadius = std::max(cbData.zCameraPlanet, planetRadius);
	const float observerHorizonDip = std::acos(std::clamp(planetRadius / observerRadius, 0.0f, 1.0f));
	const float localUpRange = observerHorizonDip + horizonDip;
	const float solarLimit = std::min(horizonDip + localUpRange + 0.00465f, RE::NI_PI * 0.5f);
	cbData.volCloudUseSun = sunDir.z > -std::sin(solarLimit) ? 1u : 0u;
}

void PhysicalSky::EarlyPrepass()
{
	if (cbData.enabled) {
		GenerateLuts();
	}
}

void PhysicalSky::ReflectionsPrepass()
{
	if (cbData.enabled) {
		std::array srvs = { texTrLut->srv.get(), texSvLut->srv.get(), texApLut->srv.get() };
		globals::d3d::context->PSSetShaderResources(61, (uint)srvs.size(), srvs.data());
		if (texVolCubeTr && texVolCubeLum) {
			std::array<ID3D11ShaderResourceView*, 2> volCubeSrvs = { texVolCubeTr->srv.get(), texVolCubeLum->srv.get() };
			globals::d3d::context->PSSetShaderResources(114, (uint)volCubeSrvs.size(), volCubeSrvs.data());
		}
	}
}

void PhysicalSky::Prepass()
{
	if (settings.enabled && settings.enableVolumetricClouds) {
		const bool generatedNoise = settings.cloudNoise.procedural || !importedShapeNoiseSrv || cloudAdjustmentGenerated;
		if (generatedNoise)
			cloudNoiseGenerator.Update(settings.cloudNoise, importedAdjustmentLutSrv.get());
		auto* shape = generatedNoise && cloudNoiseGenerator.Shape() ? cloudNoiseGenerator.Shape() : importedShapeNoiseSrv.get();
		auto* adjustment = generatedNoise && cloudNoiseGenerator.Adjustment() ? cloudNoiseGenerator.Adjustment() : importedAdjustmentLutSrv.get();
		if (baseShapeNoiseSrv.get() != shape || cloudAdjustmentLutSrv.get() != adjustment) {
			baseShapeNoiseSrv.copy_from(shape);
			cloudAdjustmentLutSrv.copy_from(adjustment);
			volMainHistoryValid = false;
		}
	}
	if (cbData.enabled) {
		const bool renderVolumetricClouds = settings.enableVolumetricClouds && ShadersOK();

		if (renderVolumetricClouds) {
			const auto cloudSettingsKey = nlohmann::json{
				{ "map", settings.cloudMap },
				{ "low", settings.cloudLayer.low },
				{ "cirrus", settings.cloudLayer.cirrus },
				{ "lighting", settings.cloudLayer.lighting },
				{ "range", settings.rayMarchRange },
				{ "step", settings.marchStepScale },
				{ "planet", settings.planetRadius },
				{ "bottom", cbData.zBottom }
			}.dump();
			if (cloudSettingsKey != volCloudSettingsKey) {
				volMainHistoryValid = false;
				volCloudSettingsKey = cloudSettingsKey;
			}
			if (ndfManager.UpdateNdf(settings.cloudMap, ndfTexManager)) {
				volMainHistoryValid = false;
			}
			if (settings.cloudLayer.cirrus.enabled && cirrusMapManager.Update(settings.cloudLayer.cirrus, ndfTexManager, ndfManager))
				volMainHistoryValid = false;
			RenderVolumetricClouds(VolumetricCloudPass::kShadowVolume);
		}

		AccumShadow();

		// Volumetric clouds
		if (renderVolumetricClouds && !globals::deferred->MediumCompositeEnabled()) {
			RenderVolumetricClouds(VolumetricCloudPass::kMainViewAndCubemap);
		} else if (!renderVolumetricClouds && texVolTr && texVolLum) {
			// Clear to neutral when disabled (white transmittance, black luminance)
			auto context = globals::d3d::context;
			FLOAT trClr[4] = { 1.f, 1.f, 1.f, 1.f };
			FLOAT lumClr[4] = { 0.f, 0.f, 0.f, 0.f };
			context->ClearUnorderedAccessViewFloat(texVolTr->uav.get(), trClr);
			context->ClearUnorderedAccessViewFloat(texVolLum->uav.get(), lumClr);
			if (texVolAux)
				context->ClearUnorderedAccessViewFloat(texVolAux->uav.get(), lumClr);
			if (texVolLowTr)
				context->ClearUnorderedAccessViewFloat(texVolLowTr->uav.get(), trClr);
			if (texVolLowLum)
				context->ClearUnorderedAccessViewFloat(texVolLowLum->uav.get(), lumClr);
			if (texVolLowAux)
				context->ClearUnorderedAccessViewFloat(texVolLowAux->uav.get(), lumClr);
			if (texVolHistoryTr)
				context->ClearUnorderedAccessViewFloat(texVolHistoryTr->uav.get(), trClr);
			if (texVolHistoryLum)
				context->ClearUnorderedAccessViewFloat(texVolHistoryLum->uav.get(), lumClr);
			if (texVolHistoryAux)
				context->ClearUnorderedAccessViewFloat(texVolHistoryAux->uav.get(), lumClr);
			if (texVolCubeTr)
				context->ClearUnorderedAccessViewFloat(texVolCubeTr->uav.get(), trClr);
			if (texVolCubeLum)
				context->ClearUnorderedAccessViewFloat(texVolCubeLum->uav.get(), lumClr);
			if (texShadowVolume)
				context->ClearUnorderedAccessViewFloat(texShadowVolume->uav.get(), trClr);
			volMainHistoryValid = false;
			volHistoryFrameDim = {};
		}

		std::array srvs = { texTrLut->srv.get(), texSvLut->srv.get(), texApLut->srv.get(), texApShadow->srv.get() };
		globals::d3d::context->PSSetShaderResources(61, (uint)srvs.size(), srvs.data());

		// Bind volumetric cloud results and shadow volume for pixel shaders. Use t110-t112 to avoid feature texture conflicts.
		if (texVolTr && texVolLum) {
			std::array<ID3D11ShaderResourceView*, 3> volSrvs = { texVolTr->srv.get(), texVolLum->srv.get(), texShadowVolume ? texShadowVolume->srv.get() : nullptr };
			globals::d3d::context->PSSetShaderResources(110, (uint)volSrvs.size(), volSrvs.data());
		}
		if (texVolCubeTr && texVolCubeLum) {
			std::array<ID3D11ShaderResourceView*, 2> volCubeSrvs = { texVolCubeTr->srv.get(), texVolCubeLum->srv.get() };
			globals::d3d::context->PSSetShaderResources(114, (uint)volCubeSrvs.size(), volCubeSrvs.data());
		}
	}
}

void PhysicalSky::GenerateLuts()
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	constexpr auto debugStr = "Physical Sky: LUT Generation";
	state->BeginPerfEvent(debugStr);
	{
		TracyD3D11Zone(state->tracyCtx, debugStr);

		auto samplers = std::array{ sampTr.get(), sampSv.get(), sampNoise.get() };
		std::array<ID3D11ShaderResourceView*, 2> srvs = {};
		ID3D11UnorderedAccessView* uav = nullptr;

		/* ---- DISPATCH ---- */
		context->CSSetSamplers(0, (int)samplers.size(), samplers.data());

		// -> transmittance
		uav = texTrLut->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csTrLutGen.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::TransmittanceLut");
		context->Dispatch((kTrLutW + 7) >> 3, (kTrLutH + 7) >> 3, 1);
		globals::profiler->EndPass();

		// -> multiscatter
		uav = texMsLut->uav.get();
		srvs.at(0) = texTrLut->srv.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(csMsLutGen.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::MultiscatterLut");
		context->Dispatch((kMsLutW + 7) >> 3, (kMsLutH + 7) >> 3, 1);
		globals::profiler->EndPass();

		// -> sky-view
		uav = texSvLut->uav.get();
		srvs.at(1) = texMsLut->srv.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(csSvLutGen.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::SkyViewLut");
		context->Dispatch((kSvLutW + 7) >> 3, (kSvLutH + 7) >> 3, 1);
		globals::profiler->EndPass();

		// -> aerial perspective
		uav = texApLut->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csApLutGen.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::AerialPerspectiveLut");
		context->Dispatch((kApLutW + 7) >> 3, (kApLutH + 7) >> 3, 1);
		globals::profiler->EndPass();

		/* ---- RESTORE ---- */
		samplers.fill(nullptr);
		srvs.fill(nullptr);
		uav = nullptr;

		context->CSSetSamplers(0, (int)samplers.size(), samplers.data());
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(nullptr, nullptr, 0);
	}
	state->EndPerfEvent();
}

void PhysicalSky::AccumShadow()
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	if (state->isMapMenuOpen) {
		constexpr FLOAT clearShadow[4] = {};
		context->ClearUnorderedAccessViewFloat(texApShadow->uav.get(), clearShadow);
		return;
	}

	auto& volumetricShadows = globals::features::volumetricShadows;
	if (!volumetricShadows.loaded)
		return;
	auto& terrainShadows = globals::features::terrainShadows;
	auto& cloudShadows = globals::features::cloudShadows;

	uint resolution[2] = { (uint)cbData.frameDim.x, (uint)cbData.frameDim.y };
	if (settings.halfResApShadow) {
		resolution[0] = (resolution[0] + 1u) / 2u;
		resolution[1] = (resolution[1] + 1u) / 2u;
	}

	constexpr auto debugStr = "Physical Sky: Shadow Accumulation";
	state->BeginPerfEvent(debugStr);
	{
		TracyD3D11Zone(state->tracyCtx, debugStr);

		auto sampler = sampTr.get();
		ID3D11ShaderResourceView* directionalShadowLights = nullptr;
		if (auto* directionalShadowBuffer = Deferred::GetSingleton()->directionalShadowLights)
			directionalShadowLights = directionalShadowBuffer->srv.get();
		auto srvs = std::array{
			globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV,
			volumetricShadows.shadowView,
			static_cast<ID3D11ShaderResourceView*>(nullptr),
			terrainShadows.IsHeightMapReady() ? terrainShadows.texShadowHeight->srv.get() : nullptr,
			cloudShadows.loaded ? cloudShadows.texCloudShadowLayers[CloudShadows::kMaxCloudLayers - 1]->srv.get() : nullptr,
			settings.enableVolumetricClouds && texShadowVolume ? texShadowVolume->srv.get() : nullptr,
		};
		auto uav = texApShadow->uav.get();

		/* ---- DISPATCH ---- */
		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShaderResources(98, 1, &directionalShadowLights);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(settings.halfResApShadow ? csShadowAccumHalfRes.get() : csShadowAccum.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::AccumShadow");
		context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);
		globals::profiler->EndPass();

		/* ---- RESTORE ---- */
		sampler = nullptr;
		srvs.fill(nullptr);
		directionalShadowLights = nullptr;
		uav = nullptr;

		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShaderResources(98, 1, &directionalShadowLights);
		context->CSSetShader(nullptr, nullptr, 0);
	}
	state->EndPerfEvent();
}

void PhysicalSky::ModifySky()
{
	auto context = globals::d3d::context;
	context->PSGetSamplers(3, 2, originalPSSamplers);

	auto samplers = std::array{ sampTr.get(), sampSv.get() };
	context->PSSetSamplers(3, static_cast<UINT>(samplers.size()), samplers.data());
}

void PhysicalSky::RestoreSamplers()
{
	auto context = globals::d3d::context;
	context->PSSetSamplers(3, 2, originalPSSamplers);
}

void PhysicalSky::ModifyGrass()
{
	auto context = globals::d3d::context;
	context->PSGetSamplers(5, 1, originalPSGrassSampler.put());

	auto sampler = sampTileable.get();
	context->PSSetSamplers(5, 1, &sampler);
}

void PhysicalSky::RestoreGrassSampler()
{
	auto context = globals::d3d::context;
	auto sampler = originalPSGrassSampler.get();
	context->PSSetSamplers(5, 1, &sampler);
	originalPSGrassSampler = nullptr;
}

void PhysicalSky::ModifyWater()
{
	auto context = globals::d3d::context;
	context->PSGetSamplers(13, 1, originalPSWaterSampler.put());

	auto& samplerModifiedBits = globals::game::shadowState->GetRuntimeData().PSSamplerModifiedBits;
	originalPSWaterSamplerModifiedBits = samplerModifiedBits & (1u << 13);
	samplerModifiedBits &= ~(1u << 13);

	auto sampler = sampSv.get();
	context->PSSetSamplers(13, 1, &sampler);
}

void PhysicalSky::RestoreWaterSampler()
{
	auto context = globals::d3d::context;
	auto sampler = originalPSWaterSampler.get();
	context->PSSetSamplers(13, 1, &sampler);
	originalPSWaterSampler = nullptr;
	globals::game::shadowState->GetRuntimeData().PSSamplerModifiedBits |= originalPSWaterSamplerModifiedBits;
	originalPSWaterSamplerModifiedBits = 0;
}

void PhysicalSky::Hooks::BSSkyShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::physicalSky.ModifySky();
	func(This, Pass, RenderFlags);
}

void PhysicalSky::Hooks::BSSkyShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::physicalSky.RestoreSamplers();
	func(This, Pass, RenderFlags);
}

void PhysicalSky::Hooks::BSGrassShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	globals::features::physicalSky.ModifyGrass();
}

void PhysicalSky::Hooks::BSGrassShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	globals::features::physicalSky.RestoreGrassSampler();
}

void PhysicalSky::Hooks::BSWaterShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	globals::features::physicalSky.ModifyWater();
}

void PhysicalSky::Hooks::BSWaterShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	globals::features::physicalSky.RestoreWaterSampler();
}
