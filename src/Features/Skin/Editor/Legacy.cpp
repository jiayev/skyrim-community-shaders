#include "Legacy.h"

#include <cmath>

namespace SkinEditor
{
	std::vector<std::pair<std::string, SkinMaterials::Material>> ImportLegacy(const std::string& a_path)
	{
		using namespace SkinMaterials;
		const auto bytes = ReadResource(a_path);
		if (bytes.size() > 4 * 1024 * 1024)
			throw std::runtime_error(T("feature.skin.legacy_size", "Legacy import exceeds 4 MiB."));
		const auto data = json::parse(bytes.begin(), bytes.end(), [](int depth, json::parse_event_t, json&) {
			if (depth > 16)
				throw std::runtime_error(T("feature.skin.legacy_depth", "Legacy import is nested too deeply."));
			return true;
		});
		std::vector<std::pair<std::string, Material>> result;
		const auto read = [&](const json& value) {
			if (result.size() >= 256)
				throw std::runtime_error(T("feature.skin.legacy_count", "Select a legacy file with at most 256 materials."));
			const auto& numbers = value.at("parameters");
			const auto number = [&](const char* key, float fallback) {
				const float input = numbers.value(key, fallback);
				if (!std::isfinite(input))
					throw std::runtime_error(T("feature.skin.legacy_number", "Legacy material contains an invalid number."));
				return input;
			};
			Material material;
			material.enabled = true;
			const auto& textures = value.at("textures");
			if (textures.contains("rfaos") && !textures.at("rfaos").is_null())
				material.textures[0] = NormalizeTexturePath(textures.at("rfaos").get<std::string>());
			const bool controls = !material.textures[0].empty();
			material.parameters[Parameter::Roughness] = std::clamp(controls ? number("PhysicalMainRoughnessMultiplier", 1.3f) : number("SkinMainRoughness", 0.7f), 0.f, 1.f);
			material.parameters[Parameter::Reflectance] = std::clamp(controls ? 0.08f * number("PhysicalSpecularStrength", 1.f) : number("F0", 0.0278f), 0.f, 0.08f);
			material.parameters[Parameter::Fuzz] = std::clamp(number("FuzzStrength", 1.f), 0.f, 1.f);
			material.parameters[Parameter::DetailEnabled] = numbers.value("EnableSkinDetail", true) ? 1.f : 0.f;
			material.parameters[Parameter::Transmission] = std::clamp(number("Translucency", 0.1f), 0.f, 1.f);
			material.parameters[Parameter::TransmissionDepth] = std::clamp(number("sssWidth", 0.2f), 0.001f, 1.f);
			Validate(material);
			result.emplace_back(value.value("name", std::string(T("feature.skin.skin_material", "Skin material"))), std::move(material));
		};
		if (data.contains("parameters") && data.contains("textures"))
			read(data);
		else {
			if (data.value("schemaVersion", 0) != 1)
				throw std::runtime_error(T("feature.skin.legacy_version", "Unsupported legacy import version."));
			if (data.contains("material") && !data.at("material").is_null())
				read(data.at("material"));
			for (const auto key : { "recordMaterials", "edits" })
				if (data.contains(key))
					for (const auto& entry : data.at(key))
						if (entry.contains("material") && !entry.at("material").is_null())
							read(entry.at("material"));
		}
		if (result.empty())
			throw std::runtime_error(T("feature.skin.legacy_empty", "This legacy file contains no complete material to import."));
		return result;
	}
}
