#include "Skin.h"
#include <DirectXTex.h>

#include "Features/Skin/BSLightingShaderMaterialFacegenExtended.h"
#include "Features/Skin/BSLightingShaderMaterialFacegenTintExtended.h"
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
	PhysicalMainRoughnessMultiplier,
	PhysicalSecondRoughnessMultiplier,
	PhysicalSpecularStrength,
	ExtraEdgeRoughness,
	EnableSkinDetail,
	SkinDetailStrength,
	SkinDetailTiling,
	BodyTilingMultiplier,
	ApplySpecularToWetness,
	ExtraSkinWetness,
	Translucency,
	sssWidth,
	thicknessMult,
	UseSSS,
	UseCalcThickness,
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

	ImGui::Checkbox("Use Calculated Thickness", &settings.UseCalcThickness);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("This will only work for exterior directional light. And it's far from precise. I don't recommend using it.");
	}

	ImGui::SliderFloat("Translucency", &settings.Translucency, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Translucency of the SSS Transmittance effect");
	}

	ImGui::SliderFloat("SSS Width", &settings.sssWidth, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Width of the SSS Transmittance effect");
	}

	ImGui::SliderFloat("Calculated Thickness Multiplier", &settings.thicknessMult, 0.0f, 50.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Multiplier for the calculated thickness");
	}

	ImGui::Spacing();

	ImGui::SliderFloat("Extra Skin Wetness", &settings.ExtraSkinWetness, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Extra wetness for skin adding to wetness feature");
	}

	ImGui::Checkbox("Apply Specular to Wetness", &settings.ApplySpecularToWetness);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Applies specular texture to wetness feature instead of roughness (needs Wetness Effects enabled)");
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

Skin::SkinData Skin::GetCommonBufferData()
{
	SkinData data{};
	data.skinParams = float4(settings.SkinMainRoughness, settings.SkinSecondRoughness, settings.SkinSpecularTexMultiplier, float(settings.EnableSkin));
	data.skinParams2 = float4(settings.SecondarySpecularStrength, settings.ExtraSkinWetness, settings.F0, settings.ExtraEdgeRoughness);
	data.skinDetailParams = float4(settings.SkinDetailTiling, settings.BodyTilingMultiplier, settings.SkinDetailStrength, float(settings.EnableSkinDetail && settings.EnableSkin));
	data.sssParams = float4(settings.Translucency, settings.sssWidth, settings.thicknessMult * float(settings.UseCalcThickness), float(settings.UseSSS));
	data.fuzzParams = float4(settings.FuzzStrength, settings.FuzzRoughness, settings.FuzzF0, 0.0f);
	data.physicalParams = float4(settings.PhysicalMainRoughnessMultiplier, settings.PhysicalSecondRoughnessMultiplier, settings.PhysicalSpecularStrength, 0.0f);
	data.ApplySpecularToWetness = uint(settings.ApplySpecularToWetness);
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

void Skin::OnLoadTextureSet(RE::BSLightingShaderMaterialBase const* material, RE::BSTextureSet* inTextureSet)
{
	if (!inTextureSet || material->normalTexture == nullptr) {
		return;
	}

	const char extraTextureName[] = "_extra.dds";
	const char* workingNormalPath = nullptr;
	const char* workingSpecularPath = nullptr;
	auto workingTextureSet = inTextureSet;
	auto hasSpecular = material->specularBackLightingTexture != nullptr;

	if (hasSpecular) {
		if (auto specularPath = workingTextureSet->GetTexturePath(RE::BSTextureSet::Texture::kSpecular)) {
			workingSpecularPath = specularPath;
		}
	} else if (auto normalPath = workingTextureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal)) {
		workingNormalPath = normalPath;
	} else {
		logger::error("[Advanced Skin] OnLoadTextureSet : No specular or normal texture found in texture set from material: {}", material->GetFeature());
		return;
	}

	const char* foundPath = nullptr;
	const char* extraTexturePath = nullptr;
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
			newPath.replace(pos, 6, extraTextureName);
			extraTexturePath = newPath.c_str();
			foundPath = workingSpecularPath;
		}
	} else {
		if (workingNormalPath && findIgnoreCase(workingNormalPath, "_n.dds") != std::string_view::npos) {
			auto pos = findIgnoreCase(workingNormalPath, "_n.dds");
			if (pos != std::string_view::npos) {
				auto newPath = std::string(workingNormalPath);
				newPath.replace(pos, 6, extraTextureName);
				extraTexturePath = newPath.c_str();
				foundPath = workingNormalPath;
			}
		} else if (workingNormalPath && findIgnoreCase(workingNormalPath, "_msn.dds") != std::string_view::npos) {
			auto pos = findIgnoreCase(workingNormalPath, "_msn.dds");
			if (pos != std::string_view::npos) {
				auto newPath = std::string(workingNormalPath);
				newPath.replace(pos, 8, extraTextureName);
				extraTexturePath = newPath.c_str();
				foundPath = workingNormalPath;
			}
		} else {
			auto pos = findIgnoreCase(std::string_view(workingNormalPath), ".dds");
			if (pos != std::string_view::npos) {
				auto newPath = std::string(workingNormalPath);
				newPath.replace(pos, 4, extraTextureName);
				extraTexturePath = newPath.c_str();
				foundPath = workingNormalPath;
			}
		}
	}
	if (!extraTexturePath) {
		return;
	}
	logger::debug("[Advanced Skin] OnLoadTextureSet : Extra texture path: {} for {}", extraTexturePath, foundPath);
}

void Skin::BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material)
{
	auto materialFeature = material->GetFeature();
	if (materialFeature != RE::BSShaderMaterial::Feature::kFaceGen &&
		materialFeature != RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
		return;
	}

	// bool useExtraTextures = false;
	// const char extraTextureName[] = "_extra.dds";

	// auto workingSpecularPath = material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kSpecular);
	// auto workingNormalPath = material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal);
	// const char* foundPath = nullptr;
	// const char* extraTexturePath = nullptr;
	// if (!workingSpecularPath && !workingNormalPath) {
	// 	return;
	// }

	// auto shadowState = globals::game::shadowState;

	// auto findIgnoreCase = [](std::string_view str, std::string_view pattern) -> size_t {
	// 	auto it = std::search(str.begin(), str.end(), pattern.begin(), pattern.end(),
	// 						[](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); });
	// 	return it == str.end() ? std::string_view::npos : std::distance(str.begin(), it);
	// };

	// if (workingSpecularPath && findIgnoreCase(workingSpecularPath, "_s.dds") != std::string_view::npos) {
	// 	auto pos = findIgnoreCase(workingSpecularPath, "_s.dds");
	// 	if (pos != std::string_view::npos) {
	// 		auto newPath = std::string(workingSpecularPath);
	// 		newPath.replace(pos, 6, extraTextureName);
	// 		extraTexturePath = newPath.c_str();
	// 		foundPath = workingSpecularPath;
	// 	}
	// } else {
	// 	if (workingNormalPath && findIgnoreCase(workingNormalPath, "_n.dds") != std::string_view::npos) {
	// 		auto pos = findIgnoreCase(workingNormalPath, "_n.dds");
	// 		if (pos != std::string_view::npos) {
	// 			auto newPath = std::string(workingNormalPath);
	// 			newPath.replace(pos, 6, extraTextureName);
	// 			extraTexturePath = newPath.c_str();
	// 			foundPath = workingNormalPath;
	// 		}
	// 	} else if (workingNormalPath && findIgnoreCase(workingNormalPath, "_msn.dds") != std::string_view::npos) {
	// 		auto pos = findIgnoreCase(workingNormalPath, "_msn.dds");
	// 		if (pos != std::string_view::npos) {
	// 			auto newPath = std::string(workingNormalPath);
	// 			newPath.replace(pos, 8, extraTextureName);
	// 			extraTexturePath = newPath.c_str();
	// 			foundPath = workingNormalPath;
	// 		}
	// 	} else {
	// 		auto pos = findIgnoreCase(std::string_view(workingNormalPath), ".dds");
	// 		if (pos != std::string_view::npos) {
	// 			auto newPath = std::string(workingNormalPath);
	// 			newPath.replace(pos, 4, extraTextureName);
	// 			extraTexturePath = newPath.c_str();
	// 			foundPath = workingNormalPath;
	// 		}
	// 	}
	// }

	// if (!extraTexturePath) {
	// 	return;
	// }

	// logger::debug("Extra texture path: {} for {}", extraTexturePath, foundPath);

	return;
}

struct BSLightingShaderMaterialFacegen_OnLoadTextureSet
{
	static void thunk(RE::BSLightingShaderMaterialFacegen* material, std::uint64_t a_arg1, RE::BSTextureSet* a_textureSet)
	{
		func(material, a_arg1, a_textureSet);
		auto skin = globals::features::skin;
		if (skin) {
			skin->OnLoadTextureSet(material, a_textureSet);
		}
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct BSLightingShaderMaterialFacegenTint_OnLoadTextureSet
{
	static void thunk(RE::BSLightingShaderMaterialFacegenTint* material, std::uint64_t a_arg1, RE::BSTextureSet* a_textureSet)
	{
		func(material, a_arg1, a_textureSet);
		auto skin = globals::features::skin;
		if (skin) {
			skin->OnLoadTextureSet(material, a_textureSet);
		}
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void Skin::PostPostLoad()
{
	logger::info("[Advanced Skin] Hooking BSLightingShaderMaterial");
	stl::write_vfunc<08, BSLightingShaderMaterialFacegen_OnLoadTextureSet>(RE::VTABLE_BSLightingShaderMaterialFacegen[0]);
	stl::write_vfunc<08, BSLightingShaderMaterialFacegenTint_OnLoadTextureSet>(RE::VTABLE_BSLightingShaderMaterialFacegenTint[0]);
}