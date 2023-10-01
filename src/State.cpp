#include "State.h"

#include <magic_enum.hpp>
#include <pystring/pystring.h>

#include "Menu.h"
#include "ShaderCache.h"

#include "Feature.h"

void State::Draw()
{
	auto& shaderCache = SIE::ShaderCache::Instance();
	if (shaderCache.IsEnabled() && currentShader) {
		auto type = currentShader->shaderType.get();
		if (type > 0 && type < RE::BSShader::Type::Total) {
			if (enabledClasses[type - 1]) {
				ModifyShaderLookup(*currentShader, currentVertexDescriptor, currentPixelDescriptor);

				auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

				if (auto vertexShader = shaderCache.GetVertexShader(*currentShader, currentVertexDescriptor)) {
					context->VSSetShader(vertexShader->shader, NULL, NULL);
				}

				if (auto pixelShader = shaderCache.GetPixelShader(*currentShader, currentPixelDescriptor)) {
					context->PSSetShader(pixelShader->shader, NULL, NULL);
				}

				for (auto* feature : Feature::GetFeatureList()) {
					if (feature->loaded) {
						feature->Draw(currentShader, currentPixelDescriptor);
					}
				}
			}
		}
	}

	currentShader = nullptr;
}

void State::DrawDeferred()
{
	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->loaded) {
			feature->DrawDeferred();
		}
	}
}

void State::Reset()
{
	for (auto* feature : Feature::GetFeatureList())
		if (feature->loaded)
			feature->Reset();
}

void State::Setup()
{
	for (auto* feature : Feature::GetFeatureList())
		if (feature->loaded)
			feature->SetupResources();
	SetupResources();
}

void State::Load()
{
	auto& shaderCache = SIE::ShaderCache::Instance();

	std::string configPath = userConfigPath;
	std::ifstream i(configPath);
	if (!i.is_open()) {
		logger::info("Unable to open user config file ({}); trying default ({})", configPath, defaultConfigPath);
		configPath = defaultConfigPath;
		i.open(configPath);
		if (!i.is_open()) {
			logger::error("Error opening config file ({})\n", configPath);
			return;
		}
	}
	logger::info("Loading config file ({})", configPath);

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
			logLevel = static_cast<spdlog::level::level_enum>((int)advanced["Log Level"]);
			//logLevel = static_cast<spdlog::level::level_enum>(max(spdlog::level::trace, min(spdlog::level::off, (int)advanced["Log Level"])));
		}
		if (advanced["Shader Defines"].is_string())
			SetDefines(advanced["Shader Defines"]);
		if (advanced["Compiler Threads"].is_number_integer())
			shaderCache.compilationThreadCount = std::clamp(advanced["Compiler Threads"].get<int32_t>(), 1, static_cast<int32_t>(std::thread::hardware_concurrency()));
		if (advanced["Background Compiler Threads"].is_number_integer())
			shaderCache.backgroundCompilationThreadCount = std::clamp(advanced["Background Compiler Threads"].get<int32_t>(), 1, static_cast<int32_t>(std::thread::hardware_concurrency()));
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
	i.close();
	if (settings["Version"].is_string() && settings["Version"].get<std::string>() != Plugin::VERSION.string()) {
		logger::info("Found older config for version {}; upgrading to {}", (std::string)settings["Version"], Plugin::VERSION.string());
		Save();
	}

	upscalerLoaded = GetModuleHandleW(L"Data\\SKSE\\Plugins\\SkyrimUpscaler.dll");
	if (upscalerLoaded)
		logger::info("Skyrim Upscaler detected");
	else
		logger::info("Skyrim Upscaler not detected");
}

void State::Save()
{
	auto& shaderCache = SIE::ShaderCache::Instance();
	std::ofstream o(userConfigPath);
	json settings;

	Menu::GetSingleton()->Save(settings);

	json advanced;
	advanced["Dump Shaders"] = shaderCache.IsDump();
	advanced["Log Level"] = logLevel;
	advanced["Shader Defines"] = shaderDefinesString;
	advanced["Compiler Threads"] = shaderCache.compilationThreadCount;
	advanced["Background Compiler Threads"] = shaderCache.backgroundCompilationThreadCount;
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
	logger::info("Saving settings to {}", userConfigPath);
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

void State::SetDefines(std::string a_defines)
{
	shaderDefines.clear();
	shaderDefinesString = "";
	std::string name = "";
	std::string definition = "";
	auto defines = pystring::split(a_defines, ";");
	for (const auto& define : defines) {
		auto cleanedDefine = pystring::strip(define);
		auto token = pystring::split(cleanedDefine, "=");
		if (token.empty() || token[0].empty())
			continue;
		if (token.size() > 2) {
			logger::warn("Define string has too many '='; ignoring {}", define);
			continue;
		}
		name = pystring::strip(token[0]);
		if (token.size() == 2) {
			definition = pystring::strip(token[1]);
		}
		shaderDefinesString += pystring::strip(define) + ";";
		shaderDefines.push_back(std::pair(name, definition));
	}
	shaderDefinesString = shaderDefinesString.substr(0, shaderDefinesString.size() - 1);
	logger::debug("Shader Defines set to {}", shaderDefinesString);
}

std::vector<std::pair<std::string, std::string>>* State::GetDefines()
{
	return &shaderDefines;
}

bool State::ShaderEnabled(const RE::BSShader::Type a_type)
{
	auto index = static_cast<uint32_t>(a_type) + 1;
	if (index && index < sizeof(enabledClasses)) {
		return enabledClasses[index];
	}
	return false;
}

bool State::IsShaderEnabled(const RE::BSShader& a_shader)
{
	return ShaderEnabled(a_shader.shaderType.get());
}

bool State::IsDeveloperMode()
{
	return GetLogLevel() <= spdlog::level::debug;
}

void State::SetupResources()
{
	D3D11_BUFFER_DESC sbDesc{};
	sbDesc.Usage = D3D11_USAGE_DYNAMIC;
	sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	sbDesc.StructureByteStride = sizeof(PerShader);
	sbDesc.ByteWidth = sizeof(PerShader);
	perShader = std::make_unique<Buffer>(sbDesc);

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = 1;
	perShader->CreateSRV(srvDesc);
}

void State::ModifyShaderLookup(const RE::BSShader& a_shader, uint& a_vertexDescriptor, uint& a_pixelDescriptor)
{
	if (a_shader.shaderType.get() == RE::BSShader::Type::Lighting || a_shader.shaderType.get() == RE::BSShader::Type::Water) {
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

		if (a_vertexDescriptor != lastVertexDescriptor || a_pixelDescriptor != lastPixelDescriptor) {
			PerShader data{};
			data.VertexShaderDescriptor = a_vertexDescriptor;
			data.PixelShaderDescriptor = a_pixelDescriptor;

			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(perShader->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			size_t bytes = sizeof(PerShader);
			memcpy_s(mapped.pData, bytes, &data, bytes);
			context->Unmap(perShader->resource.get(), 0);

			lastVertexDescriptor = a_vertexDescriptor;
			lastPixelDescriptor = a_pixelDescriptor;
		}

		if (a_shader.shaderType.get() == RE::BSShader::Type::Lighting) {
			a_vertexDescriptor &= ~((uint32_t)SIE::ShaderCache::LightingShaderFlags::AdditionalAlphaMask |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::AmbientSpecular |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::DoAlphaTest |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::ShadowDir |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::DefShadow |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::CharacterLight |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::RimLighting |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::SoftLighting |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::BackLighting |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::Specular |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::AnisoLighting |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::BaseObjectIsSnow |
									(uint32_t)SIE::ShaderCache::LightingShaderFlags::Snow);

			a_pixelDescriptor &= ~((uint32_t)SIE::ShaderCache::LightingShaderFlags::AdditionalAlphaMask |
								   (uint32_t)SIE::ShaderCache::LightingShaderFlags::AmbientSpecular |
								   (uint32_t)SIE::ShaderCache::LightingShaderFlags::ShadowDir |
								   (uint32_t)SIE::ShaderCache::LightingShaderFlags::DefShadow |
								   (uint32_t)SIE::ShaderCache::LightingShaderFlags::CharacterLight);

			static auto enableImprovedSnow = RE::GetINISetting("bEnableImprovedSnow:Display");
			static bool vr = REL::Module::IsVR();

			if (vr || !enableImprovedSnow->GetBool())
				a_pixelDescriptor &= ~((uint32_t)SIE::ShaderCache::LightingShaderFlags::Snow);

			{
				uint32_t technique = 0x3F & (a_vertexDescriptor >> 24);
				if (technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::Glowmap ||
					technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::Parallax ||
					technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::Facegen ||
					technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::FacegenRGBTint ||
					technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::LODObjects ||
					technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::LODObjectHD ||
					technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::MultiIndexSparkle ||
					technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::Hair)
					a_vertexDescriptor &= ~(0x3F << 24);
			}

			{
				uint32_t technique = 0x3F & (a_pixelDescriptor >> 24);
				if (technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::Glowmap)
					a_pixelDescriptor &= ~(0x3F << 24);
			}

		} else {
			a_vertexDescriptor &= ~((uint32_t)SIE::ShaderCache::WaterShaderFlags::Reflections |
									(uint32_t)SIE::ShaderCache::WaterShaderFlags::Cubemap |
									(uint32_t)SIE::ShaderCache::WaterShaderFlags::Interior |
									(uint32_t)SIE::ShaderCache::WaterShaderFlags::Reflections);

			a_pixelDescriptor &= ~((uint32_t)SIE::ShaderCache::WaterShaderFlags::Reflections |
								   (uint32_t)SIE::ShaderCache::WaterShaderFlags::Cubemap |
								   (uint32_t)SIE::ShaderCache::WaterShaderFlags::Interior);
		}

		ID3D11ShaderResourceView* view = perShader->srv.get();
		context->PSSetShaderResources(127, 1, &view);
	}
}