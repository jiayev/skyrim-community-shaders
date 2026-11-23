#include "Features/PhysicalSky.h"

#include "CSEditor/EditorWindow.h"
#include "CSEditor/SceneManager/SceneWidgetInterceptor.h"
#include "I18n/I18n.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.physical_sky."

namespace
{
	struct CloudSpeciesPreset
	{
		float coverage, cloudType, typeVariation;
		std::array<int, 2> baseCells;
		std::array<float, 3> coverageWeights;
		float secondaryCoverage, updraftOrganization, baseDepthScale;
		float disturbance = .2f;
		float cellular = 0.f;
		float convection = 0.f;
		float weatherSize = 24.f;
		float curvatureRadius = 1000.f;
		float coreFraction = .35f;
		float stormOutflow = .65f;
		std::array<int, 2> stormCells = { 4, 4 };
	};

	// Parameters of the eight reference renders; seeds and the current wind belong to the weather.
	constexpr CloudSpeciesPreset stratus{
		.coverage = .97f,
		.cloudType = 0.f,
		.typeVariation = .008f,
		.baseCells = { 3, 5 },
		.coverageWeights = { .79f, .17f, .04f },
		.secondaryCoverage = .07f,
		.updraftOrganization = 0.f,
		.baseDepthScale = .44f
	};
	constexpr CloudSpeciesPreset stratocumulus{
		.coverage = .86f,
		.cloudType = .12f,
		.typeVariation = .08f,
		.baseCells = { 20, 24 },
		.coverageWeights = { .42f, .50f, .08f },
		.secondaryCoverage = .10f,
		.updraftOrganization = .18f,
		.baseDepthScale = .43f,
		.disturbance = .28f,
		.cellular = .96f
	};
	constexpr CloudSpeciesPreset nimbostratus{
		.coverage = .96f,
		.cloudType = -1.f,
		.typeVariation = .12f,
		.baseCells = { 3, 4 },
		.coverageWeights = { .86f, .12f, .02f },
		.secondaryCoverage = .05f,
		.updraftOrganization = 0.f,
		.baseDepthScale = 1.35f
	};
	constexpr CloudSpeciesPreset fractus{
		.coverage = .32f,
		.cloudType = .095f,
		.typeVariation = .15f,
		.baseCells = { 13, 11 },
		.coverageWeights = { .12f, .27f, .61f },
		.secondaryCoverage = .18f,
		.updraftOrganization = .18f,
		.baseDepthScale = .36f,
		.disturbance = .53f
	};
	constexpr CloudSpeciesPreset humilis{
		.coverage = .42f,
		.cloudType = .18f,
		.typeVariation = .07f,
		.baseCells = { 16, 14 },
		.coverageWeights = { .48f, .39f, .13f },
		.secondaryCoverage = 0.f,
		.updraftOrganization = .62f,
		.baseDepthScale = .48f
	};
	constexpr CloudSpeciesPreset mediocris{
		.coverage = .49f,
		.cloudType = .35f,
		.typeVariation = .20f,
		.baseCells = { 9, 7 },
		.coverageWeights = { .52f, .34f, .14f },
		.secondaryCoverage = .09f,
		.updraftOrganization = .87f,
		.baseDepthScale = .86f
	};
	constexpr CloudSpeciesPreset congestus{
		.coverage = .43f,
		.cloudType = .56f,
		.typeVariation = .38f,
		.baseCells = { 10, 9 },
		.coverageWeights = { .62f, .31f, .07f },
		.secondaryCoverage = 0.f,
		.updraftOrganization = 1.f,
		.baseDepthScale = 1.3f,
		.weatherSize = 36.f,
		.curvatureRadius = 480.f,
		.coreFraction = .48f,
		.stormOutflow = .74f
	};
	constexpr CloudSpeciesPreset cumulonimbus{
		.coverage = .21f,
		.cloudType = .29f,
		.typeVariation = .50f,
		.baseCells = { 10, 9 },
		.coverageWeights = { .64f, .28f, .08f },
		.secondaryCoverage = .08f,
		.updraftOrganization = 1.f,
		.baseDepthScale = 1.f,
		.disturbance = .46f,
		.convection = .41f,
		.weatherSize = 42.f,
		.curvatureRadius = 480.f,
		.coreFraction = .48f,
		.stormOutflow = .74f,
		.stormCells = { 1, 1 }
	};

	SceneBlend PresetLayout(const CloudSpeciesPreset& preset, SceneBlend layout)
	{
		// Preserve each endpoint's seed, paint asset and blend weight, including inherited weather transitions.
		if (layout.states.empty() || std::ranges::none_of(layout.states, [](const json& state) { return state.at("weight") > 0.f; }))
			layout.states = json::array({ { { "weight", 1.f }, { "value", CloudWeatherLayout{} } } });
		const json shape{
			{ "baseCells", preset.baseCells }, { "stormCells", preset.stormCells }, { "baseEdge", .18f }, { "stormEdge", .24f },
			{ "coverageWeights", preset.coverageWeights }, { "secondaryCoverage", preset.secondaryCoverage },
			{ "cellular", preset.cellular }, { "updraftOrganization", preset.updraftOrganization },
			{ "coreFraction", preset.coreFraction }, { "stormOutflow", preset.stormOutflow }, { "paintedWeight", 0.f }
		};
		for (auto& state : layout.states)
			state.at("value").update(shape);
		return layout;
	}

	bool ApplyCloudSpecies(PhysicalSky::Settings& settings, const CloudSpeciesPreset& preset)
	{
		using Identity = SceneSettingsManager::SettingIdentity;
		static const Identity layoutId{ "PhysicalSky", { "cloudWeather" }, "layout" };
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto* armed = SceneWidgetInterceptor::GetArmedContext();
		const bool scene = armed && !armed->baseline;
		auto& weather = settings.cloudWeather;
		auto& low = settings.cloudLayer.low;
		SceneBlend layout;
		if (scene) {
			const auto current = manager->GetContextValueForEdit(armed->contextId, layoutId);
			if (!current || !SceneBlend::IsValid(*current))
				return false;
			layout = current->get<SceneBlend>();
		} else {
			const auto* base = manager->FindAppliedBaseline(layoutId);
			layout = base ? base->get<SceneBlend>() : weather.layout;
		}
		layout = PresetLayout(preset, std::move(layout));

		std::map<Identity, json> values;
		const auto set = [&](Identity id, float& member, float value) {
			if (scene) {
				values.emplace(std::move(id), value);
			} else {
				member = value;
				manager->RecordBaselineEdit(id, value);
			}
		};
		set({ "PhysicalSky", { "cloudWeather" }, "coverage" }, weather.coverage, preset.coverage);
		set({ "PhysicalSky", { "cloudWeather" }, "cloudType" }, weather.cloudType, preset.cloudType);
		set({ "PhysicalSky", { "cloudWeather" }, "typeVariation" }, weather.typeVariation, preset.typeVariation);
		set({ "PhysicalSky", { "cloudWeather" }, "convection" }, weather.convection, preset.convection);
		set({ "PhysicalSky", { "cloudLayer", "low" }, "baseDepthScale" }, low.baseDepthScale, preset.baseDepthScale);
		set({ "PhysicalSky", { "cloudLayer", "low" }, "curvatureRadius" }, low.curvatureRadius, preset.curvatureRadius);
		set({ "PhysicalSky", { "cloudLayer", "low", "weatherSize" }, "x" }, low.weatherSize.x, preset.weatherSize);
		set({ "PhysicalSky", { "cloudLayer", "low", "weatherSize" }, "y" }, low.weatherSize.y, preset.weatherSize);
		set({ "PhysicalSky", { "cloudLayer", "wind" }, "disturbance" }, settings.cloudLayer.wind.disturbance, preset.disturbance);
		if (scene) {
			values.emplace(layoutId, layout);
			return manager->ApplyContextPreset(armed->contextId, values);
		}
		weather.layout = std::move(layout);
		manager->RecordBaselineEdit(layoutId, weather.layout);
		return true;
	}
}

void PhysicalSky::DrawCloudSpeciesPresets()
{
	if (!ImGui::BeginCombo(T(TKEY("cloud_species_preset"), "Cloud species preset"),
			T(TKEY("cloud_species_choose"), "Choose a species to apply…"), ImGuiComboFlags_HeightLarge)) {
		Util::AddTooltip(T(TKEY("cloud_species_hint"), "Applies the species to the low cloud generator. Keeps the pattern seeds and sets painted map influence to zero. You can edit the parameters afterwards."));
		return;
	}
	const auto option = [&](const char* name, const CloudSpeciesPreset& preset) {
		if (ImGui::Selectable(name) && !ApplyCloudSpecies(settings, preset))
			EditorWindow::GetSingleton()->ShowNotification(T(TKEY("cloud_species_apply_failed"), "Could not apply the cloud species to this scene."));
	};
	option(T(TKEY("cloud_species_stratus"), "Stratus"), stratus);
	option(T(TKEY("cloud_species_stratocumulus"), "Stratocumulus"), stratocumulus);
	option(T(TKEY("cloud_species_nimbostratus"), "Nimbostratus"), nimbostratus);
	option(T(TKEY("cloud_species_fractus"), "Cumulus fractus"), fractus);
	option(T(TKEY("cloud_species_humilis"), "Cumulus humilis"), humilis);
	option(T(TKEY("cloud_species_mediocris"), "Cumulus mediocris"), mediocris);
	option(T(TKEY("cloud_species_congestus"), "Cumulus congestus"), congestus);
	option(T(TKEY("cloud_species_cumulonimbus"), "Cumulonimbus"), cumulonimbus);
	ImGui::EndCombo();
}
