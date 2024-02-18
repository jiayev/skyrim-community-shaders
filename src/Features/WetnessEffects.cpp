#include "WetnessEffects.h"

#include <Util.h>

const float MIN_START_PERCENTAGE = 0.05f;
const float DEFAULT_TRANSITION_PERCENTAGE = 1.0f;
const float TRANSITION_CURVE_MULTIPLIER = 2.0f;
const float TRANSITION_DENOMINATOR = 256.0f;
const float DRY_WETNESS = 0.0f;
const float RAIN_DELTA_PER_SECOND = 2.0f / 3600.0f;
const float SNOWY_DAY_DELTA_PER_SECOND = -0.489f / 3600.0f;  // Only doing evaporation until snow wetness feature is added
const float CLOUDY_DAY_DELTA_PER_SECOND = -0.735f / 3600.0f;
const float CLEAR_DAY_DELTA_PER_SECOND = -1.518f / 3600.0f;
const float WETNESS_SCALE = 2.0;  // Speed at which wetness builds up and drys.
const float PUDDLE_SCALE = 1.0;   // Speed at which puddles build up and dry
const float MAX_PUDDLE_DEPTH = 3.0f;
const float MAX_WETNESS_DEPTH = 2.0f;
const float MAX_PUDDLE_WETNESS = 1.0f;
const float MAX_WETNESS = 1.0f;
const float SECONDS_IN_A_DAY = 86400;
const float MAX_TIME_DELTA = SECONDS_IN_A_DAY - 30;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WetnessEffects::Settings,
	EnableWetnessEffects,
	MaxRainWetness,
	MaxPuddleWetness,
	MaxShoreWetness,
	ShoreRange,
	PuddleRadius,
	PuddleMaxAngle,
	PuddleMinWetness,
	MinRainWetness,
	SkinWetness,
	WeatherTransitionSpeed)

void WetnessEffects::DrawSettings()
{
	if (ImGui::TreeNodeEx("Wetness Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Wetness", (bool*)&settings.EnableWetnessEffects);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enables a wetness effect near water and when it is raining.");
		}

		ImGui::SliderFloat("Rain Wetness", &settings.MaxRainWetness, 0.0f, 1.0f);
		ImGui::SliderFloat("Puddle Wetness", &settings.MaxPuddleWetness, 0.0f, 4.0f);
		ImGui::SliderFloat("Shore Wetness", &settings.MaxShoreWetness, 0.0f, 1.0f);
		ImGui::TreePop();
	}

	ImGui::Spacing();
	ImGui::Spacing();

	if (ImGui::TreeNodeEx("Advanced", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Weather transition speed", &settings.WeatherTransitionSpeed, 0.5f, 5.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"How fast wetness appears when raining and how quickly it dries "
				"after rain has stopped. ");
		}

		ImGui::SliderFloat("Min Rain Wetness", &settings.MinRainWetness, 0.0f, 0.9f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"The minimum amount an object gets wet from rain. ");
		}

		ImGui::SliderFloat("Skin Wetness", &settings.SkinWetness, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"How wet character skin and hair get during rain. ");
		}

		ImGui::SliderInt("Shore Range", (int*)&settings.ShoreRange, 1, 64);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"The maximum distance from a body of water that Shore Wetness affects. ");
		}

		ImGui::SliderFloat("Puddle Radius", &settings.PuddleRadius, 0.3f, 3.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"The radius that is used to determine puddle size and location. ");
		}

		ImGui::SliderFloat("Puddle Max Angle", &settings.PuddleMaxAngle, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"How flat a surface needs to be for puddles to form on it. ");
		}

		ImGui::SliderFloat("Puddle Min Wetness", &settings.PuddleMinWetness, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"The wetness value at which puddles start to form. ");
		}

		ImGui::TreePop();
	}

	ImGui::Spacing();
	ImGui::Spacing();

	if (ImGui::TreeNodeEx("Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text(std::format("Wetness depth : {:.2f}", wetnessDepth / WETNESS_SCALE).c_str());
		ImGui::Text(std::format("Puddle depth : {:.2f}", puddleDepth / PUDDLE_SCALE).c_str());
		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::Text(std::format("Current weather : {0:X}", currentWeatherID).c_str());
		ImGui::Text(std::format("Previous weather : {0:X}", lastWeatherID).c_str());
		ImGui::TreePop();
	}
}

float WetnessEffects::CalculateWeatherTransitionPercentage(float skyCurrentWeatherPct, float beginFade, bool fadeIn)
{
	float weatherTransitionPercentage = DEFAULT_TRANSITION_PERCENTAGE;
	// Correct if beginFade is zero or negative
	beginFade = beginFade > 0 ? beginFade : beginFade + TRANSITION_DENOMINATOR;
	// Wait to start transition until precipitation begins/ends
	float startPercentage = (TRANSITION_DENOMINATOR - beginFade) * (1.0f / TRANSITION_DENOMINATOR);

	if (fadeIn) {
		float currentPercentage = (skyCurrentWeatherPct - startPercentage) / (1 - startPercentage);
		weatherTransitionPercentage = std::clamp(currentPercentage, 0.0f, 1.0f);
	} else {
		float currentPercentage = (startPercentage - skyCurrentWeatherPct) / (startPercentage);
		weatherTransitionPercentage = 1 - std::clamp(currentPercentage, 0.0f, 1.0f);
	}
	return weatherTransitionPercentage;
}

void WetnessEffects::CalculateWetness(RE::TESWeather* weather, RE::Sky* sky, float seconds, float& weatherWetnessDepth, float& weatherPuddleDepth)
{
	float wetnessDepthDelta = CLEAR_DAY_DELTA_PER_SECOND * WETNESS_SCALE * seconds;
	float puddleDepthDelta = CLEAR_DAY_DELTA_PER_SECOND * PUDDLE_SCALE * seconds;
	if (weather && sky) {
		// Figure out the weather type and set the wetness
		if (weather->precipitationData && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy)) {
			// Raining
			wetnessDepthDelta = RAIN_DELTA_PER_SECOND * WETNESS_SCALE * seconds;
			puddleDepthDelta = RAIN_DELTA_PER_SECOND * PUDDLE_SCALE * seconds;
		} else if (weather->precipitationData && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow)) {
			wetnessDepthDelta = SNOWY_DAY_DELTA_PER_SECOND * WETNESS_SCALE * seconds;
			puddleDepthDelta = SNOWY_DAY_DELTA_PER_SECOND * PUDDLE_SCALE * seconds;
		} else if (weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kCloudy)) {
			wetnessDepthDelta = CLOUDY_DAY_DELTA_PER_SECOND * WETNESS_SCALE * seconds;
			puddleDepthDelta = CLOUDY_DAY_DELTA_PER_SECOND * PUDDLE_SCALE * seconds;
		}
	}

	weatherWetnessDepth = wetnessDepthDelta > 0 ? std::min(weatherWetnessDepth + wetnessDepthDelta, MAX_WETNESS_DEPTH) : std::max(weatherWetnessDepth + wetnessDepthDelta, 0.0f);
	weatherPuddleDepth = puddleDepthDelta > 0 ? std::min(weatherPuddleDepth + puddleDepthDelta, MAX_PUDDLE_DEPTH) : std::max(weatherPuddleDepth + puddleDepthDelta, 0.0f);
}

void WetnessEffects::Draw(const RE::BSShader* shader, const uint32_t)
{
	if (shader->shaderType.any(RE::BSShader::Type::Lighting, RE::BSShader::Type::Grass)) {
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

		if (requiresUpdate) {
			requiresUpdate = false;

			PerPass data{};
			data.Wetness = DRY_WETNESS;
			data.PuddleWetness = DRY_WETNESS;
			currentWeatherID = 0;
			uint32_t previousLastWeatherID = lastWeatherID;
			lastWeatherID = 0;

			if (settings.EnableWetnessEffects) {
				if (auto sky = RE::Sky::GetSingleton()) {
					if (sky->mode.get() == RE::Sky::Mode::kFull) {
						if (auto currentWeather = sky->currentWeather) {
							currentWeatherID = currentWeather->GetFormID();
							if (auto calendar = RE::Calendar::GetSingleton()) {
								float currentWeatherWetnessDepth = wetnessDepth;
								float currentWeatherPuddleDepth = puddleDepth;
								float currentGameTime = calendar->GetCurrentGameTime() * SECONDS_IN_A_DAY;
								lastGameTimeValue = lastGameTimeValue == 0 ? currentGameTime : lastGameTimeValue;
								float seconds = currentGameTime - lastGameTimeValue;
								lastGameTimeValue = currentGameTime;

								if (abs(seconds) >= MAX_TIME_DELTA) {
									// If too much time has passed, snap wetness depths to the current weather.
									seconds = 0.0f;
									currentWeatherWetnessDepth = 0.0f;
									currentWeatherPuddleDepth = 0.0f;
									CalculateWetness(currentWeather, sky, 1.0f, currentWeatherWetnessDepth, currentWeatherPuddleDepth);
									wetnessDepth = currentWeatherWetnessDepth > 0 ? MAX_WETNESS_DEPTH : 0.0f;
									puddleDepth = currentWeatherPuddleDepth > 0 ? MAX_PUDDLE_DEPTH : 0.0f;
								}

								if (seconds > 0 || (seconds < 0 && (wetnessDepth > 0 || puddleDepth > 0))) {
									float weatherTransitionPercentage = DEFAULT_TRANSITION_PERCENTAGE;
									float lastWeatherWetnessDepth = wetnessDepth;
									float lastWeatherPuddleDepth = puddleDepth;
									seconds *= settings.WeatherTransitionSpeed;
									CalculateWetness(currentWeather, sky, seconds, currentWeatherWetnessDepth, currentWeatherPuddleDepth);
									// If there is a lastWeather, figure out what type it is and set the wetness
									if (auto lastWeather = sky->lastWeather) {
										lastWeatherID = lastWeather->GetFormID();
										CalculateWetness(lastWeather, sky, seconds, lastWeatherWetnessDepth, lastWeatherPuddleDepth);
										// If it was raining, wait to transition until precipitation ends, otherwise use the current weather's fade in
										if (lastWeather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy)) {
											weatherTransitionPercentage = CalculateWeatherTransitionPercentage(sky->currentWeatherPct, lastWeather->data.precipitationEndFadeOut, false);
										} else {
											weatherTransitionPercentage = CalculateWeatherTransitionPercentage(sky->currentWeatherPct, currentWeather->data.precipitationBeginFadeIn, true);
										}
									}

									// Transition between CurrentWeather and LastWeather depth values
									wetnessDepth = std::lerp(lastWeatherWetnessDepth, currentWeatherWetnessDepth, weatherTransitionPercentage);
									puddleDepth = std::lerp(lastWeatherPuddleDepth, currentWeatherPuddleDepth, weatherTransitionPercentage);
								} else {
									lastWeatherID = previousLastWeatherID;
								}

								// Calculate the wetness value from the water depth
								data.Wetness = std::min(wetnessDepth, MAX_WETNESS);
								data.PuddleWetness = std::min(puddleDepth, MAX_PUDDLE_WETNESS);
							}
						}
					}
				}
			}

			auto& state = RE::BSShaderManager::State::GetSingleton();
			RE::NiTransform& dalcTransform = state.directionalAmbientTransform;
			Util::StoreTransform3x4NoScale(data.DirectionalAmbientWS, dalcTransform);

			data.settings = settings;
			// Disable Shore Wetness if Wetness Effects are Disabled
			data.settings.MaxShoreWetness = settings.EnableWetnessEffects ? settings.MaxShoreWetness : 0.0f;

			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			size_t bytes = sizeof(PerPass);
			memcpy_s(mapped.pData, bytes, &data, bytes);
			context->Unmap(perPass->resource.get(), 0);
		}
		ID3D11ShaderResourceView* views[1]{};
		views[0] = perPass->srv.get();
		context->PSSetShaderResources(22, ARRAYSIZE(views), views);
	}
}

void WetnessEffects::SetupResources()
{
	D3D11_BUFFER_DESC sbDesc{};
	sbDesc.Usage = D3D11_USAGE_DYNAMIC;
	sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	sbDesc.StructureByteStride = sizeof(PerPass);
	sbDesc.ByteWidth = sizeof(PerPass);
	perPass = std::make_unique<Buffer>(sbDesc);

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = 1;
	perPass->CreateSRV(srvDesc);
}

void WetnessEffects::Reset()
{
	requiresUpdate = true;
}

void WetnessEffects::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void WetnessEffects::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void WetnessEffects::RestoreDefaultSettings()
{
	settings = {};
}
