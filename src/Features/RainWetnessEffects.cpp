#include "RainWetnessEffects.h"

const float MIN_START_PERCENTAGE = 0.05f;
const float DEFAULT_TRANSITION_PERCENTAGE = 1.0f;
const float DRY_SHININESS_MULTIPLIER = 1.0f;
const float DRY_SPECULAR_MULTIPLIER = 1.0f;
const float DRY_DIFFUSE_MULTIPLIER = 1.0f;
const float TRANSITION_CURVE_MULTIPLIER = 3.0f;
const float TRANSITION_DENOMINATOR = 256.0f;
const float DAY = 0.0f;
const float NIGHT = 1.0f;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	RainWetnessEffects::Settings,
	EnableRainWetnessEffects,
	RainShininessMultiplierDay,
	RainSpecularMultiplierDay,
	RainDiffuseMultiplierDay,
	RainShininessMultiplierNight,
	RainSpecularMultiplierNight,
	RainDiffuseMultiplierNight)

void RainWetnessEffects::DrawSettings()
{
	if (ImGui::TreeNodeEx("Rain Wetness Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Rain Wetness", (bool*)&settings.EnableRainWetnessEffects);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Enables a wetness effect when it is raining.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Daytime", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Rain Specular Multiplier", &settings.RainSpecularMultiplierDay, 0, 50);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Controls the strength of the wetness effect.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::SliderFloat("Rain Shininess Multiplier", &settings.RainShininessMultiplierDay, 0.5, 4);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Controls the appearance of shininess by focusing the reflection.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::SliderFloat("Rain Diffuse Multiplier", &settings.RainDiffuseMultiplierDay, 0, 2);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Darkens the diffuse color to simulate the darkening of wet surfaces.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Nightime", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Rain Specular Multiplier", &settings.RainSpecularMultiplierNight, 0, 50);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Controls the strength of the wetness effect.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::SliderFloat("Rain Shininess Multiplier", &settings.RainShininessMultiplierNight, 0.5, 4);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Controls the appearance of shininess by focusing the reflection.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::SliderFloat("Rain Diffuse Multiplier", &settings.RainDiffuseMultiplierNight, 0, 2);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Darkens the diffuse color to simulate the darkening of wet surfaces.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::TreePop();
	}
}

void RainWetnessEffects::UpdateCubemap()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto context = renderer->GetRuntimeData().context;
	auto cubemap = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];

	for (UINT face = 0; face < 6; face++) {
		D3D11_BOX srcBox = { 0 };
		srcBox.left = 0;
		srcBox.right = (blurredReflectionsTexture->desc.Width >> 0);
		srcBox.top = 0;
		srcBox.bottom = (blurredReflectionsTexture->desc.Height >> 0);
		srcBox.front = 0;
		srcBox.back = 1;

		// Calculate the subresource index for the current face and mip level
		UINT srcSubresourceIndex = D3D11CalcSubresource(0, face, 1);

		// Copy the subresource from the source to the destination texture
		context->CopySubresourceRegion(blurredReflectionsTexture->resource.get(), D3D11CalcSubresource(0, face, 4), 0, 0, 0, cubemap.texture, srcSubresourceIndex, &srcBox);
	}

	context->GenerateMips(blurredReflectionsTexture->srv.get());
}

void RainWetnessEffects::Draw(const RE::BSShader* shader, const uint32_t)
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

	if (!blurredReflectionsTexture) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto cubemap = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];

		D3D11_TEXTURE2D_DESC texDesc{};
		cubemap.texture->GetDesc(&texDesc);
		texDesc.MipLevels = 4;
		texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
		blurredReflectionsTexture = new Texture2D(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		cubemap.SRV->GetDesc(&srvDesc);
		srvDesc.TextureCube.MipLevels = 4;
		blurredReflectionsTexture->CreateSRV(srvDesc);
	}

	if (shader->shaderType.any(RE::BSShader::Type::Lighting)) {
		// During world cubemap generation we cannot use the cubemap
		auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
		if (shadowState->GetRuntimeData().cubeMapRenderTarget == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS) {
			ID3D11ShaderResourceView* views[1]{};
			views[0] = nullptr;
			context->PSSetShaderResources(64, 1, views);
		} else if (!renderedScreenCamera) {
			UpdateCubemap();
			renderedScreenCamera = true;
			ID3D11ShaderResourceView* views[1]{};
			views[0] = blurredReflectionsTexture->srv.get();
			context->PSSetShaderResources(64, 1, views);
		}

		PerPass data{};
		data.settings = settings;

		data.RainShininessMultiplier = DRY_SHININESS_MULTIPLIER;
		data.RainSpecularMultiplier = DRY_SPECULAR_MULTIPLIER;
		data.RainDiffuseMultiplier = DRY_DIFFUSE_MULTIPLIER;

		float WeatherTransitionPercentage = DEFAULT_TRANSITION_PERCENTAGE;
		float DayNightTransition = DEFAULT_TRANSITION_PERCENTAGE;
		float ShininessMultiplierCurrentDay = DRY_SHININESS_MULTIPLIER;
		float ShininessMultiplierPreviousDay = DRY_SHININESS_MULTIPLIER;
		float SpecularMultiplierCurrentDay = DRY_SPECULAR_MULTIPLIER;
		float SpecularMultiplierPreviousDay = DRY_SPECULAR_MULTIPLIER;
		float DiffuseMultiplierCurrentDay = DRY_DIFFUSE_MULTIPLIER;
		float DiffuseMultiplierPreviousDay = DRY_DIFFUSE_MULTIPLIER;
		float ShininessMultiplierCurrentNight = DRY_SHININESS_MULTIPLIER;
		float ShininessMultiplierPreviousNight = DRY_SHININESS_MULTIPLIER;
		float SpecularMultiplierCurrentNight = DRY_SPECULAR_MULTIPLIER;
		float SpecularMultiplierPreviousNight = DRY_SPECULAR_MULTIPLIER;
		float DiffuseMultiplierCurrentNight = DRY_DIFFUSE_MULTIPLIER;
		float DiffuseMultiplierPreviousNight = DRY_DIFFUSE_MULTIPLIER;

		if (settings.EnableRainWetnessEffects) {
			if (auto player = RE::PlayerCharacter::GetSingleton()) {
				if (auto cell = player->GetParentCell()) {
					if (!cell->IsInteriorCell()) {
						if (auto sky = RE::Sky::GetSingleton()) {
							if (auto currentWeather = sky->currentWeather) {
								// Set the Day Night Transition with Day = 0, and Night = 1
								if (auto calendar = RE::Calendar::GetSingleton()) {
									if (auto currentClimate = sky->currentClimate) {
										struct tm sunriseBeginTM = currentClimate->timing.sunrise.GetBeginTime();
										struct tm sunriseEndTM = currentClimate->timing.sunrise.GetEndTime();
										struct tm sunsetBeginTM = currentClimate->timing.sunset.GetBeginTime();
										struct tm sunsetEndTM = currentClimate->timing.sunset.GetEndTime();
										float gameHour = calendar->GetHour();
										// Skyrim doesn't seem to use seconds, but doing this in seconds just in case
										int sunriseBeginTime = (sunriseBeginTM.tm_hour * 3600) + (sunriseBeginTM.tm_min * 60) + sunriseBeginTM.tm_sec;
										int sunriseEndTime = (sunriseEndTM.tm_hour * 3600) + (sunriseEndTM.tm_min * 60) + sunriseEndTM.tm_sec;
										int sunsetBeginTime = (sunsetBeginTM.tm_hour * 3600) + (sunsetBeginTM.tm_min * 60) + sunsetBeginTM.tm_sec;
										int sunsetEndTime = (sunsetEndTM.tm_hour * 3600) + (sunsetEndTM.tm_min * 60) + sunsetEndTM.tm_sec;
										int time = static_cast<int>(gameHour * 3600);

										if (sunsetEndTime > sunsetBeginTime && sunsetBeginTime >= sunriseEndTime && sunriseEndTime > sunriseBeginTime && sunriseBeginTime >= 0) {
											if ((time - sunriseBeginTime) < 0 || (time - sunsetEndTime) > 0) {
												// Night
												DayNightTransition = NIGHT;
											} else if ((time - sunriseEndTime) <= 0) {
												// During sunrise, night 1 -> day 0
												DayNightTransition = 1.0f - (static_cast<float>(time - sunriseBeginTime) / static_cast<float>(sunriseEndTime - sunriseBeginTime));
											} else if ((time - sunsetBeginTime) < 0) {
												// During day
												DayNightTransition = DAY;
											} else if ((time - sunsetEndTime) <= 0) {
												// During sunset, day 0 -> night 1
												DayNightTransition = static_cast<float>(time - sunsetBeginTime) / static_cast<float>(sunsetEndTime - sunsetBeginTime);
											} else {
												// This should never happen
												logger::info("This shouldn't happen");
											}
										}
									}
								}

								if (currentWeather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy)) {
									// Currently raining
									ShininessMultiplierCurrentDay = settings.RainShininessMultiplierDay;
									SpecularMultiplierCurrentDay = settings.RainSpecularMultiplierDay;
									DiffuseMultiplierCurrentDay = settings.RainDiffuseMultiplierDay;
									ShininessMultiplierCurrentNight = settings.RainShininessMultiplierNight;
									SpecularMultiplierCurrentNight = settings.RainSpecularMultiplierNight;
									DiffuseMultiplierCurrentNight = settings.RainDiffuseMultiplierNight;

									// Fade in gradually after precipitation has started
									float beginFade = currentWeather->data.precipitationBeginFadeIn;
									beginFade = beginFade > 0 ? beginFade : beginFade + TRANSITION_DENOMINATOR;
									float startPercentage = (TRANSITION_DENOMINATOR - beginFade) * (1.0f / TRANSITION_DENOMINATOR);
									startPercentage = startPercentage > MIN_START_PERCENTAGE ? startPercentage : MIN_START_PERCENTAGE;
									float currentPercentage = (sky->currentWeatherPct - startPercentage) / (1 - startPercentage);
									WeatherTransitionPercentage = std::clamp(currentPercentage, 0.0f, 1.0f);
								} else {
									// Not currently raining
									ShininessMultiplierCurrentDay = DRY_SHININESS_MULTIPLIER;
									SpecularMultiplierCurrentDay = DRY_SPECULAR_MULTIPLIER;
									DiffuseMultiplierCurrentDay = DRY_DIFFUSE_MULTIPLIER;
									ShininessMultiplierCurrentNight = DRY_SHININESS_MULTIPLIER;
									SpecularMultiplierCurrentNight = DRY_SPECULAR_MULTIPLIER;
									DiffuseMultiplierCurrentNight = DRY_DIFFUSE_MULTIPLIER;
								}

								if (auto lastWeather = sky->lastWeather) {
									if (lastWeather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy)) {
										// Was raining before
										ShininessMultiplierPreviousDay = settings.RainShininessMultiplierDay;
										SpecularMultiplierPreviousDay = settings.RainSpecularMultiplierDay;
										DiffuseMultiplierPreviousDay = settings.RainDiffuseMultiplierDay;
										ShininessMultiplierPreviousNight = settings.RainShininessMultiplierNight;
										SpecularMultiplierPreviousNight = settings.RainSpecularMultiplierNight;
										DiffuseMultiplierPreviousNight = settings.RainDiffuseMultiplierNight;

										// Fade out gradually
										WeatherTransitionPercentage = sky->currentWeatherPct;

									} else {
										// Wasn't raining before
										ShininessMultiplierPreviousDay = DRY_SHININESS_MULTIPLIER;
										SpecularMultiplierPreviousDay = DRY_SPECULAR_MULTIPLIER;
										DiffuseMultiplierPreviousDay = DRY_DIFFUSE_MULTIPLIER;
										ShininessMultiplierPreviousNight = DRY_SHININESS_MULTIPLIER;
										SpecularMultiplierPreviousNight = DRY_SPECULAR_MULTIPLIER;
										DiffuseMultiplierPreviousNight = DRY_DIFFUSE_MULTIPLIER;
									}
								} else {
									// No last weather, 100% transition
									WeatherTransitionPercentage = DEFAULT_TRANSITION_PERCENTAGE;
									ShininessMultiplierPreviousDay = ShininessMultiplierCurrentDay;
									SpecularMultiplierPreviousDay = SpecularMultiplierCurrentDay;
									DiffuseMultiplierPreviousDay = DiffuseMultiplierCurrentDay;
									ShininessMultiplierPreviousNight = ShininessMultiplierCurrentNight;
									SpecularMultiplierPreviousNight = SpecularMultiplierCurrentNight;
									DiffuseMultiplierPreviousNight = DiffuseMultiplierCurrentNight;
								}

								// Adjust the transition curve to ease in to the transition
								WeatherTransitionPercentage = (exp2(TRANSITION_CURVE_MULTIPLIER * log2(WeatherTransitionPercentage)));

								data.RainShininessMultiplier = std::lerp(std::lerp(ShininessMultiplierPreviousDay, ShininessMultiplierPreviousNight, DayNightTransition), std::lerp(ShininessMultiplierCurrentDay, ShininessMultiplierCurrentNight, DayNightTransition), WeatherTransitionPercentage);
								data.RainSpecularMultiplier = std::lerp(std::lerp(SpecularMultiplierPreviousDay, SpecularMultiplierPreviousNight, DayNightTransition), std::lerp(SpecularMultiplierCurrentDay, SpecularMultiplierCurrentNight, DayNightTransition), WeatherTransitionPercentage);
								data.RainDiffuseMultiplier = std::lerp(std::lerp(DiffuseMultiplierPreviousDay, DiffuseMultiplierPreviousNight, DayNightTransition), std::lerp(DiffuseMultiplierCurrentDay, DiffuseMultiplierCurrentNight, DayNightTransition), WeatherTransitionPercentage);
							}
						}
					}
				}
			}
		}

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(PerPass);
		memcpy_s(mapped.pData, bytes, &data, bytes);
		context->Unmap(perPass->resource.get(), 0);

		ID3D11ShaderResourceView* views[1]{};
		views[0] = perPass->srv.get();
		context->PSSetShaderResources(22, ARRAYSIZE(views), views);
	}
}

void RainWetnessEffects::SetupResources()
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

void RainWetnessEffects::Reset()
{
	renderedScreenCamera = false;
}

void RainWetnessEffects::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void RainWetnessEffects::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

bool RainWetnessEffects::HasShaderDefine(RE::BSShader::Type shaderType)
{
	switch (shaderType) {
	case RE::BSShader::Type::Lighting:
		return true;
	default:
		return false;
	}
}