#include "PhysicalSky.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <imgui_stdlib.h>

#include "CloudShadows.h"
#include "Deferred.h"
#include "I18n/I18n.h"
#include "LinearLighting.h"
#include "SkySync.h"
#include "TerrainShadows.h"
#include "VolumetricShadows.h"

#include "State.h"

#define I18N_KEY_PREFIX "feature.physical_sky."
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalSky::WorldspaceInfo,
	zBottom)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HpTextureOverrideSettings,
	lowWeatherPath,
	highWeatherPath,
	profilePath,
	scCellPath,
	highCellPath,
	highWarpPath,
	highWispPath)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HpGeneratedCloudMapSettings,
	generationVersion,
	weatherDim,
	profileWidth,
	profileHeight,
	worldSize,
	center,
	lowCoverage,
	lowContrast,
	stratocumulus,
	highCoverage,
	highContrast,
	overrides)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HpLowCloudSettings,
	noiseScale,
	noiseOffset,
	detailNoiseScale,
	windDirection,
	windSpeed,
	baseNoiseWindSpeed,
	detailNoiseWindSpeed,
	detailNoiseVerticalWindSpeed,
	billowyLow,
	billowyHigh,
	wispyLow,
	wispyHigh,
	detailStrengthCu,
	detailStrengthTcu,
	detailStrengthCb,
	densityThreshold,
	densityMultiplier,
	densityMultiplierCu,
	densityMultiplierTcu,
	densityMultiplierCb,
	bottomSmoothHeight,
	bottomSmoothPow,
	wispyEdgeWidth,
	wispyReach,
	wispyTopHeight,
	wispyTopHardness,
	coverageCoverIntensity,
	coverageCoverContrast,
	coverageHeightIntensity,
	coverageHeightContrast,
	coverTopStrength,
	coverTopMax,
	coverTopCurvePow)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HpStratocumulusSettings,
	cellScale,
	worleyStrength,
	heightScale,
	detailStrength,
	cellThickPow,
	cellThickStrength,
	cellNoiseStrength,
	coverageIntensity,
	coverageContrast)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HpHighCloudSettings,
	enabled,
	cellScale,
	cellWindSpeed,
	cellWarpScale,
	cellWarpStrength,
	cellThickStrength,
	asCellThickStrength,
	cellThickPow,
	bottom,
	top,
	bottomCoverageScale,
	heightCurvePow,
	densityThreshold,
	densitySoftness,
	softness,
	wispScale,
	wispStrength,
	horizonDistanceStart,
	horizonDistanceEnd,
	densityMultiplier,
	densitySoftAIntensity,
	densitySoftAContrast,
	densityModAIntensity,
	densityModAContrast,
	forwardEccentricity,
	backwardEccentricity,
	ambientTopMultiplier,
	ambientBottomMultiplier,
	skyBlendStrength,
	msAttenuation,
	msContribution,
	msEccentricity,
	lightAbsorption,
	viewAbsorption,
	coverAbsorptionStrength)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HpLightingSettings,
	scatterTint,
	forwardEccentricity,
	backwardEccentricity,
	ambientTopMultiplier,
	ambientBottomMultiplier,
	aoUpwardScale,
	msAttenuation,
	msContribution,
	msEccentricity,
	scatterSourceODScale,
	scatterSourceCurvePow,
	powderIntensity,
	lightSteps)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HpPhiFwdSettings,
	intensity,
	depthPow,
	depthBias,
	boundaryConfidence,
	msBuildScale,
	compress)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudLayer,
	lowestAltitude,
	highestAltitude,
	low,
	stratocumulus,
	high,
	lighting,
	phiFwd)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalSky::Settings,
	enabled,
	enableAllExteriorCells,
	forceEnableAllInteriorCells,
	overrideDirLight,
	lightSkyStatics,
	skyStaticsBrightness,
	halfResApShadow,
	tonemapper,
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
	aerosolFalloff,
	aerosolPhaseG,
	aerosolScatter,
	aerosolAbsorption,
	ozoneAltitude,
	ozoneThickness,
	ozoneAbsorption,
	fallbackZBottom,
	enableVanillaClouds,
	cloudRelightMix,
	cloudOriginalMix,
	silverLiningMix,
	silverLiningSpread,
	enableVolumetricClouds,
	rayMarchRange,
	shadowVolumeRange,
	cloudMaxStep,
	cloudMap,
	cloudLayer)

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
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void PhysicalSky::DataLoaded()
{
	if (!globals::features::skySync.loaded) {
		failedLoadedMessage = "Sky Sync is required for Physical Sky to function.";
		loaded = false;
	}
}

void PhysicalSky::RestoreDefaultSettings()
{
	settings = {};
}

void PhysicalSky::LoadSettings(json& o_json)
{
	settings = o_json;
}

void PhysicalSky::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PhysicalSky::DrawSettings()
{
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
		if (ImGui::BeginTabItem(T(TKEY("debug"), "Debug"))) {
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
			ImGui::TextColored({ 0, 1, 0, 1 }, "OK");
		else
			ImGui::TextColored({ 1, 0, 0, 1 }, "ERROR");

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

	ImGui::SeparatorText(T(TKEY("worldspace_whitelist"), "Worldspace Whitelist"));
	{
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
		const bool llEnabled = globals::features::linearLighting.settings.enableLinearLighting;
		ImGui::BeginDisabled(llEnabled);
		if (ImGui::BeginTable("tonemap", 4, ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
			ImGui::TableNextColumn();
			ImGui::Text("%s", T(TKEY("tonemapper"), "Tonemapper"));
			ImGui::TableNextColumn();
			ImGui::RadioButton(T(TKEY("linear"), "Linear"), &settings.tonemapper, 0);
			ImGui::TableNextColumn();
			ImGui::RadioButton(T(TKEY("gamma"), "Gamma"), &settings.tonemapper, 1);
			ImGui::TableNextColumn();
			ImGui::RadioButton(T(TKEY("reinherd"), "Reinherd"), &settings.tonemapper, 2);
			ImGui::EndTable();
		}
		ImGui::EndDisabled();
		if (llEnabled) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("tonemapper_is_forced_to_linear_when_linear_lighting"), "Tonemapper is forced to Linear when Linear Lighting is enabled."));
		}
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
		ImGui::Text("%s", T(TKEY("overrides_the_color_of_directional_light_linear_tonemapper"), "Overrides the color of directional light. Linear tonemapper and 1.0 transmittance mix are recommended."));
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

		ImGui::ColorEdit3(T(TKEY("scatter"), "Scatter"), &settings.rayleighScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::SliderFloat(T(TKEY("falloff"), "Falloff"), &settings.rayleighFalloff, 0.f, 2.f, "%.2f km^-1");
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("aerosol_mie"), "Aerosol (Mie)"));
	{
		ImGui::PushID("Mie");
		ImGui::TextWrapped("%s", T(TKEY("solid_and_liquid_particles_greater_than_1_10"),
									 "Solid and liquid particles greater than 1/10 of the light wavelength but not too much, like dust. Strongly anisotropic (Mie Scattering). "
									 "They contributes to the aureole around bright celestial bodies."));

		ImGui::SliderFloat(T(TKEY("anisotropy"), "Anisotropy"), &settings.aerosolPhaseG, -1, 1);
		ImGui::ColorEdit3(T(TKEY("scatter"), "Scatter"), &settings.aerosolScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit3(T(TKEY("absorption"), "Absorption"), &settings.aerosolAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("usually_1_9_of_scatter_coefficient_dust_pollution"), "Usually 1/9 of scatter coefficient. Dust/pollution is lower, fog is higher."));
		ImGui::SliderFloat(T(TKEY("falloff"), "Falloff"), &settings.aerosolFalloff, 0.f, 2.f, "%.2f km^-1");
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("ozone"), "Ozone"));
	{
		ImGui::PushID("Ozone");
		ImGui::TextWrapped("%s", T(TKEY("the_ozone_layer_high_up_in_the_sky"),
									 "The ozone layer high up in the sky that mainly absorbs light of certain wavelength. "
									 "It keeps the zenith sky blue, especially at sunrise or sunset."));

		ImGui::ColorEdit3(T(TKEY("absorption"), "Absorption"), &settings.ozoneAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
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
	auto& layer = settings.cloudLayer;
	auto& low = settings.cloudLayer.low;
	auto& sc = settings.cloudLayer.stratocumulus;
	auto& high = settings.cloudLayer.high;
	auto& lighting = settings.cloudLayer.lighting;
	auto& phi = settings.cloudLayer.phiFwd;

	ImGui::SeparatorText(T(TKEY("performance"), "Performance"));
	{
		ImGui::SliderFloat(T(TKEY("ray_march_range"), "Ray March Range"), &settings.rayMarchRange, 1.f, 64.f, "%.1f km");
		ImGui::SliderFloat(T(TKEY("shadow_volume_range"), "Shadow Cookie Range"), &settings.shadowVolumeRange, 1.f, 16.f, "%.1f km");
		uint32_t minStep = 1, maxStep = 200;
		ImGui::SliderScalar(T(TKEY("cloud_max_steps"), "Cloud Max Steps"), ImGuiDataType_U32, &settings.cloudMaxStep, &minStep, &maxStep);
		uint32_t minLightStep = 1, maxLightStep = 16;
		ImGui::SliderScalar(T(TKEY("light_steps"), "Light Steps"), ImGuiDataType_U32, &lighting.lightSteps, &minLightStep, &maxLightStep);
	}

	ImGui::SeparatorText(T(TKEY("placement"), "Placement"));
	{
		layer.lowestAltitude = std::clamp(layer.lowestAltitude, 0.0f, 12.0f);
		layer.highestAltitude = std::clamp(layer.highestAltitude, layer.lowestAltitude + 0.05f, 16.0f);
		ImGui::SliderFloat(T(TKEY("lowest_cloud_altitude"), "Lowest Cloud Altitude"), &layer.lowestAltitude, 0.f, layer.highestAltitude - 0.05f, "%.2f km");
		ImGui::SliderFloat(T(TKEY("highest_cloud_altitude"), "Highest Cloud Altitude"), &layer.highestAltitude, layer.lowestAltitude + 0.05f, 16.f, "%.2f km");
	}

	ImGui::SeparatorText(T(TKEY("composition"), "Low Clouds"));
	{
		bool noiseSettingsChanged = false;
		noiseSettingsChanged |= ImGui::SliderFloat3(T(TKEY("base_noise_frequency"), "Base Noise Frequency"), &low.noiseScale.x, 0.00001f, 0.001f, "%.6f 1/m", ImGuiSliderFlags_Logarithmic);
		noiseSettingsChanged |= ImGui::SliderFloat(T(TKEY("detail_noise_frequency"), "Detail Noise Frequency"), &low.detailNoiseScale, 0.00002f, 0.01f, "%.6f 1/m", ImGuiSliderFlags_Logarithmic);
		noiseSettingsChanged |= ImGui::SliderFloat(T(TKEY("billowy_low"), "Billowy Low"), &low.billowyLow, 0.f, 2.f, "%.2f");
		noiseSettingsChanged |= ImGui::SliderFloat(T(TKEY("billowy_high"), "Billowy High"), &low.billowyHigh, 0.f, 2.f, "%.2f");
		noiseSettingsChanged |= ImGui::SliderFloat(T(TKEY("wispy_low"), "Wispy Low"), &low.wispyLow, 0.f, 2.f, "%.2f");
		noiseSettingsChanged |= ImGui::SliderFloat(T(TKEY("wispy_high"), "Wispy High"), &low.wispyHigh, 0.f, 2.f, "%.2f");
		noiseSettingsChanged |= ImGui::SliderFloat(T(TKEY("cu_detail_strength"), "Cu Detail Strength"), &low.detailStrengthCu, 0.f, 2.f, "%.2f");
		noiseSettingsChanged |= ImGui::SliderFloat(T(TKEY("tcu_detail_strength"), "Tcu Detail Strength"), &low.detailStrengthTcu, 0.f, 2.f, "%.2f");
		noiseSettingsChanged |= ImGui::SliderFloat(T(TKEY("cb_detail_strength"), "Cb Detail Strength"), &low.detailStrengthCb, 0.f, 2.f, "%.2f");
		if (noiseSettingsChanged)
			volMainHistoryValid = false;
		ImGui::SliderFloat2(T(TKEY("wind_direction"), "Wind Direction"), &low.windDirection.x, -1.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("wind_speed"), "Wind Speed"), &low.windSpeed, 0.f, 80.f, "%.1f m/s");
		ImGui::SliderFloat(T(TKEY("density_threshold"), "Density Threshold"), &low.densityThreshold, 0.f, 0.5f, "%.3f");
		ImGui::SliderFloat(T(TKEY("density_multiplier"), "Density Multiplier"), &low.densityMultiplier, 0.f, 1.f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cu_density"), "Cu Density"), &low.densityMultiplierCu, 0.f, 4.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("tcu_density"), "Tcu Density"), &low.densityMultiplierTcu, 0.f, 4.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("cb_density"), "Cb Density"), &low.densityMultiplierCb, 0.f, 4.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("cover_top_strength"), "Cover Top Strength"), &low.coverTopStrength, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("cover_top_max"), "Cover Top Max"), &low.coverTopMax, 1.f, 4.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("bottom_smooth_height"), "Bottom Smooth Height"), &low.bottomSmoothHeight, 0.f, 0.5f, "%.3f");
	}

	ImGui::SeparatorText(T(TKEY("stratocumulus"), "Stratocumulus"));
	{
		ImGui::SliderFloat2(T(TKEY("sc_cell_scale"), "Sc Cell Scale"), &sc.cellScale.x, 0.1f, 32.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("sc_strength"), "Sc Strength"), &sc.worleyStrength, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("sc_height_scale"), "Sc Height Scale"), &sc.heightScale, 0.01f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("sc_detail_strength"), "Sc Detail Strength"), &sc.detailStrength, 0.f, 2.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("sc_cell_thickness"), "Sc Cell Thickness"), &sc.cellThickStrength, 0.f, 1.f, "%.2f");
	}

	ImGui::SeparatorText(T(TKEY("high_clouds"), "High Clouds"));
	{
		high.bottom = std::clamp(high.bottom, 0.0f, 1.0f);
		high.top = std::clamp(std::max(high.top, high.bottom), high.bottom, 1.0f);
		ImGui::Checkbox(T(TKEY("enable_high_clouds"), "Enable High Clouds"), &high.enabled);
		ImGui::SliderFloat(T(TKEY("high_bottom_layer_fraction"), "High Bottom (Layer Fraction)"), &high.bottom, 0.f, high.top, "%.2f");
		ImGui::SliderFloat(T(TKEY("high_top_layer_fraction"), "High Top (Layer Fraction)"), &high.top, high.bottom, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("high_density"), "High Density"), &high.densityMultiplier, 0.f, 2.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("high_softness"), "High Softness"), &high.softness, 0.001f, 0.25f, "%.3f");
		ImGui::SliderFloat2(T(TKEY("high_cell_scale"), "High Cell Scale"), &high.cellScale.x, 0.1f, 32.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("high_wisp_strength"), "High Wisp Strength"), &high.wispStrength, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("high_sky_blend"), "High Sky Blend"), &high.skyBlendStrength, 0.f, 1.f, "%.2f");
	}

	ImGui::SeparatorText(T(TKEY("lighting"), "Lighting"));
	{
		ImGui::ColorEdit3(T(TKEY("scatter_tint"), "Scatter Tint"), &lighting.scatterTint.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::SliderFloat(T(TKEY("forward_eccentricity"), "Forward Eccentricity"), &lighting.forwardEccentricity, 0.f, 0.95f, "%.2f");
		ImGui::SliderFloat(T(TKEY("backward_eccentricity"), "Backward Eccentricity"), &lighting.backwardEccentricity, 0.f, 0.8f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ambient_top"), "Ambient Top"), &lighting.ambientTopMultiplier, 0.f, 5.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ambient_bottom"), "Ambient Bottom"), &lighting.ambientBottomMultiplier, 0.f, 5.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ms_attenuation"), "MS Attenuation"), &lighting.msAttenuation, 0.01f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ms_contribution"), "MS Contribution"), &lighting.msContribution, 0.01f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ms_eccentricity"), "MS Eccentricity"), &lighting.msEccentricity, 0.01f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("upward_ao"), "Upward AO"), &lighting.aoUpwardScale, 0.f, 4.f, "%.2f");
	}

	ImGui::SeparatorText(T(TKEY("phi_fwd"), "PhiFwd"));
	{
		ImGui::SliderFloat(T(TKEY("phi_fwd_intensity"), "PhiFwd Intensity"), &phi.intensity, 0.f, 4.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("phi_fwd_depth_pow"), "PhiFwd Depth Pow"), &phi.depthPow, 0.f, 4.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("phi_fwd_depth_bias"), "PhiFwd Depth Bias"), &phi.depthBias, -1.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("phi_fwd_boundary"), "PhiFwd Boundary"), &phi.boundaryConfidence, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("phi_fwd_ms_build"), "PhiFwd MS Build"), &phi.msBuildScale, 0.f, 8.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("phi_fwd_compress"), "PhiFwd Compress"), &phi.compress, 0.f, 4.f, "%.2f");
	}

	ImGui::SeparatorText(T(TKEY("cloud_map"), "Cloud Map"));
	{
		ndfManager.DrawNdfSettings(settings.cloudMap, ndfTexManager);
		if (ImGui::Button(T(TKEY("reload_cloud_textures"), "Reload Cloud Textures"), { -FLT_MIN, 0 }))
			LoadCloudTextures();
		if (baseShapeNoiseSrv && detailErosionNoiseSrv)
			ImGui::TextColored({ 0, 1, 0, 1 }, "%s", T(TKEY("cloud_textures_loaded"), "Cloud Textures: Loaded"));
		else
			ImGui::TextColored({ 1, 0, 0, 1 }, "%s", T(TKEY("cloud_textures_missing"), "Cloud Textures: Missing"));
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

		BUFFER_VIEWER_NODE_BULLET(texTrLut, 1.f);
		BUFFER_VIEWER_NODE_BULLET(texMsLut, 1.f);
		BUFFER_VIEWER_NODE_BULLET(texSvLut, 1.f);
		BUFFER_VIEWER_NODE_BULLET(texApShadow, debugScale);
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
		// texDesc.Width /= 2;
		// texDesc.Height /= 2;

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
		auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.data())))
			info.csPtr->attach(rawPtr);
	}
}

bool PhysicalSky::ShadersOK()
{
	bool baseShadersOk = csTrLutGen && csMsLutGen && csSvLutGen && csApLutGen && csShadowAccum && csShadowAccumHalfRes &&
	                     texTrLut && texSvLut && texApLut && texApShadow;
	bool volumetricShadersOk = !settings.enableVolumetricClouds ||
	                           (csVolMainView && csVolReproject && csVolUpscale && csVolShadowVolume && csVolShadowFilter && csVolCubemap && csVolAmbientSH && texVolCloudAmbientSH &&
								   texVolTr && texVolLum && texVolAux && texVolLowTr && texVolLowLum && texVolLowAux && texVolUpscaleTr && texVolUpscaleLum && texVolUpscaleAux &&
								   texVolHistoryTr && texVolHistoryLum && texVolHistoryAux && texVolCubeTr && texVolCubeLum &&
								   texShadowVolume && texShadowVolumeTemp && baseShapeNoiseSrv && detailErosionNoiseSrv && ndfManager.texLowWeather && ndfManager.texHighWeather && ndfManager.texProfile);
	return baseShadersOk && volumetricShadersOk;
}

void PhysicalSky::Reset()
{
	auto& skySync = globals::features::skySync;
	skySync.lightColors = std::nullopt;

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
		cbData.enabled = allGood;
		linearLighting.isDirLightLinear = false;
		return;
	}

	// resolution
	float2 res{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = { floor(dynres.x), floor(dynres.y) };

	auto sunDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)];
	auto masserDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)];
	auto secundaDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)];

	cbData = {
		.texDim = res,
		.rcpTexDim = float2(1.0f) / res,
		.frameDim = dynres,
		.rcpFrameDim = float2(1.0f) / dynres,
		.sunDir = { sunDir.x, sunDir.y, sunDir.z },
		.sunlightColor = settings.sunlightColor,
		.trMix = settings.trMix,
		.masserDir = { masserDir.x, masserDir.y, masserDir.z },
		.apLumMix = settings.apLumMix,
		.masserColor = settings.masserColor,
		.apTrMix = settings.apTrMix,
		.secundaDir = { secundaDir.x, secundaDir.y, secundaDir.z },
		.sunDiskCos = cos(settings.sunDiskRad) * (settings.proceduralSun ? 1.f : 0.f),
		.secundaColor = settings.secundaColor,
		.enabled = allGood,
		.tonemapper = linearLighting.settings.enableLinearLighting ? 0 : settings.tonemapper,
		.vanillaMix = settings.vanillaMix,
		.zBottom = zBottom,
		.rPlanet = settings.planetRadius / Util::Units::GAME_UNIT_TO_KM,
		.rAtmosphere = settings.atmosphereRadius / Util::Units::GAME_UNIT_TO_KM,
		.groundAlbedo = settings.groundAlbedo,
		.cloudShadowRemapRange = settings.cloudShadowRemapRange,
		.aerosolFalloff = settings.aerosolFalloff * Util::Units::GAME_UNIT_TO_KM,
		.aerosolPhaseG = settings.aerosolPhaseG,
		.aerosolScatter = settings.aerosolScatter * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.halfResApShadow = settings.halfResApShadow ? 1u : 0u,
		.aerosolAbsorption = settings.aerosolAbsorption * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.rayleighFalloff = settings.rayleighFalloff * Util::Units::GAME_UNIT_TO_KM,
		.rayleighScatter = settings.rayleighScatter * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.ozoneAltitude = settings.ozoneAltitude / Util::Units::GAME_UNIT_TO_KM,
		.ozoneThickness = settings.ozoneThickness / Util::Units::GAME_UNIT_TO_KM,
		.ozoneAbsorption = settings.ozoneAbsorption * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.enableVanillaClouds = settings.enableVanillaClouds ? 1u : 0u,
		.cloudRelightMix = settings.cloudRelightMix,
		.cloudOriginalMix = settings.cloudOriginalMix,
		.silverLiningMix = settings.silverLiningMix,
		.silverLiningSpread = settings.silverLiningSpread,
		.enableVolumetricClouds = settings.enableVolumetricClouds ? 1u : 0u,
		.shadowVolumeRange = settings.shadowVolumeRange / Util::Units::GAME_UNIT_TO_KM,
		.lowestCloudAltitude = settings.cloudLayer.lowestAltitude / Util::Units::GAME_UNIT_TO_KM,
		.highestCloudAltitude = settings.cloudLayer.highestAltitude / Util::Units::GAME_UNIT_TO_KM,
		.volCloudScatter = settings.cloudLayer.lighting.scatterTint * settings.cloudLayer.low.densityMultiplier * Util::Units::GAME_UNIT_TO_KM,
		.volCloudAverageDensity = settings.cloudLayer.low.densityMultiplier,
		.volCloudAbsorption = (float3(1.f) - settings.cloudLayer.lighting.scatterTint * 0.25f) * settings.cloudLayer.low.densityMultiplier * Util::Units::GAME_UNIT_TO_KM,
		.lightSkyStatics = settings.lightSkyStatics ? 1u : 0u,
		.skyStaticsBrightness = settings.skyStaticsBrightness,
		.pad0 = { 0u, 0u },
	};

	if (settings.overrideDirLight) {
		linearLighting.isDirLightLinear = true;
		const float pbrCompensationMult = linearLighting.settings.enableLinearLighting ? 1.0f : RE::NI_PI;  // Colors should match PBR values when not using linear lighting
		auto LightConvFn = [pbrCompensationMult](float3 color) {
			color /= pbrCompensationMult;
			return RE::NiColor(color.x, color.y, color.z);
		};
		skySync.lightColors = { LightConvFn(cbData.sunlightColor), LightConvFn(cbData.masserColor), LightConvFn(cbData.secundaColor) };
	} else {
		linearLighting.isDirLightLinear = false;
	}

	RE::NiPoint3 posCam = { 0, 0, 0 };
	if (auto cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
		posCam = cam->cameraRoot->world.translate;
		cbData.zCameraPlanet = posCam.z - cbData.zBottom + cbData.rPlanet;
	}
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
		ID3D11ShaderResourceView* msSrv = texMsLut ? texMsLut->srv.get() : nullptr;
		globals::d3d::context->PSSetShaderResources(113, 1, &msSrv);
		if (texVolCubeTr && texVolCubeLum) {
			std::array<ID3D11ShaderResourceView*, 2> volCubeSrvs = { texVolCubeTr->srv.get(), texVolCubeLum->srv.get() };
			globals::d3d::context->PSSetShaderResources(114, (uint)volCubeSrvs.size(), volCubeSrvs.data());
		}
	}
}

void PhysicalSky::Prepass()
{
	if (cbData.enabled) {
		const bool renderVolumetricClouds = settings.enableVolumetricClouds && csVolMainView && csVolReproject && csVolUpscale && csVolShadowVolume && csVolShadowFilter && csVolCubemap && csVolAmbientSH && texVolCloudAmbientSH;

		if (renderVolumetricClouds) {
			ndfManager.UpdateNdf(settings.cloudMap);
			RenderVolumetricClouds(VolumetricCloudPass::kShadowVolume);
		}

		AccumShadow();

		// Volumetric clouds
		if (renderVolumetricClouds) {
			RenderVolumetricClouds(VolumetricCloudPass::kMainViewAndCubemap);
		} else if (texVolTr && texVolLum) {
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
			if (texVolUpscaleTr)
				context->ClearUnorderedAccessViewFloat(texVolUpscaleTr->uav.get(), trClr);
			if (texVolUpscaleLum)
				context->ClearUnorderedAccessViewFloat(texVolUpscaleLum->uav.get(), lumClr);
			if (texVolUpscaleAux)
				context->ClearUnorderedAccessViewFloat(texVolUpscaleAux->uav.get(), lumClr);
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
				context->ClearUnorderedAccessViewFloat(texShadowVolume->uav.get(), lumClr);
			if (texShadowVolumeTemp)
				context->ClearUnorderedAccessViewFloat(texShadowVolumeTemp->uav.get(), lumClr);
			volMainHistoryValid = false;
			volHistoryWidth = 0;
			volHistoryHeight = 0;
		}

		std::array srvs = { texTrLut->srv.get(), texSvLut->srv.get(), texApLut->srv.get(), texApShadow->srv.get() };
		globals::d3d::context->PSSetShaderResources(61, (uint)srvs.size(), srvs.data());
		ID3D11ShaderResourceView* msSrv = texMsLut ? texMsLut->srv.get() : nullptr;
		globals::d3d::context->PSSetShaderResources(113, 1, &msSrv);

		// Bind volumetric cloud results and shadow cookie for pixel shaders. Use t110-t112 to avoid feature texture conflicts.
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

	auto& volumetricShadows = globals::features::volumetricShadows;
	if (!volumetricShadows.loaded)
		return;
	auto& terrainShadows = globals::features::terrainShadows;
	auto& cloudShadows = globals::features::cloudShadows;

	float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	float2 size = Util::ConvertToDynamic(screenSize);
	uint resolution[2] = { (uint)size.x, (uint)size.y };
	if (settings.halfResApShadow) {
		resolution[0] = std::max(1u, resolution[0] / 2u);
		resolution[1] = std::max(1u, resolution[1] / 2u);
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
