#include "State.h"

#include <magic_enum.hpp>

#include "Menu.h"
#include "ShaderCache.h"

#include "Feature.h"
#include "Features/Clustered.h"

void State::Draw()
{
	auto& shaderCache = SIE::ShaderCache::Instance();
	if (shaderCache.IsEnabled() && currentShader) {
		auto type = currentShader->shaderType.get();
		if (type > 0 && type < RE::BSShader::Type::Total) {
			if (enabledClasses[type - 1]) {
				auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

				if (auto vertexShader = shaderCache.GetVertexShader(*currentShader, currentVertexDescriptor)) {
					context->VSSetShader(vertexShader->shader, NULL, NULL);
				}

				if (auto pixelShader = shaderCache.GetPixelShader(*currentShader, currentPixelDescriptor)) {
					context->PSSetShader(pixelShader->shader, NULL, NULL);
				}

				for (auto* feature : Feature::GetFeatureList())
					feature->Draw(currentShader, currentPixelDescriptor);
			}
		}
	}

	currentShader = nullptr;
}

void State::Reset()
{
	Clustered::GetSingleton()->Reset();
	for (auto* feature : Feature::GetFeatureList())
		feature->Reset();
}

void State::Setup()
{
	for (auto* feature : Feature::GetFeatureList())
		feature->SetupResources();
}

void State::Load()
{
	auto& shaderCache = SIE::ShaderCache::Instance();

	std::string configPath = "Data\\SKSE\\Plugins\\CommunityShaders.json";

	std::ifstream i(configPath);
	if (!i.is_open()) {
		logger::error("Error opening config file ({})\n", configPath);
		return;
	}

	json settings;
	try {
		i >> settings;
	} catch (const nlohmann::json::parse_error& e) {
		logger::error("Error parsing json config file ({}) : {}\n", configPath, e.what());
		return;
	}

	if (settings["Menu"].is_object()) {
		Menu::GetSingleton()->Load(settings["Menu"]);
	}

	if (settings["Advanced"].is_object()) {
		json& advanced = settings["Advanced"];
		if (advanced["Dump Shaders"].is_boolean())
			shaderCache.SetDump(advanced["Dump Shaders"]);
		if (advanced["Log Level"].is_number_integer()) {
			logLevel = static_cast<spdlog::level::level_enum>(max(spdlog::level::trace, min(spdlog::level::off, (int)advanced["Log Level"])));
		}
	}

	if (settings["General"].is_object()) {
		json& general = settings["General"];

		if (general["Enable Shaders"].is_boolean())
			shaderCache.SetEnabled(general["Enable Shaders"]);

		if (general["Enable Disk Cache"].is_boolean())
			shaderCache.SetDiskCache(general["Enable Disk Cache"]);

		if (general["Enable Async"].is_boolean())
			shaderCache.SetAsync(general["Enable Async"]);
	}

	if (settings["Replace Original Shaders"].is_object()) {
		json& originalShaders = settings["Replace Original Shaders"];
		for (int classIndex = 0; classIndex < RE::BSShader::Type::Total - 1; ++classIndex) {
			auto name = magic_enum::enum_name((RE::BSShader::Type)(classIndex + 1));
			if (originalShaders[name].is_boolean()) {
				enabledClasses[classIndex] = originalShaders[name];
			}
		}
	}

	for (auto* feature : Feature::GetFeatureList())
		feature->Load(settings);
}

void State::Save()
{
	auto& shaderCache = SIE::ShaderCache::Instance();
	std::ofstream o(L"Data\\SKSE\\Plugins\\CommunityShaders.json");
	json settings;

	Menu::GetSingleton()->Save(settings);

	json advanced;
	advanced["Dump Shaders"] = shaderCache.IsDump();
	advanced["Log Level"] = logLevel;
	settings["Advanced"] = advanced;

	json general;
	general["Enable Shaders"] = shaderCache.IsEnabled();
	general["Enable Disk Cache"] = shaderCache.IsDiskCache();
	general["Enable Async"] = shaderCache.IsAsync();

	settings["General"] = general;

	json originalShaders;
	for (int classIndex = 0; classIndex < RE::BSShader::Type::Total - 1; ++classIndex) {
		originalShaders[magic_enum::enum_name((RE::BSShader::Type)(classIndex + 1))] = enabledClasses[classIndex];
	}
	settings["Replace Original Shaders"] = originalShaders;

	settings["Version"] = Plugin::VERSION.string();

	for (auto* feature : Feature::GetFeatureList())
		feature->Save(settings);

	o << settings.dump(1);
}

bool State::ValidateCache(CSimpleIniA& a_ini)
{
	bool valid = true;
	for (auto* feature : Feature::GetFeatureList())
		valid = valid && feature->ValidateCache(a_ini);
	return valid;
}

void State::WriteDiskCacheInfo(CSimpleIniA& a_ini)
{
	for (auto* feature : Feature::GetFeatureList())
		feature->WriteDiskCacheInfo(a_ini);
}

void State::SetLogLevel(spdlog::level::level_enum a_level)
{
	logLevel = a_level;
	spdlog::set_level(logLevel);
	spdlog::flush_on(logLevel);
	logger::info("Log Level set to {} ({})", magic_enum::enum_name(logLevel), static_cast<int>(logLevel));
}

spdlog::level::level_enum State::GetLogLevel()
{
	return logLevel;
}