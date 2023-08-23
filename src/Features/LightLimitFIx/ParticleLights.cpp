#include "Features/LightLimitFix/ParticleLights.h"

#include <numbers>

void ParticleLights::GetConfigs()
{
	auto configs = clib_util::distribution::get_configs("Data\\ParticleLights", "", ".ini");

	if (configs.empty()) {
		logger::warn("[LLF] No .ini files were found within the Data\\ParticleLights folder, aborting...");
		return;
	}

	logger::info("[LLF] {} matching inis found", configs.size());

	for (auto& path : configs) {
		logger::info("[LLF] loading ini : {}", path);

		CSimpleIniA ini;
		ini.SetUnicode();
		ini.SetMultiKey();

		if (const auto rc = ini.LoadFile(path.c_str()); rc < 0) {
			logger::error("\t\t[LLF] couldn't read INI");
			continue;
		}

		Config data{};
		data.cull = ini.GetBoolValue("Light", "Cull", false);
		data.colorMult.red = (float)ini.GetDoubleValue("Light", "ColorMultRed", 1.0);
		data.colorMult.green = (float)ini.GetDoubleValue("Light", "ColorMultGreen", 1.0);
		data.colorMult.blue = (float)ini.GetDoubleValue("Light", "ColorMultBlue", 1.0);
		data.radiusMult = (float)ini.GetDoubleValue("Light", "RadiusMult", 1.0);
		data.flicker = ini.GetBoolValue("Light", "Flicker", false);
		data.flickerSpeed = (float)ini.GetDoubleValue("Light", "FlickerSpeed", 1.0);
		data.flickerIntensity = (float)ini.GetDoubleValue("Light", "FlickerIntensity", 0.0);
		data.flickerMovement = (float)ini.GetDoubleValue("Light", "FlickerMovement", 0.0) / std::numbers::pi_v<float>;

		auto lastSeparatorPos = path.find_last_of("\\/");
		if (lastSeparatorPos != std::string::npos) {
			std::string filename = path.substr(lastSeparatorPos + 1);
			if (filename.size() < 4) {
				logger::error("[LLF] Path too short");
				continue;
			}

			filename.erase(filename.length() - 4);  // Remove ".ini"
#pragma warning(push)
#pragma warning(disable: 4244)
			std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
#pragma warning(pop)

			logger::debug("[LLF] Inserting {}", filename);

			particleLightConfigs.insert({ filename, data });
		} else {
			logger::error("[LLF] Path incomplete");
		}
	}
}
