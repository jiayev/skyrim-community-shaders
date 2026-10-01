#pragma once

#include "I18n/I18n.h"
#include <array>
#include <optional>
#include <span>

namespace SkinMaterials
{
	inline constexpr uint32_t Version = 1;
	enum class Parameter : uint32_t
	{
#define SKIN_PARAMETER(id, name, label, value, min, max, integer) id,
#include "Parameters.def"
#undef SKIN_PARAMETER
		Count
	};
	inline constexpr size_t ParameterCount = static_cast<size_t>(Parameter::Count);
	inline constexpr std::array<Parameter, ParameterCount> ParameterOrder{
		Parameter::Roughness, Parameter::SecondaryRoughness, Parameter::SpecularTextureMultiplier,
		Parameter::SecondarySpecularStrength, Parameter::Reflectance, Parameter::BaseColorMultiplier,
		Parameter::PhysicalMainRoughnessMultiplier, Parameter::PhysicalSecondRoughnessMultiplier, Parameter::PhysicalSpecularStrength,
		Parameter::ExtraEdgeRoughness, Parameter::Fuzz, Parameter::FuzzRoughness, Parameter::FuzzF0,
		Parameter::TransmissionEnabled, Parameter::Transmission, Parameter::TransmissionDepth, Parameter::SSSAmount,
		Parameter::DetailEnabled, Parameter::DetailStrength, Parameter::DetailTiling, Parameter::BodyTilingMultiplier, Parameter::WetResponse
	};
	inline constexpr std::array<uint32_t, 3> TextureSlots{ 5, 4, 8 };
	std::array<const char*, 3> TextureLabels();
	const char* ParameterLabel(size_t a_index);
	const char* SettingName(size_t a_index);

	struct ParameterInfo
	{
		const char* name;
		const char* label;
		float initial;
		float minimum;
		float maximum;
		bool integer;
	};
	std::span<const ParameterInfo, ParameterCount> ParameterTable();

	struct Parameters
	{
		std::array<float, ParameterCount> values{
#define SKIN_PARAMETER(id, name, label, value, min, max, integer) value,
#include "Parameters.def"
#undef SKIN_PARAMETER
		};
		float& operator[](Parameter a_parameter) { return values[static_cast<size_t>(a_parameter)]; }
		float operator[](Parameter a_parameter) const { return values[static_cast<size_t>(a_parameter)]; }
		bool operator==(const Parameters&) const = default;
	};

	struct Material
	{
		Parameters parameters;
		std::array<bool, ParameterCount> specified{};
		std::array<std::string, 3> textures;
		bool enabled = false;
		bool operator==(const Material&) const = default;
	};

	enum class Status
	{
		Unmarked,
		Valid,
		Invalid
	};
	struct Definition
	{
		Material material;
		Status status = Status::Unmarked;
		std::string diagnostic;
	};

	enum class TextureMode : uint32_t
	{
		Inherit,
		Resource,
		Default
	};
	struct TextureEdit
	{
		TextureMode mode = TextureMode::Inherit;
		std::string path;
		bool operator==(const TextureEdit&) const = default;
	};
	struct Changes
	{
		std::array<std::optional<float>, ParameterCount> parameters;
		std::array<TextureEdit, 3> textures;
		bool operator==(const Changes&) const = default;
	};

	bool IsCompatible(const RE::BSShaderProperty* a_property);
	Definition ReadProperty(RE::BSShaderProperty* a_property);
	std::string NormalizeTexturePath(std::string a_path);
	void Validate(const Material& a_material);
	void Validate(const Changes& a_changes);
	Material Apply(Material a_base, const Changes& a_changes);
	Material Resolve(Material a_material, const Parameters& a_defaults);
}
