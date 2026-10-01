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
			Material material;
			material.enabled = true;
			const auto& textures = value.at("textures");
			if (textures.contains("rfaos") && !textures.at("rfaos").is_null())
				material.textures[0] = NormalizeTexturePath(textures.at("rfaos").get<std::string>());
			const auto fields = ParameterTable();
			for (size_t i = 0; i < fields.size(); ++i) {
				const auto& field = fields[i];
				const float input = field.integer ? (numbers.value(SettingName(i), field.initial != 0) ? 1.0f : 0.0f) : numbers.value(SettingName(i), field.initial);
				if (!std::isfinite(input))
					throw std::runtime_error(T("feature.skin.legacy_number", "Legacy material contains an invalid number."));
				material.parameters.values[i] = std::clamp(input, field.minimum, field.maximum);
				material.specified[i] = true;
			}
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
