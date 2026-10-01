#include "Material.h"

#include <cctype>
#include <cmath>
#include <set>

namespace SkinMaterials
{
	std::array<const char*, 3> TextureLabels()
	{
		return { T("feature.skin.skin_controls", "Skin controls"),
			T("feature.skin.wet_surface", "Wet surface"),
			T("feature.skin.detail_normal_mask", "Detail normal + mask") };
	}

	const char* ParameterLabel(size_t a_index)
	{
		switch (static_cast<Parameter>(a_index)) {
		case Parameter::Roughness:
			return T("feature.skin.primary_roughness", "Primary Roughness");
		case Parameter::Reflectance:
			return T("feature.skin.reflectance", "Reflectance (F0)");
		case Parameter::Fuzz:
			return T("feature.skin.fuzz", "Fuzz");
		case Parameter::DetailEnabled:
			return T("feature.skin.detail_enabled", "Enable detail");
		case Parameter::DetailStrength:
			return T("feature.skin.detail_strength", "Detail strength");
		case Parameter::DetailTiling:
			return T("feature.skin.detail_tiling", "Detail tiling");
		case Parameter::SSSAmount:
			return T("feature.skin.sss_amount", "SSS strength");
		case Parameter::Transmission:
			return T("feature.skin.transmission", "Transmission");
		case Parameter::TransmissionDepth:
			return T("feature.skin.transmission_depth", "Transmission depth");
		case Parameter::WetResponse:
			return T("feature.skin.wet_response", "Wet response");
		case Parameter::SecondaryRoughness:
			return T("feature.skin.secondary_roughness", "Secondary Roughness");
		case Parameter::SpecularTextureMultiplier:
			return T("feature.skin.specular_texture_multiplier", "Specular Texture Multiplier");
		case Parameter::SecondarySpecularStrength:
			return T("feature.skin.secondary_specular_strength", "Secondary Specular Strength");
		case Parameter::BaseColorMultiplier:
			return T("feature.skin.base_color_multiplier", "Base Color Multiplier");
		case Parameter::PhysicalMainRoughnessMultiplier:
			return T("feature.skin.physical_main_roughness_multiplier", "Physical Main Roughness Multiplier");
		case Parameter::PhysicalSecondRoughnessMultiplier:
			return T("feature.skin.physical_second_roughness_multiplier", "Physical Second Roughness Multiplier");
		case Parameter::PhysicalSpecularStrength:
			return T("feature.skin.physical_specular_multiplier", "Physical Specular Multiplier");
		case Parameter::ExtraEdgeRoughness:
			return T("feature.skin.extra_edge_roughness", "Extra Edge Roughness");
		case Parameter::FuzzRoughness:
			return T("feature.skin.fuzz_roughness", "Fuzz Roughness");
		case Parameter::FuzzF0:
			return T("feature.skin.fuzz_f0", "Fuzz F0");
		case Parameter::TransmissionEnabled:
			return T("feature.skin.transmission_enabled", "Enable transmission");
		case Parameter::BodyTilingMultiplier:
			return T("feature.skin.body_tiling_multiplier", "Body Tiling Multiplier");
		default:
			return "";
		}
	}
	const char* SettingName(size_t a_index)
	{
		static constexpr std::array<const char*, ParameterCount> names{
			"SkinMainRoughness", "F0", "FuzzStrength", "EnableSkinDetail", "SkinDetailStrength", "SkinDetailTiling", "SSSAmount", "Translucency", "sssWidth", "WetResponse", "SkinSecondRoughness", "SkinSpecularTexMultiplier", "SecondarySpecularStrength", "BaseColorMultiplier", "PhysicalMainRoughnessMultiplier", "PhysicalSecondRoughnessMultiplier", "PhysicalSpecularStrength", "ExtraEdgeRoughness", "FuzzRoughness", "FuzzF0", "UseSSS", "BodyTilingMultiplier"
		};
		return names.at(a_index);
	}

	std::span<const ParameterInfo, ParameterCount> ParameterTable()
	{
		static constexpr std::array<ParameterInfo, ParameterCount> fields{ {
#define SKIN_PARAMETER(id, name, label, value, min, max, integer) { name, label, value, min, max, integer },
#include "Parameters.def"
#undef SKIN_PARAMETER
		} };
		return fields;
	}

	std::string NormalizeTexturePath(std::string a_path)
	{
		std::replace(a_path.begin(), a_path.end(), '\\', '/');
		std::transform(a_path.begin(), a_path.end(), a_path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (a_path.starts_with("data/"))
			a_path.erase(0, 5);
		if (a_path.empty())
			return {};
		if (!a_path.starts_with("textures/") || !a_path.ends_with(".dds") || a_path.size() > 1024 ||
			a_path.find(':') != std::string::npos || a_path.find("//") != std::string::npos ||
			a_path.find("/../") != std::string::npos || a_path.find("/./") != std::string::npos ||
			std::any_of(a_path.begin(), a_path.end(), [](unsigned char c) { return c < 32; }))
			throw std::runtime_error(T("feature.skin.choose_a_dds_resource_inside_textures", "Choose a DDS resource inside textures/."));
		for (size_t start = 0; start < a_path.size();) {
			const auto end = a_path.find('/', start);
			const auto segment = a_path.substr(start, end == std::string::npos ? end : end - start);
			const auto stem = segment.substr(0, segment.find('.'));
			if (segment.empty() || segment.back() == '.' || segment.back() == ' ' ||
				segment.find_first_of("<>\"|?*") != std::string::npos || stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
				(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9'))
				throw std::runtime_error(T("feature.skin.choose_a_dds_resource_inside_textures", "Choose a DDS resource inside textures/."));
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
		return a_path;
	}

	void Validate(const Material& a_material)
	{
		const auto fields = ParameterTable();
		for (size_t i = 0; i < fields.size(); ++i) {
			const float value = a_material.parameters.values[i];
			const auto& field = fields[i];
			if (!std::isfinite(value) || value < field.minimum || value > field.maximum ||
				(field.integer && value != 0.0f && value != 1.0f))
				throw std::runtime_error(std::string(T("feature.skin.invalid_value", "Invalid value: ")) + ParameterLabel(i));
		}
		for (const auto& path : a_material.textures)
			NormalizeTexturePath(path);
	}

	void Validate(const Changes& a_changes)
	{
		Material material;
		for (size_t i = 0; i < ParameterCount; ++i)
			if (a_changes.parameters[i])
				material.parameters.values[i] = *a_changes.parameters[i];
		for (size_t i = 0; i < a_changes.textures.size(); ++i) {
			const auto& texture = a_changes.textures[i];
			if (texture.mode > TextureMode::Default || (texture.mode == TextureMode::Resource && texture.path.empty()) ||
				(texture.mode != TextureMode::Resource && !texture.path.empty()))
				throw std::runtime_error(T("feature.skin.invalid_texture_selection", "Invalid texture selection."));
			material.textures[i] = texture.path;
		}
		Validate(material);
	}

	Material Apply(Material a_base, const Changes& a_changes)
	{
		for (size_t i = 0; i < ParameterCount; ++i)
			if (a_changes.parameters[i]) {
				a_base.parameters.values[i] = *a_changes.parameters[i];
				a_base.specified[i] = true;
			}
		for (size_t i = 0; i < a_changes.textures.size(); ++i) {
			const auto& texture = a_changes.textures[i];
			if (texture.mode != TextureMode::Inherit)
				a_base.textures[i] = texture.mode == TextureMode::Resource ? texture.path : "";
		}
		a_base.enabled = true;
		return a_base;
	}

	Material Resolve(Material a_material, const Parameters& a_defaults)
	{
		for (size_t i = 0; i < ParameterCount; ++i)
			if (!a_material.enabled || !a_material.specified[i])
				a_material.parameters.values[i] = a_defaults.values[i];
		if (!a_material.enabled)
			a_material.textures = {};
		a_material.specified.fill(true);
		a_material.enabled = true;
		return a_material;
	}

	bool IsCompatible(const RE::BSShaderProperty* a_property)
	{
		const auto* material = a_property ? a_property->material : nullptr;
		using Feature = RE::BSShaderMaterial::Feature;
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		return material && material->GetType() == RE::BSShaderMaterial::Type::kLighting &&
		       (material->GetFeature() == Feature::kFaceGen || material->GetFeature() == Feature::kFaceGenRGBTint) &&
		       !a_property->flags.any(Flag::kMenuScreen, Flag::kEnvMap, Flag::kMultiLayerParallax, Flag::kDecal, Flag::kDynamicDecal);
	}

	Definition ReadProperty(RE::BSShaderProperty* a_property)
	{
		Definition result;
		if (!a_property)
			return result;
		try {
			std::map<std::string, RE::NiExtraData*> extras;
			for (uint16_t i = 0; i < a_property->extraDataSize; ++i) {
				auto* extra = a_property->extra[i];
				if (!extra || !extra->name.c_str())
					continue;
				const std::string name(extra->name.c_str());
				if (name.starts_with("CS_Skin") && !extras.emplace(name, extra).second)
					throw std::runtime_error(T("feature.skin.duplicate_skin_field", "Duplicate Skin field: ") + name);
			}
			const auto marker = extras.find("CS_SkinVersion");
			if (marker == extras.end())
				return result;
			auto* version = netimmerse_cast<RE::NiIntegerExtraData*>(marker->second);
			if (!version || version->value != static_cast<int32_t>(Version))
				throw std::runtime_error(T("feature.skin.unsupported_cs_skinversion_or_field_type", "Unsupported CS_SkinVersion or field type."));
			if (!IsCompatible(a_property))
				throw std::runtime_error(T("feature.skin.advanced_skin_requires_an_unconflicted_facegen_material", "Advanced Skin requires an unconflicted FaceGen material."));
			const auto fields = ParameterTable();
			for (size_t i = 0; i < fields.size(); ++i) {
				const auto found = extras.find(fields[i].name);
				if (found == extras.end())
					continue;
				float value;
				if (fields[i].integer) {
					auto* number = netimmerse_cast<RE::NiIntegerExtraData*>(found->second);
					if (!number || (number->value != 0 && number->value != 1))
						throw std::runtime_error(T("feature.skin.expected_integer_data_0_or_1", "Expected Integer Data 0 or 1: ") + found->first);
					value = static_cast<float>(number->value);
				} else {
					auto* number = netimmerse_cast<RE::NiFloatExtraData*>(found->second);
					if (!number || !std::isfinite(number->value))
						throw std::runtime_error(T("feature.skin.expected_finite_float_data", "Expected finite Float Data: ") + found->first);
					value = number->value;
				}
				result.material.specified[i] = true;
				result.material.parameters.values[i] = std::clamp(value, fields[i].minimum, fields[i].maximum);
				if (value != result.material.parameters.values[i])
					result.diagnostic += found->first + T("feature.skin.was_clamped", " was clamped. ");
			}
			auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material);
			if (material->textureSet) {
				for (size_t i = 0; i < TextureSlots.size(); ++i) {
					const auto* path = material->textureSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(TextureSlots[i]));
					result.material.textures[i] = NormalizeTexturePath(path ? path : "");
				}
			}
			result.material.enabled = true;
			result.status = Status::Valid;
		} catch (const std::exception& e) {
			result.material = {};
			result.status = Status::Invalid;
			result.diagnostic = e.what();
		}
		return result;
	}
}
