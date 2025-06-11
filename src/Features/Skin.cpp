#include "Skin.h"
#include <DirectXTex.h>

#include "Deferred.h"
#include "Hooks.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skin::Settings,
	EnableSkin,
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
	ExtraSkinWetness,
	WetFadeTime,
	StartSweat,
	FullSweat,
	WetParams,
	Translucency,
	sssWidth,
	UseSSS,
	FuzzStrength,
	FuzzRoughness,
	FuzzF0);

void Skin::DrawSettings()
{
	ImGui::Checkbox("Enable Advanced Skin", &settings.EnableSkin);

	ImGui::Text("Advanced Skin Shader using dual specular lobes.");

	ImGui::Spacing();
	ImGui::SliderFloat("Primary Roughness", &settings.SkinMainRoughness, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Controls microscopic roughness of stratum corneum layer");
	}

	ImGui::SliderFloat("Secondary Roughness", &settings.SkinSecondRoughness, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Smoothness of epidermal cell layer reflections");
		ImGui::BulletText("Should be 30-50%% lower than Primary");
	}

	ImGui::SliderFloat("Specular Texture Multiplier", &settings.SkinSpecularTexMultiplier, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Multiplier for specular map");
		ImGui::BulletText("A multiplier for the vanilla specular map, applied to the first layer's roughness");
	}

	ImGui::SliderFloat("Secondary Specular Strength", &settings.SecondarySpecularStrength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Intensity of secondary specular highlights");
	}

	ImGui::SliderFloat("Fresnel F0", &settings.F0, 0.0f, 0.1f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Fresnel reflectance");
	}
	
	ImGui::SliderFloat("Base Color Multiplier", &settings.BaseColorMultiplier, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Multiplier for the base color texture");
	}

	ImGui::Spacing();
	ImGui::Text("Options for additional roughness and specular maps.");

	ImGui::SliderFloat("Physical Main Roughness Multiplier", &settings.PhysicalMainRoughnessMultiplier, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat("Physical Second Roughness Multiplier", &settings.PhysicalSecondRoughnessMultiplier, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat("Physical Specular Multiplier", &settings.PhysicalSpecularStrength, 0.0f, 2.0f, "%.2f");

	ImGui::Spacing();

	ImGui::SliderFloat("Extra Edge Roughness", &settings.ExtraEdgeRoughness, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Extra roughness at the edges of the skin, to approximate peach fuzz on the face.");
	}

	ImGui::SliderFloat("Fuzz Strength", &settings.FuzzStrength, 0.0f, 2.0f, "%.2f");

	ImGui::SliderFloat("Fuzz Roughness", &settings.FuzzRoughness, 0.1f, 1.0f, "%.2f");

	ImGui::SliderFloat("Fuzz F0", &settings.FuzzF0, 0.0f, 0.5f, "%.4f");

	ImGui::Spacing();

	ImGui::Checkbox("Enable SSS Transmission", &settings.UseSSS);

	ImGui::SliderFloat("Translucency", &settings.Translucency, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Translucency of the SSS Transmittance effect");
	}

	ImGui::SliderFloat("SSS Width", &settings.sssWidth, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Width of the SSS Transmittance effect");
	}

	ImGui::Spacing();

	ImGui::SliderFloat("Extra Skin Wetness", &settings.ExtraSkinWetness, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Extra wetness for skin adding to wetness feature");
	}

	ImGui::SliderFloat("Wetness Fade Out Time", &settings.WetFadeTime, 0.0f, 50.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Time it takes for the wetness to reduce from 1.0 to 0.0 after leaving water");
	}

	ImGui::SliderFloat("Stamina Threshold for Sweat", &settings.StartSweat, 0.01f, 1.0f, "%.2f", 
		ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat("Full Sweat Threshold", &settings.FullSweat, 0.0f, 1.0f, "%.2f",
		ImGuiSliderFlags_AlwaysClamp);

	ImGui::SliderFloat("Wetness Perlin Noise Scale", &settings.WetParams.x, 0.0f, 1024.0f, "%1.f");
	ImGui::SliderFloat("Wetness Perlin Noise Lacunarity", &settings.WetParams.y, 0.0f, 2.0f, "%.1f");
	ImGui::SliderFloat("Wetness Perlin Noise Persistence", &settings.WetParams.z, 0.0f, 20.0f, "%.2f");
	ImGui::SliderFloat("Wetness Normal Scale", &settings.WetParams.w, 0.0f, 20.0f, "%.1f");

	if (actorWetnessMap.contains(0x14)) {
		ImGui::Text("Player Wetness: %.2f", std::min(actorWetnessMap[0x14].x + actorWetnessMap[0x14].y, 2.0f));
	} else {
		ImGui::Text("Player Wetness: 0.00");
	}

	ImGui::Text("Player Stamina: %.2f / %.2f", playerStamina, playerStaminaMax);

	if (const auto consoleRef = RE::Console::GetSelectedRef()) {
		uint32_t selectedid = consoleRef->formID;
		if (actorWetnessMap.contains(selectedid)) {
			ImGui::Text("Console Selected Wetness: %.2f", std::min(actorWetnessMap[selectedid].x + actorWetnessMap[selectedid].y, 2.0f));
		} else {
			ImGui::Text("Console Selected Wetness: 0.00");
		}
	}

	ImGui::Spacing();

	ImGui::Checkbox("Enable Skin Detail", &settings.EnableSkinDetail);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Enable skin detail texture");
	}

	ImGui::SliderFloat("Skin Detail Strength", &settings.SkinDetailStrength, -2.0f, 2.0f);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Strength of skin detail texture");
	}

	ImGui::SliderFloat("Skin Detail Tiling", &settings.SkinDetailTiling, 1.0f, 50.0f, "%1.f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("The more tiling, the more detailed the skin will be");
	}

	ImGui::SliderFloat("Body Tiling Multiplier", &settings.BodyTilingMultiplier, 0.5f, 5.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Multiply the tiling for the body to match the face");
	}

	if (ImGui::Button("Reload Skin Detail Texture")) {
		ReloadSkinDetail();
	}

	BUFFER_VIEWER_NODE(texSkinDetail, 1.0f)
}

void Skin::SetupResources()
{
	auto device = globals::d3d::device;

	logger::debug("Loading skin detail texture...");
	{
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
				.MipLevels = 10 }
		};
		texSkinDetail->CreateSRV(srvDesc);
	}

	{
		PerGeometryCB = new ConstantBuffer(ConstantBufferDesc<PerGeometryData>());
	}
}

void Skin::ReloadSkinDetail()
{
	auto device = globals::d3d::device;

	logger::debug("Reloading skin detail texture...");
	{
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
				.MipLevels = 10 }
		};
		texSkinDetail->CreateSRV(srvDesc);
	}
}

void Skin::Prepass()
{
	auto context = globals::d3d::context;

	if (texSkinDetail) {
		ID3D11ShaderResourceView* srv = texSkinDetail->srv.get();
		context->PSSetShaderResources(72, 1, &srv);
	}
}

void Skin::PostPostLoad()
{
	Hooks::Install();
}

Skin::SkinData Skin::GetCommonBufferData()
{
	SkinData data{};
	data.skinParams = float4(settings.SkinMainRoughness, settings.SkinSecondRoughness, settings.SkinSpecularTexMultiplier, float(settings.EnableSkin));
	data.skinParams2 = float4(settings.SecondarySpecularStrength, settings.ExtraSkinWetness, settings.F0, settings.BaseColorMultiplier);
	data.skinDetailParams = float4(settings.SkinDetailTiling, settings.BodyTilingMultiplier, settings.SkinDetailStrength, float(settings.EnableSkinDetail && settings.EnableSkin));
	data.sssParams = float4(settings.Translucency, settings.sssWidth, 0.0f, float(settings.UseSSS));
	data.fuzzParams = float4(settings.FuzzStrength, settings.FuzzRoughness, settings.FuzzF0, settings.ExtraEdgeRoughness);
	data.physicalParams = float4(settings.PhysicalMainRoughnessMultiplier, settings.PhysicalSecondRoughnessMultiplier, settings.PhysicalSpecularStrength, 0.0f);
	data.wetParams = settings.WetParams;
	return data;
}

void Skin::LoadSettings(json& o_json)
{
	settings = o_json;
}

void Skin::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Skin::RestoreDefaultSettings()
{
	settings = {};
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
								auto       center{ bound->center };
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

float4 Skin::GetWetness(RE::BSGeometry* geometry)
{
	float4 wetness = float4(0.0f, 0.0f, 0.0f, 0.0f);
	if (auto userData = geometry->GetUserData())
		if (auto actor = userData->As<RE::Actor>())
		{
			uint32_t refid = actor->AsReference()->formID;
			const float positionZ = actor->GetPositionZ();
			wetness.z = positionZ;
			const float stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
			const float permanentStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
			const float temporaryStamina = actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kStamina);
			const float maxStamina = permanentStamina + temporaryStamina;
			const float staminaPercentage = stamina / maxStamina;
			if (refid == 0x14) { // Player
				playerStamina = stamina;
				playerStaminaMax = maxStamina;
			}
			wetness.x = (staminaPercentage >= settings.StartSweat) ? 0.0f : 
                		(staminaPercentage <= settings.FullSweat) ? 1.0f : 
                		(settings.StartSweat - staminaPercentage) / (settings.StartSweat - settings.FullSweat);
			if (actor->IsInWater()) {
				wetness.y = 2.0f;
				float waterHeight = GetWaterHeight(actor->AsReference(), actor->GetPosition());
				wetness.w = std::max(0.0f, waterHeight - positionZ);
			} else {
				wetness.y = 0.0f;
				wetness.w = 0.0f;
			}

			if(actorWetnessMap.contains(refid)) {
				if (actorWetnessMap[refid].x < wetness.x) {
					actorWetnessMap[refid].x = wetness.x;
				} else if (actorWetnessMap[refid].x > wetness.x) {
					actorWetnessMap[refid].x -= *globals::game::deltaTime * (1.0f / settings.WetFadeTime);
					actorWetnessMap[refid].x = std::max(actorWetnessMap[refid].x, 0.0f);
					wetness.x = actorWetnessMap[refid].x;
				}

				if (actorWetnessMap[refid].y < wetness.y) {
					actorWetnessMap[refid].y = wetness.y;
					if (actorWetnessMap[refid].w < wetness.w) {
						actorWetnessMap[refid].w = wetness.w;
					} else {
						wetness.w = actorWetnessMap[refid].w;
					}
				} else if (actorWetnessMap[refid].y > wetness.y) {
					actorWetnessMap[refid].y -= *globals::game::deltaTime * (1.0f / settings.WetFadeTime);
					actorWetnessMap[refid].y = std::max(actorWetnessMap[refid].y, 0.0f);
					wetness.y = actorWetnessMap[refid].y;
					if (wetness.y == 0.0f) {
						wetness.w = 0.0f;
						actorWetnessMap[refid].w = 0.0f;
					} else if (actorWetnessMap[refid].w < wetness.w) {
						actorWetnessMap[refid].w = wetness.w;
					} else {
						wetness.w = actorWetnessMap[refid].w;
					}
				} else if (actorWetnessMap[refid].w < wetness.w) {
					actorWetnessMap[refid].w = wetness.w;
				} else {
					wetness.w = actorWetnessMap[refid].w;
				}
			} else {
				actorWetnessMap.emplace(refid, wetness);
			}
		}
	return wetness;
}

struct SkinExtendedRendererState
{
	static constexpr uint32_t NumPSTextures = 1;
	static constexpr uint32_t FirstPSTexture = 71;

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

void Skin::SetupExtraTexture(RE::BSLightingShaderMaterialBase const* material, RE::BSTextureSet* inTextureSet)
{
	if (!inTextureSet || material->normalTexture == nullptr) {
		logger::error("[Advanced Skin] SetupExtraTexture : Texture set is null for material: {}", static_cast<int>(material->GetFeature()));
		return;
	}

	uint32_t hashKey = 0;
	hashKey = material->hashKey;
	if (hashKey == 0) {
		logger::error("[Advanced Skin] SetupExtraTexture : Invalid hash key for material: {}", static_cast<int>(material->GetFeature()));
		return;
	}

	const char extraTextureName[] = "_rfaos.dds";
	const char wetnessTextureName[] = "_wet.dds";
	const char* workingNormalPath = nullptr;
	const char* workingSpecularPath = nullptr;
	auto workingMaterial = static_cast<const RE::BSLightingShaderMaterialBase*>(material);
	auto hasSpecular = workingMaterial->specularBackLightingTexture != nullptr;

	const auto& stateData = globals::game::graphicsState->GetRuntimeData();

	if (hasSpecular) {
		if (auto specularPath = inTextureSet->GetTexturePath(RE::BSTextureSet::Texture::kSpecular)) {
			workingSpecularPath = specularPath;
		}
	}
	if (auto normalPath = inTextureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal)) {
		workingNormalPath = normalPath;
	} else {
		logger::error("[Advanced Skin] SetupExtraTexture : No specular or normal texture found in texture set from material: {}", static_cast<int>(material->GetFeature()));
		return;
	}

	const char* foundPath = nullptr;
	const char* extraTexturePath = nullptr;
	const char* wetnessTexturePath = nullptr;
	if (!workingSpecularPath && !workingNormalPath) {
		return;
	}

	auto findIgnoreCase = [](std::string_view str, std::string_view pattern) -> size_t {
		auto it = std::search(str.begin(), str.end(), pattern.begin(), pattern.end(),
			[](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); });
		return it == str.end() ? std::string_view::npos : std::distance(str.begin(), it);
	};

	if (hasSpecular && workingSpecularPath && findIgnoreCase(workingSpecularPath, "_s.dds") != std::string_view::npos) {
		auto pos = findIgnoreCase(workingSpecularPath, "_s.dds");
		if (pos != std::string_view::npos) {
			auto newPath = std::string(workingSpecularPath);
			auto newPath2 = std::string(workingSpecularPath);
			newPath.replace(pos, 6, extraTextureName);
			newPath2.replace(pos, 6, wetnessTextureName);
			extraTexturePath = newPath.c_str();
			wetnessTexturePath = newPath2.c_str();
			foundPath = workingSpecularPath;
		}
	} else {
		if (workingNormalPath && findIgnoreCase(workingNormalPath, "_n.dds") != std::string_view::npos) {
			auto pos = findIgnoreCase(workingNormalPath, "_n.dds");
			if (pos != std::string_view::npos) {
				auto newPath = std::string(workingNormalPath);
				auto newPath2 = std::string(workingNormalPath);
				newPath.replace(pos, 6, extraTextureName);
				newPath2.replace(pos, 6, wetnessTextureName);
				extraTexturePath = newPath.c_str();
				wetnessTexturePath = newPath2.c_str();
				foundPath = workingNormalPath;
			}
		} else if (workingNormalPath && findIgnoreCase(workingNormalPath, "_msn.dds") != std::string_view::npos) {
			auto pos = findIgnoreCase(workingNormalPath, "_msn.dds");
			if (pos != std::string_view::npos) {
				auto newPath = std::string(workingNormalPath);
				auto newPath2 = std::string(workingNormalPath);
				newPath.replace(pos, 8, extraTextureName);
				newPath2.replace(pos, 8, wetnessTextureName);
				extraTexturePath = newPath.c_str();
				wetnessTexturePath = newPath2.c_str();
				foundPath = workingNormalPath;
			}
		} else {
			auto pos = findIgnoreCase(std::string_view(workingNormalPath), ".dds");
			if (pos != std::string_view::npos) {
				auto newPath = std::string(workingNormalPath);
				auto newPath2 = std::string(workingNormalPath);
				newPath.replace(pos, 4, extraTextureName);
				newPath2.replace(pos, 4, wetnessTextureName);
				extraTexturePath = newPath.c_str();
				wetnessTexturePath = newPath2.c_str();
				foundPath = workingNormalPath;
			}
		}
	}

	logger::debug("[Advanced Skin] SetupExtraTexture : Extra texture path: {} for {}", extraTexturePath, foundPath);

	auto& workingExtraPtr = skinExtraTextures.try_emplace(hashKey).first->second;
	workingExtraPtr[0] = stateData.defaultTextureWhite;
	workingExtraPtr[1] = stateData.defaultTextureWhite;

	inTextureSet->SetTexturePath(RE::BSTextureSet::Texture::kEnvironment, extraTexturePath);
	inTextureSet->SetTexturePath(RE::BSTextureSet::Texture::kMultilayer, wetnessTexturePath);
	inTextureSet->SetTexture(RE::BSTextureSet::Texture::kEnvironment, workingExtraPtr[0]);
	inTextureSet->SetTexture(RE::BSTextureSet::Texture::kMultilayer, workingExtraPtr[1]);
	// logger::debug("[Advanced Skin] SetupExtraTexture : Extra texture set with hash key: {}", hashKey);
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
		GetSingleton()->SetupExtraTexture(material, materialTextureSet);
	}

	auto graphicsState = globals::game::graphicsState;
	auto workingExtraPtr = skinExtraTextures[hashKey];

	const bool hasExtraTexture = workingExtraPtr[0] != nullptr && workingExtraPtr[1] != nullptr;
	const bool isExtraTextureLoaded = workingExtraPtr[0] != graphicsState->GetRuntimeData().defaultTextureBlack;
	if (hasExtraTexture && isExtraTextureLoaded) {
		skinExtendedRendererState.SetExtraSkinPSTexture(workingExtraPtr[0]->rendererTexture, workingExtraPtr[1]->rendererTexture);
	} else {
		skinExtendedRendererState.SetExtraSkinPSTexture(graphicsState->GetRuntimeData().defaultTextureBlack->rendererTexture, graphicsState->GetRuntimeData().defaultTextureBlack->rendererTexture);
	}
}

void Skin::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	auto deferred = globals::deferred;
	auto context = globals::d3d::context;

	if (deferred->deferredPass) {
		if (a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kFace, RE::BSShaderProperty::EShaderPropertyFlag::kFaceGenRGBTint)) {
			auto geometry = a_pass->geometry;
			float4 wetness = GetWetness(geometry);
			// wetness = 0.0;

			if (currentWetness != wetness) {
				currentWetness = wetness;
				PerGeometryData perGeometryData{};
				perGeometryData.skinPerGeometry = wetness;
				PerGeometryCB->Update(perGeometryData);
			}

			ID3D11Buffer* buffer = { PerGeometryCB->CB() };
			context->PSSetConstantBuffers(7, 1, &buffer);
		}
	}
}

void Skin::SetShaderResouces(ID3D11DeviceContext* a_context)
{
	if (skinExtendedRendererState.PSResourceModifiedBits != 0) {
		a_context->PSSetShaderResources(71, 1, &skinExtendedRendererState.PSTexture.at(0));
		a_context->PSSetShaderResources(74, 1, &skinExtendedRendererState.PSTexture.at(1));
	}
	skinExtendedRendererState.PSResourceModifiedBits = 0;
}

void Skin::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto skin = GetSingleton();
	skin->BSLightingShader_SetupGeometry(Pass);
	return func(This, Pass, RenderFlags);
}