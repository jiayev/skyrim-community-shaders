#include "Features/PhysicalSky.h"

#include "CSEditor/SceneManager/SceneWidgetInterceptor.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Util.h"

#include <DDSTextureLoader.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <imgui_stdlib.h>
#include <numbers>

#define I18N_KEY_PREFIX "feature.physical_sky."

std::string TextureManager::Key(const std::filesystem::path& path)
{
	if (path.empty())
		return {};
	const auto utf8 = path.lexically_normal().generic_u8string();
	std::string result(utf8.begin(), utf8.end());
	std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); });
	return result;
}

void TextureManager::EnsureLoaded(const std::string& path)
{
	if (!path.empty() && !loadResults.contains(Key(std::filesystem::u8path(path))))
		LoadTexture(std::filesystem::u8path(path));
}

void TextureManager::Reload()
{
	std::vector<std::string> paths;
	for (const auto& [path, result] : loadResults)
		paths.push_back(path);
	for (const auto& path : paths)
		LoadTexture(std::filesystem::u8path(path));
}

bool TextureManager::LoadTexture(std::filesystem::path path)
{
	if (path.empty())
		return false;
	const auto key = Key(path);
	winrt::com_ptr<ID3D11ShaderResourceView> loaded;
	const HRESULT result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context,
		path.wstring().c_str(), nullptr, loaded.put());
	loadResults[key] = result;
	if (FAILED(result)) {
		texList.erase(key);
		logger::warn("Cloud texture load failed: {} (0x{:08X})", key, static_cast<uint32_t>(result));
	} else {
		texList[key] = std::move(loaded);
	}
	++revision;
	return SUCCEEDED(result);
}

void TextureManager::DrawUI()
{
	ImGui::InputText(T(TKEY("path"), "Path"), &uiPath, 0);
	if (Util::Button(T(TKEY("load"), "Load"))) {
		LoadTexture(std::filesystem::u8path(uiPath));
	}
	ImGui::SameLine();
	if (Util::Button(T(TKEY("remove"), "Remove"))) {
		loadResults[Key(std::filesystem::u8path(uiPath))] = S_FALSE;
		if (texList.erase(Key(std::filesystem::u8path(uiPath))))
			++revision;
	}

	if (ImGui::BeginListBox(T(TKEY("loaded_textures"), "Loaded Textures"))) {
		for (const auto& path : ListPaths()) {
			if (ImGui::Selectable(path.c_str(), uiPath == path))
				uiPath = path;
		}
		ImGui::EndListBox();
	}
	for (const auto& [path, result] : loadResults) {
		if (FAILED(result)) {
			ImGui::TextColored({ 1, 0.6f, 0.2f, 1 }, "%s: %s (0x%08X)",
				T(TKEY("texture_load_failed"), "Texture load failed; reload after correcting the file"), path.c_str(), static_cast<unsigned>(result));
		}
	}
}

namespace nlohmann
{
	void to_json(json& j, const TextureManager& v)
	{
		std::vector<std::string> tex_list;
		std::ranges::transform(v.texList, std::back_inserter(tex_list), [](auto kvpair) { return kvpair.first; });
		j = tex_list;
	}

	void from_json(const json& j, TextureManager& v)
	{
		if (j.empty())
			return;
		std::vector<std::string> tex_list = j;
		for (auto& tex : tex_list)
			if (!v.LoadTexture(std::filesystem::u8path(tex)))
				logger::warn("Loading texture manager from config: Texture {} missing.", tex);
	}
}

namespace
{
	eastl::unique_ptr<Texture2D> CreateWeatherTexture(uint32_t dimension, DXGI_FORMAT format, const char* name, bool mips = false, uint32_t height = 0)
	{
		D3D11_TEXTURE2D_DESC desc{
			.Width = dimension,
			.Height = height ? height : dimension,
			.MipLevels = mips ? 0u : 1u,
			.ArraySize = 1,
			.Format = format,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | (mips ? D3D11_BIND_RENDER_TARGET : 0u),
			.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0u
		};
		auto texture = eastl::make_unique<Texture2D>(desc, name);
		texture->CreateSRV({ .Format = format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = mips ? UINT(-1) : 1u } });
		texture->CreateUAV({ .Format = format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
		return texture;
	}

	float FiniteClamp(float value, float minimum, float maximum, float fallback)
	{
		return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
	}

	void SanitizeRange(float4& range, bool limitOutput = true)
	{
		range.x = FiniteClamp(range.x, -16.f, 16.f, -1.f);
		range.y = FiniteClamp(range.y, -16.f, 16.f, 1.f);
		if (std::abs(range.y - range.x) < 1e-5f)
			range.y = range.x + 1e-5f;
		range.z = limitOutput ? FiniteClamp(range.z, -16.f, 16.f, 0.f) : (std::isfinite(range.z) ? range.z : 0.f);
		range.w = limitOutput ? FiniteClamp(range.w, -16.f, 16.f, 1.f) : (std::isfinite(range.w) ? range.w : 1.f);
	}
}

void CloudWeatherManager::SetupResources()
{
	generatorCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<GenerationParameters>(), "PhysicalSky::CloudWeatherConstants");
	noiseCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<WeatherNoiseParameters>());
	for (auto& map : maps)
		map = CreateWeatherTexture(kMapSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CloudWeather");
	D3D11_SAMPLER_DESC desc{
		.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
		.ComparisonFunc = D3D11_COMPARISON_NEVER,
		.MinLOD = 0.f,
		.MaxLOD = D3D11_FLOAT32_MAX
	};
	sampler = nullptr;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&desc, sampler.put()));
	Util::SetResourceName(sampler.get(), "PhysicalSky::CloudWeatherSampler");
}

CloudWeatherSettings::CloudWeatherSettings()
{
	layout.states.push_back({ { "value", CloudWeatherLayout{} }, { "weight", 1.f } });
}

void CloudWeatherManager::CompileShaders(Util::AsyncShaderSet& a_shaders)
{
	generatedValid = false;
	for (auto& field : sharedNoise)
		field.valid = false;
	layoutNoise.clear();
	noiseInputs.fill(nullptr);
	for (auto& resource : noiseInputResources)
		resource = nullptr;
	a_shaders.Add(generatorProgram, L"Data\\Shaders\\PhysicalSky\\CloudWeatherGenerate.cs.hlsl");
	a_shaders.Add(noiseProgram, L"Data\\Shaders\\PhysicalSky\\WeatherNoise.cs.hlsl");
}

namespace
{
	/// One texture slot of the weather map: a combo over the loaded textures, plus a validity line.
	void DrawNdfTextureChoice(TextureManager& textures, const char* a_label, std::string& a_path, uint32_t a_channels,
		const char* a_emptyLabel = nullptr)
	{
		if (!a_emptyLabel)
			a_emptyLabel = T(TKEY("ndf_none"), "None");
		if (ImGui::BeginCombo(a_label, a_path.empty() ? a_emptyLabel : a_path.c_str())) {
			if (ImGui::Selectable(a_emptyLabel, a_path.empty()))
				a_path.clear();
			for (const auto& choice : textures.ListPaths())
				if (ImGui::Selectable(choice.c_str(), a_path == choice))
					a_path = choice;
			ImGui::EndCombo();
		}
		if (!a_path.empty() && !CloudWeatherManager::IsLinearTexture(textures.Query(a_path), a_channels))
			Util::Text::WrappedWarning("%s", T(TKEY("ndf_incompatible_texture"), "Missing or incompatible texture; using generated input (local influences are skipped)."));
	}
}

void CloudWeatherManager::DrawNoise(CloudWeatherSettings& settings, TextureManager& textures)
{
	if (SceneWidgetInterceptor::IsArmed())
		return;
	for (uint32_t index = 0; index < 4; ++index) {
		ImGui::PushID(static_cast<int>(index));
		if (ImGui::TreeNode("Noise", T(TKEY("ndf_noise_input"), "Noise input %u"), index)) {  // i18n-ignore: ImGui ID
			auto& input = settings.noise[index];
			DrawNdfTextureChoice(textures, T(TKEY("ndf_dds_override"), "DDS override"), input.texturePath, 1, T(TKEY("ndf_generated"), "Generated"));
			auto& noise = input.parameters;
			if (Util::Button(T(TKEY("ndf_reset_noise_parameters"), "Reset noise parameters")))
				noise = CloudWeatherSettings{}.noise[index].parameters;
			const char* noiseTypes[] = {
				T(TKEY("ndf_noise_alligator"), "Alligator"),
				T(TKEY("ndf_noise_perlin_fbm"), "Perlin fBm"),
				T(TKEY("ndf_noise_perlin_worley"), "Perlin-Worley")
			};
			int type = static_cast<int>(std::min(noise.type, 2u));
			if (ImGui::Combo(T(TKEY("ndf_noise_type"), "Noise type"), &type, noiseTypes, IM_ARRAYSIZE(noiseTypes)))
				noise.type = static_cast<uint32_t>(type);
			ImGui::InputScalar(T(TKEY("ndf_seed"), "Seed"), ImGuiDataType_U32, &noise.seed);
			const uint32_t one = 1, maxOctaves = 8, maxLacunarity = 4, maxRepetitions = 8;
			ImGui::SliderScalar(T(TKEY("ndf_tile_repetitions"), "Tile repetitions"), ImGuiDataType_U32, &noise.repetitions, &one, &maxRepetitions);
			const uint32_t maxFrequency = std::min(128u, kMapSize / (2u * std::clamp(noise.repetitions, 1u, maxRepetitions)));
			ImGui::SliderScalar(T(TKEY("ndf_base_frequency"), "Base frequency"), ImGuiDataType_U32, &noise.frequency, &one, &maxFrequency);
			ImGui::SliderScalar(T(TKEY("ndf_octaves"), "Octaves"), ImGuiDataType_U32, &noise.octaves, &one, &maxOctaves);
			ImGui::SliderScalar(T(TKEY("ndf_lacunarity"), "Lacunarity"), ImGuiDataType_U32, &noise.lacunarity, &one, &maxLacunarity);
			ImGui::SliderFloat(T(TKEY("ndf_persistence"), "Persistence"), &noise.persistence, 0.f, 1.f);
			ImGui::SliderFloat(T(TKEY("ndf_contrast"), "Contrast"), &noise.contrast, 0.1f, 8.f);
			ImGui::SliderFloat(T(TKEY("ndf_response_exponent"), "Response exponent"), &noise.responseExponent, 0.1f, 8.f);
			ImGui::SliderFloat(T(TKEY("ndf_bias"), "Bias"), &noise.bias, -1.f, 1.f);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
}

#undef I18N_KEY_PREFIX

bool CloudWeatherManager::UpdateNoise(NoiseField& field, const WeatherNoiseParameters& parameters)
{
	auto data = parameters;
	data.type = std::min(data.type, 2u);
	data.repetitions = std::clamp(data.repetitions, 1u, 8u);
	const uint32_t maxFrequency = std::min(128u, kMapSize / (2u * data.repetitions));
	data.frequency = std::clamp(data.frequency, 1u, maxFrequency);
	data.frequencyY = std::min(data.frequencyY, maxFrequency);
	data.octaves = std::clamp(data.octaves, 1u, 8u);
	data.lacunarity = std::clamp(data.lacunarity, 1u, 4u);
	data.persistence = FiniteClamp(data.persistence, 0.f, 1.f, 0.5f);
	data.contrast = FiniteClamp(data.contrast, 0.1f, 8.f, 1.f);
	data.bias = FiniteClamp(data.bias, -1.f, 1.f, 0.f);
	data.responseExponent = FiniteClamp(data.responseExponent, 0.1f, 8.f, 1.f);
	data.padding = 0.f;
	if (field.valid && std::memcmp(&field.parameters, &data, sizeof(data)) == 0)
		return false;
	if (!field.texture)
		field.texture = CreateWeatherTexture(kMapSize, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::WeatherNoise", true);

	auto* context = globals::d3d::context;
	noiseCb->Update(data);
	auto* cb = noiseCb->CB();
	auto* uav = field.texture->uav.get();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(noiseProgram.get(), nullptr, 0);
	context->Dispatch((kMapSize + 7u) >> 3, (kMapSize + 7u) >> 3, 1);
	uav = nullptr;
	cb = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);
	context->GenerateMips(field.texture->srv.get());
	field.parameters = data;
	field.valid = true;
	return true;
}

bool CloudWeatherManager::Update(const CloudWeatherSettings& settings, TextureManager& textures, double phase, float2 flow)
{
	if (!Ready() || !generatorCb || !noiseCb || !sampler)
		return false;
	auto* context = globals::d3d::context;
	std::array<ID3D11ShaderResourceView*, 4> sources{};
	for (const auto& input : settings.noise)
		textures.EnsureLoaded(input.texturePath);
	for (uint32_t i = 0; i < 4; ++i) {
		const auto& input = settings.noise[i];
		if (auto* inputTexture = textures.Query(input.texturePath); IsLinearTexture(inputTexture, 1)) {
			sources[i] = inputTexture;
			continue;
		}
		const bool changed = UpdateNoise(sharedNoise[i], input.parameters);
		if (changed) {
			++weatherNoiseRevision;
			if (i == 2u)
				++noiseRevision;
		}
		sources[i] = sharedNoise[i].texture->srv.get();
	}

	if (noiseInputs[2] != sources[2])
		++noiseRevision;
	if (sources != noiseInputs) {
		++weatherNoiseRevision;
		noiseInputs = sources;
		for (size_t i = 0; i < noiseInputs.size(); ++i)
			noiseInputResources[i].copy_from(noiseInputs[i]);
	}

	const float4 weather{
		FiniteClamp(settings.coverage, 0.f, 1.f, .58f),
		FiniteClamp(settings.cloudType, -1.f, 1.f, .24f),
		FiniteClamp(settings.typeVariation, 0.f, 1.f, .30f),
		FiniteClamp(settings.convection, 0.f, 1.f, 0.f)
	};
	// The slow local moisture changes update roughly three times a second at the default speed.
	const auto fieldPhase = static_cast<float>(std::floor(std::fmod(phase * 1.5, 4096.0) * 256.0) / 256.0);
	if (generatedValid && generatedLayout == settings.layout.states && textureRevision == textures.revision &&
		generatedNoiseRevision == weatherNoiseRevision &&
		(!hasProceduralLayout || (generatedPhase == fieldPhase && generatedFlow.x == flow.x && generatedFlow.y == flow.y)) &&
		std::memcmp(&weather, &generatedWeather, sizeof(weather)) == 0)
		return false;

	std::vector<std::pair<CloudWeatherLayout, float>> layouts;
	float totalWeight = 0.f;
	for (const auto& endpoint : settings.layout.states) {
		try {
			const float weight = endpoint.at("weight").get<float>();
			if (!std::isfinite(weight) || weight <= 0.f)
				continue;
			auto layout = endpoint.at("value").get<CloudWeatherLayout>();
			textures.EnsureLoaded(layout.paintedMap);
			layouts.emplace_back(std::move(layout), weight);
			totalWeight += weight;
		} catch (const nlohmann::json::exception&) {
		}
	}
	if (layouts.empty())
		layouts.emplace_back(CloudWeatherLayout{}, 1.f);
	else {
		for (auto& [layout, weight] : layouts)
			weight /= totalWeight;
	}

	const auto field = [](const std::array<int, 2>& cells, uint32_t seed) {
		return std::array{ static_cast<uint32_t>(std::clamp(cells[0], 1, 64)),
			static_cast<uint32_t>(std::clamp(cells[1], 1, 64)), seed, 0u };
	};
	layoutNoise.resize(layouts.size());
	std::vector<std::array<ID3D11ShaderResourceView*, kWeatherNoiseFields>> layoutSources(layouts.size());
	hasFiniteWeather = false;
	for (size_t i = 0; i < layouts.size(); ++i) {
		const auto& layout = layouts[i].first;
		if (layout.paintedWeight >= 1.f && IsLinearTexture(textures.Query(layout.paintedMap), 4))
			continue;
		hasFiniteWeather |= weather.w > 0.f;
		const auto base = field(layout.baseCells, layout.baseSeed);
		const auto storm = field(layout.stormCells, layout.stormSeed);
		const std::array<size_t, kWeatherNoiseFields> sharedInputs{ 0, 1, 3, 0 };
		for (size_t channel = 0; channel < kWeatherNoiseFields; ++channel) {
			const bool convective = channel == 3;
			const auto& configuration = convective ? storm : base;
			const auto slot = sharedInputs[channel];
			const uint32_t referenceCells = convective ? 4u : 6u;
			const uint32_t referenceSeed = convective ? 15373u : 1763u;
			const auto& shared = sharedNoise[slot];
			const bool imported = !shared.valid || shared.texture->srv.get() != sources[slot];
			if (imported || (configuration[0] == referenceCells && configuration[1] == referenceCells && configuration[2] == referenceSeed)) {
				layoutSources[i][channel] = sources[slot];
				continue;
			}
			auto params = settings.noise[slot].parameters;
			const uint32_t frequency = std::clamp(params.frequency, 1u, 128u);
			params.frequency = std::max(1u, (frequency * configuration[0] + referenceCells / 2u) / referenceCells);
			params.frequencyY = std::max(1u, (frequency * configuration[1] + referenceCells / 2u) / referenceCells);
			params.seed ^= configuration[2] ^ referenceSeed;
			auto& noise = layoutNoise[i][channel];
			UpdateNoise(noise, params);
			layoutSources[i][channel] = noise.texture->srv.get();
		}
	}
	if (hasFiniteWeather && !finiteMaps[0]) {
		// Keep the background available outside the convection footprint, including
		// any painted contribution shared by both maps during a weather transition.
		for (auto& map : finiteMaps)
			map = CreateWeatherTexture(kMapSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::FiniteCloudWeather");
	}

	auto* cb = generatorCb->CB();
	auto* samp = sampler.get();
	winrt::com_ptr<ID3D11SamplerState> previousSampler;
	context->CSGetSamplers(0, 1, previousSampler.put());
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &samp);
	context->CSSetShader(generatorProgram.get(), nullptr, 0);
	globals::profiler->BeginPass("PhysicalSky::CloudWeather");
	ID3D11ShaderResourceView* previous = nullptr;
	ID3D11ShaderResourceView* previousFinite = nullptr;
	ID3D11ShaderResourceView* nullSrvs[3 + kWeatherNoiseFields] = {};
	ID3D11UnorderedAccessView* nullUavs[2] = {};
	uint32_t target = 0;
	hasConvection = false;
	hasProceduralLayout = false;
	proceduralWeight = 0.f;
	for (size_t i = 0; i < layouts.size(); ++i) {
		const auto& [layout, weight] = layouts[i];
		auto* painted = textures.Query(layout.paintedMap);
		const float paintedWeight = IsLinearTexture(painted, 4) ? FiniteClamp(layout.paintedWeight, 0.f, 1.f, 1.f) : 0.f;
		hasConvection |= paintedWeight > 0.f || weather.w > 0.f;
		hasProceduralLayout |= paintedWeight < 1.f && (weather.x > 0.f || weather.w > 0.f);
		proceduralWeight += weight * (1.f - paintedWeight);
		const CloudWeatherLayout defaults;
		std::array<float, 3> weights;
		for (size_t channel = 0; channel < weights.size(); ++channel)
			weights[channel] = FiniteClamp(layout.coverageWeights[channel], 0.f, 1.f, defaults.coverageWeights[channel]);
		if (weights[0] + weights[1] + weights[2] <= 0.f)
			weights = defaults.coverageWeights;
		const GenerationParameters data{
			.weather = weather,
			.shape = { FiniteClamp(layout.baseEdge, .01f, 1.f, .18f), FiniteClamp(layout.stormEdge, .01f, 1.f, .24f),
				FiniteClamp(layout.coreFraction, 0.f, 1.f, .35f), paintedWeight },
			.base = field(layout.baseCells, layout.baseSeed),
			.storm = field(layout.stormCells, layout.stormSeed),
			.phase = fieldPhase,
			.weight = weight,
			.hasPrevious = previous ? 1u : 0u,
			.resolve = i + 1 == layouts.size() ? 1u : 0u,
			.organization = { flow.x, flow.y, FiniteClamp(layout.cellular, 0.f, 1.f, 0.f), FiniteClamp(layout.stormOutflow, 0.f, 1.f, .65f) },
			.coverageMix = { weights[0], weights[1], weights[2], FiniteClamp(layout.secondaryCoverage, 0.f, 1.f, 0.f) },
			.updraftOrganization = FiniteClamp(layout.updraftOrganization, 0.f, 1.f, defaults.updraftOrganization),
			.regionalCoverageStrength = FiniteClamp(layout.regionalCoverageStrength, 0.f, 1.f, 0.f),
			.regionalCells = static_cast<uint32_t>(std::clamp(layout.regionalCells, 2, 8)),
			.writeFiniteWeather = hasFiniteWeather ? 1u : 0u,
		};
		generatorCb->Update(data);
		std::array<ID3D11ShaderResourceView*, 3 + kWeatherNoiseFields> inputs{ paintedWeight > 0.f ? painted : nullptr, previous };
		std::copy(layoutSources[i].begin(), layoutSources[i].end(), inputs.begin() + 2);
		inputs.back() = previousFinite;
		ID3D11UnorderedAccessView* outputs[]{ maps[target]->uav.get(), hasFiniteWeather ? finiteMaps[target]->uav.get() : nullptr };
		context->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(outputs), outputs, nullptr);
		context->Dispatch(kMapSize / 8u, kMapSize / 8u, 1);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
		context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
		previous = maps[target]->srv.get();
		previousFinite = hasFiniteWeather ? finiteMaps[target]->srv.get() : nullptr;
		outputIndex = target;
		target ^= 1u;
	}
	globals::profiler->EndPass();
	cb = nullptr;
	samp = previousSampler.get();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &samp);
	context->CSSetShader(nullptr, nullptr, 0);

	textureRevision = textures.revision;
	generatedWeather = weather;
	generatedLayout = settings.layout.states;
	generatedPhase = fieldPhase;
	generatedFlow = flow;
	generatedNoiseRevision = weatherNoiseRevision;
	generatedValid = true;
	return true;
}

bool CloudWeatherManager::IsLinearTexture(ID3D11ShaderResourceView* srv, uint32_t channels)
{
	if (!srv)
		return false;
	D3D11_SHADER_RESOURCE_VIEW_DESC desc;
	srv->GetDesc(&desc);
	if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
		return false;
	return IsLinearFormat(desc.Format, channels);
}

bool CloudWeatherManager::IsLinearFormat(DXGI_FORMAT format, uint32_t channels)
{
	switch (format) {
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_BC4_UNORM:
		return channels <= 1u;
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_BC5_UNORM:
		return channels <= 2u;
	case DXGI_FORMAT_R32G32B32_FLOAT:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return channels <= 3u;
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC7_UNORM:
		return true;
	default:
		return false;
	}
}

bool CloudWeatherManager::IsTexturePair(NdfTextureSet maps)
{
	if (!IsLinearTexture(maps.height, 2) || !IsLinearTexture(maps.modeling, 3))
		return false;
	auto dimensions = [](ID3D11ShaderResourceView* srv) {
		D3D11_SHADER_RESOURCE_VIEW_DESC view;
		srv->GetDesc(&view);
		winrt::com_ptr<ID3D11Resource> resource;
		srv->GetResource(resource.put());
		D3D11_TEXTURE2D_DESC desc;
		resource.as<ID3D11Texture2D>()->GetDesc(&desc);
		return std::pair{ std::max(desc.Width >> view.Texture2D.MostDetailedMip, 1u), std::max(desc.Height >> view.Texture2D.MostDetailedMip, 1u) };
	};
	return dimensions(maps.height) == dimensions(maps.modeling);
}

CirrusSettings::CirrusSettings()
{
	weather.states.push_back({ { "value", CirrusWeatherState{} }, { "weight", 1.f } });
}

MidCloudSettings::MidCloudSettings()
{
	weather.states.push_back({ { "value", MidCloudWeatherState{} }, { "weight", 1.f } });
}

float2 MidCloudSettings::GetAltitudeRange() const
{
	const float base = FiniteClamp(altitude, 100.f, 16000.f, 3500.f);
	const float span = FiniteClamp(thickness, 100.f, 4000.f, 800.f);
	const float lift = span * FiniteClamp(bands.displacement, 0.f, 0.5f, 0.f);
	return { base - lift, base + span + lift };
}

void MidCloudMapManager::SetupResources()
{
	parameters = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<Parameters>(), "PhysicalSky::MiddleCloudWeatherConstants");
	for (auto& map : maps)
		map = CreateWeatherTexture(512, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::MiddleCloudWeather");
	D3D11_SAMPLER_DESC desc{};
	desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	desc.MaxLOD = D3D11_FLOAT32_MAX;
	sampler = nullptr;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&desc, sampler.put()));
	Util::SetResourceName(sampler.get(), "PhysicalSky::MiddleCloudWeatherSampler");
}

void MidCloudMapManager::CompileShaders(Util::AsyncShaderSet& a_shaders)
{
	valid = false;
	output = nullptr;
	a_shaders.Add(shader, L"Data\\Shaders\\PhysicalSky\\MidCloudGenerate.cs.hlsl");
}

bool MidCloudMapManager::Update(const MidCloudSettings& settings, TextureManager& textures, const CloudWeatherManager& weatherMap)
{
	if (!shader || !parameters || !sampler || !maps[0] || !maps[1] ||
		std::ranges::any_of(weatherMap.GetNoiseInputs(), [](auto* source) { return !source; })) {
		valid = false;
		return false;
	}
	std::vector<std::pair<MidCloudWeatherState, float>> states;
	for (const auto& endpoint : settings.weather.states) {
		try {
			const float weight = endpoint.at("weight").get<float>();
			if (!std::isfinite(weight) || weight <= 0.f)
				continue;
			auto state = endpoint.at("value").get<MidCloudWeatherState>();
			textures.EnsureLoaded(state.localMap);
			states.emplace_back(std::move(state), weight);
		} catch (const nlohmann::json::exception&) {}
	}
	const auto nextKey = nlohmann::json{ settings.weather, settings.heightPower, settings.bands.displacement }.dump();
	if (valid && key == nextKey && textureRevision == textures.revision && noiseRevision == weatherMap.GetWeatherNoiseRevision())
		return false;
	auto* context = globals::d3d::context;
	winrt::com_ptr<ID3D11SamplerState> previousSampler;
	context->CSGetSamplers(0, 1, previousSampler.put());
	auto* cb = parameters->CB();
	auto* sampling = sampler.get();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &sampling);
	context->CSSetShader(shader.get(), nullptr, 0);
	ID3D11ShaderResourceView* previous = nullptr;
	ID3D11ShaderResourceView* nullSources[6] = {};
	uint32_t target = 0;
	for (const auto& [state, weight] : states) {
		auto* local = textures.Query(state.localMap);
		if (!CloudWeatherManager::IsLinearTexture(local, 3))
			local = nullptr;
		Parameters data{
			.weather = { state.layers[0], state.layers[1], state.secondaryCoverage, state.coverageGain, state.thicknessVariation },
			.localWeight = local ? FiniteClamp(state.localWeight, 0.f, 1.f, 0.f) : 0.f,
			.localBlendMode = std::min(state.localBlendMode, 1u),
			.weight = std::clamp(weight, 0.f, 1.f),
			.hasPrevious = previous ? 1u : 0u,
			.shape = { FiniteClamp(settings.heightPower, 0.25f, 4.f, 1.f), FiniteClamp(settings.bands.displacement, 0.f, .5f, 0.f), 0.f, 0.f }
		};
		for (auto& layer : data.weather) {
			layer.noise = std::min(layer.noise, 4u);
			layer.frequency = FiniteClamp(layer.frequency, 0.f, 64.f, 1.f);
			layer.offset.x = FiniteClamp(layer.offset.x, -10000.f, 10000.f, 0.f);
			layer.offset.y = FiniteClamp(layer.offset.y, -10000.f, 10000.f, 0.f);
			SanitizeRange(layer.range, false);
		}
		parameters->Update(data);
		std::array<ID3D11ShaderResourceView*, 6> sources{};
		std::copy(weatherMap.GetNoiseInputs().begin(), weatherMap.GetNoiseInputs().end(), sources.begin());
		sources[4] = local;
		sources[5] = previous;
		auto* uav = maps[target]->uav.get();
		context->CSSetShaderResources(0, 6, sources.data());
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->Dispatch(64, 64, 1);
		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, 6, nullSources);
		previous = maps[target]->srv.get();
		target ^= 1u;
	}
	if (!previous) {
		const float empty[4] = {};
		context->ClearUnorderedAccessViewFloat(maps[0]->uav.get(), empty);
		previous = maps[0]->srv.get();
	}
	cb = nullptr;
	sampling = previousSampler.get();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &sampling);
	context->CSSetShader(nullptr, nullptr, 0);
	output.copy_from(previous);
	key = nextKey;
	textureRevision = textures.revision;
	noiseRevision = weatherMap.GetWeatherNoiseRevision();
	valid = true;
	return true;
}

void CirrusMapManager::SetupResources()
{
	generationCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<GenerationParameters>());
	for (auto& texture : texWeather)
		texture = CreateWeatherTexture(kDimension, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::CirrusWeather", true);
	D3D11_SAMPLER_DESC desc{
		.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
		.ComparisonFunc = D3D11_COMPARISON_NEVER,
		.MinLOD = 0.f,
		.MaxLOD = D3D11_FLOAT32_MAX
	};
	sampler = nullptr;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&desc, sampler.put()));
}

void CirrusMapManager::CompileShaders(Util::AsyncShaderSet& a_shaders)
{
	generatedValid = false;
	outputs = {};
	for (auto& resource : outputResources)
		resource = nullptr;
	a_shaders.Add(weatherProgram, L"Data\\Shaders\\PhysicalSky\\CirrusGenerate.cs.hlsl", {}, "generateWeather");
}

bool CirrusMapManager::ShadersReady(const CirrusSettings&) const
{
	return weatherProgram && generationCb && sampler && texWeather[0] && texWeather[1];
}

CirrusTextureSet CirrusMapManager::GetTextures() const
{
	return outputs;
}

bool CirrusMapManager::Update(const CirrusSettings& settings, TextureManager& textures, const CloudWeatherManager& weatherMap)
{
	if (!ShadersReady(settings))
		return false;
	const std::string patternPath = "Data/Textures/PhysicalSky/cirrus.dds";
	textures.EnsureLoaded(patternPath);
	std::vector<CirrusWeatherState> states;
	std::vector<float> weights;
	for (const auto& endpoint : settings.weather.states) {
		try {
			const float weight = endpoint.at("weight").get<float>();
			if (!std::isfinite(weight) || weight <= 0.f)
				continue;
			auto state = endpoint.at("value").get<CirrusWeatherState>();
			textures.EnsureLoaded(state.localMap);
			states.push_back(std::move(state));
			weights.push_back(weight);
		} catch (const nlohmann::json::exception&) {
		}
	}
	const float uvSize = settings.GetUvSize();
	const auto key = nlohmann::json{ { "weather", settings.weather.states }, { "uvSize", uvSize } }.dump();
	if (generatedValid && generatedKey == key && generatedRevision == textures.revision && noiseRevision == weatherMap.GetWeatherNoiseRevision())
		return false;
	const bool hadOutputs = bool(outputs);
	outputs = {};
	for (auto& resource : outputResources)
		resource = nullptr;
	auto* patterns = textures.Query(patternPath);
	if (!CloudWeatherManager::IsLinearTexture(patterns, 3)) {
		generatedValid = false;
		return hadOutputs;
	}
	if (std::ranges::any_of(weatherMap.GetNoiseInputs(), [](auto* input) { return !input; })) {
		generatedValid = false;
		return hadOutputs;
	}
	auto* context = globals::d3d::context;
	auto* cb = generationCb->CB();
	auto* samp = sampler.get();
	winrt::com_ptr<ID3D11SamplerState> previousSampler;
	context->CSGetSamplers(0, 1, previousSampler.put());
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &samp);
	context->CSSetShader(weatherProgram.get(), nullptr, 0);
	ID3D11ShaderResourceView* previous = nullptr;
	ID3D11ShaderResourceView* nullSrvs[6] = {};
	uint32_t target = 0;
	for (size_t i = 0; i < states.size(); ++i) {
		const auto& state = states[i];
		auto* local = textures.Query(state.localMap);
		if (!CloudWeatherManager::IsLinearTexture(local, 3))
			local = nullptr;
		GenerationParameters data{
			.weather = state.layers,
			.windOffset = {},
			.localWeight = local ? FiniteClamp(state.localWeight, 0.f, 1.f, 0.f) : 0.f,
			.localWindScale = FiniteClamp(state.localWindScale, 0.f, 16.f, 1.f),
			.localBlendMode = std::min(state.localBlendMode, 1u),
			.weight = weights[i],
			.hasPrevious = previous ? 1u : 0u
		};
		for (auto& layer : data.weather) {
			layer.noise = std::min(layer.noise, 4u);
			layer.frequency = FiniteClamp(layer.frequency, 0.f, 64.f, 1.f);
			layer.offset.x = FiniteClamp(layer.offset.x, -10000.f, 10000.f, 0.f);
			layer.offset.y = FiniteClamp(layer.offset.y, -10000.f, 10000.f, 0.f);
			SanitizeRange(layer.range);
		}
		generationCb->Update(data);
		std::array<ID3D11ShaderResourceView*, 6> sources = {};
		std::copy(weatherMap.GetNoiseInputs().begin(), weatherMap.GetNoiseInputs().end(), sources.begin());
		sources[4] = local;
		sources[5] = previous;
		context->CSSetShaderResources(0, 6, sources.data());
		auto* uav = texWeather[target]->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->Dispatch((kDimension + 7u) >> 3, (kDimension + 7u) >> 3, 1);
		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, 6, nullSrvs);
		previous = texWeather[target]->srv.get();
		target ^= 1u;
	}
	if (!previous) {
		const float empty[4] = {};
		context->ClearUnorderedAccessViewFloat(texWeather[0]->uav.get(), empty);
		previous = texWeather[0]->srv.get();
	}
	context->GenerateMips(previous);
	cb = nullptr;
	samp = previousSampler.get();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &samp);
	context->CSSetShader(nullptr, nullptr, 0);
	outputs = { previous, patterns };
	outputResources[0].copy_from(outputs.weather);
	outputResources[1].copy_from(outputs.patterns);
	generatedKey = key;

	generatedRevision = textures.revision;
	noiseRevision = weatherMap.GetWeatherNoiseRevision();
	generatedValid = true;
	return true;
}

#define I18N_KEY_PREFIX "feature.physical_sky."

static bool DrawWeatherNoise(const char* label, CirrusNoiseLayer& layer, bool normalizedOutput = true)
{
	if (!ImGui::TreeNode(label))
		return false;
	bool changed = ImGui::DragFloat2(T(TKEY("ndf_input_interval"), "Input interval"), &layer.range.x, 0.01f, -1.f, 1.f);
	changed |= ImGui::DragFloat2(T(TKEY("ndf_output_interval"), "Output interval"), &layer.range.z, 0.01f, 0.f, normalizedOutput ? 1.f : 0.f);
	changed |= ImGui::DragFloat(T(TKEY("ndf_frequency"), "Frequency"), &layer.frequency, 0.05f, 0.f, 64.f);
	changed |= ImGui::DragFloat2(T(TKEY("ndf_offset"), "Offset"), &layer.offset.x, 0.005f);
	const char* slots[] = { "0", "1", "2", "3", T(TKEY("ndf_noise_slot_zero"), "Zero") };
	int slot = static_cast<int>(std::min(layer.noise, 4u));
	if (ImGui::Combo(T(TKEY("ndf_noise_slot"), "Noise slot"), &slot, slots, IM_ARRAYSIZE(slots))) {
		layer.noise = static_cast<uint32_t>(slot);
		changed = true;
	}
	ImGui::TreePop();
	return changed;
}

template <class WeatherState, class DrawState>
static void DrawSceneWeatherState(SceneBlend& weather, const SceneSettingsManager::SettingIdentity& identity, const char* overrideLabel, DrawState draw)
{
	const auto* armed = SceneWidgetInterceptor::GetArmedContext();
	const bool scene = armed && !armed->baseline;
	auto* manager = SceneSettingsManager::GetSingleton();
	std::optional<size_t> entryIndex;
	nlohmann::json value = weather;
	bool editable = true;
	if (scene) {
		entryIndex = manager->FindContextUserEntry(armed->contextId, identity.featureShortName, identity.settingPath, identity.settingKey);
		const auto entries = manager->GetContextEntries(armed->contextId);
		const bool active = entryIndex && *entryIndex < entries.size() && !entries[*entryIndex].deleted;
		if (active) {
			value = entries[*entryIndex].value;
			editable = !entries[*entryIndex].paused;
		} else {
			if (Util::Button(overrideLabel)) {
				if (entryIndex)
					manager->ClearContextTombstone(armed->contextId, identity.featureShortName, identity.settingPath, identity.settingKey);
				manager->AddContextSetting(armed->contextId, identity.featureShortName, identity.settingPath, identity.settingKey);
			}
			return;
		}
		if (Util::Button(T(TKEY("local_ndf_scene_inherit"), "Remove override / inherit"))) {
			manager->RemoveContextSetting(armed->contextId, *entryIndex);
			return;
		}
	}
	WeatherState state;
	if (SceneBlend::IsValid(value) && !value["states"].empty()) {
		const auto endpoint = std::max_element(value["states"].begin(), value["states"].end(), [](const auto& lhs, const auto& rhs) {
			return lhs["weight"].template get<float>() < rhs["weight"].template get<float>();
		});
		try {
			state = (*endpoint)["value"].template get<WeatherState>();
		} catch (const nlohmann::json::exception&) {
		}
		if (value["states"].size() > 1)
			ImGui::TextWrapped("%s", T(TKEY("cirrus_edit_mixed"), "A transition is active. Editing captures the strongest endpoint as a single weather state."));
	}
	ImGui::BeginDisabled(!editable);
	const bool changed = draw(state);
	ImGui::EndDisabled();
	if (changed && editable) {
		SceneBlend selection;
		selection.states.push_back({ { "value", state }, { "weight", 1.f } });
		if (scene && entryIndex) {
			const SceneSettingsManager::EntryValueUpdate update{ *entryIndex, selection };
			manager->UpdateContextEntryValues(armed->contextId, { &update, 1 }, true);
		} else {
			weather = selection;
			manager->RecordBaselineEdit(identity, selection);
		}
	}
}

void CloudWeatherManager::DrawLayout(CloudWeatherSettings& settings, TextureManager& textures)
{
	if (!ImGui::TreeNode(T(TKEY("weather_cloud_layout"), "Cloud distribution")))
		return;
	const SceneSettingsManager::SettingIdentity identity{ "PhysicalSky", { "cloudWeather" }, "layout" };
	DrawSceneWeatherState<CloudWeatherLayout>(settings.layout, identity, T(TKEY("weather_cloud_layout_override"), "Override cloud distribution in this scene"),
		[&](CloudWeatherLayout& layout) {
			bool changed = ImGui::InputScalar(T(TKEY("weather_cloud_base_seed"), "Low cloud pattern seed"), ImGuiDataType_U32, &layout.baseSeed);
			changed |= ImGui::SliderInt2(T(TKEY("weather_cloud_base_cells"), "Low cloud cells across map (X / Y)"), layout.baseCells.data(), 1, 64);
			changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_base_edge"), "Low cloud coverage transition"), &layout.baseEdge, .01f, 1.f);
			if (ImGui::TreeNode(T(TKEY("weather_cloud_coverage_mix"), "Low cloud noise weights"))) {
				changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_system_weight"), "Weather systems"), &layout.coverageWeights[0], 0.f, 1.f);
				changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_cluster_weight"), "Cloud clusters"), &layout.coverageWeights[1], 0.f, 1.f);
				changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_fragment_weight"), "Small fragments"), &layout.coverageWeights[2], 0.f, 1.f);
				changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_secondary_coverage"), "Secondary cloud patches"), &layout.secondaryCoverage, 0.f, 1.f);
				Util::Text::WrappedSecondary("%s", T(TKEY("weather_cloud_coverage_mix_hint"), "Relative weights for procedural noise inputs 0, 1 and 3. Secondary patches can fill gaps in the primary coverage."));
				ImGui::TreePop();
			}
			changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_regional_coverage"), "Regional coverage contrast"), &layout.regionalCoverageStrength, 0.f, 1.f);
			if (auto tooltip = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("weather_cloud_regional_coverage_desc"), "Multiplies the generated low cloud coverage by a broad field. Clear regions lose cover and cloudy regions gain cover."));
			changed |= ImGui::SliderInt(T(TKEY("weather_cloud_regional_cells"), "Cloud regions across map"), &layout.regionalCells, 2, 8);
			changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_updraft"), "Concentrate growth in updrafts"), &layout.updraftOrganization, 0.f, 1.f);
			changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_cellular"), "Stratocumulus organization"), &layout.cellular, 0.f, 1.f);
			changed |= ImGui::InputScalar(T(TKEY("weather_cloud_storm_seed"), "Cumulonimbus pattern seed"), ImGuiDataType_U32, &layout.stormSeed);
			changed |= ImGui::SliderInt2(T(TKEY("weather_cloud_storm_cells"), "Cumulonimbus cells across map (X / Y)"), layout.stormCells.data(), 1, 64);
			if (auto tooltip = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("weather_cloud_storm_cells_desc"), "1 × 1 generates one main cell and lower clouds around it. Larger grids distribute storm candidates within the same finite weather region."));
			changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_storm_edge"), "Cumulonimbus coverage transition"), &layout.stormEdge, .01f, 1.f);
			changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_core_fraction"), "Cumulonimbus core width"), &layout.coreFraction, 0.f, 1.f);
			changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_storm_outflow"), "Anvil outflow reach"), &layout.stormOutflow, 0.f, 1.f);
			if (auto tooltip = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("weather_cloud_storm_outflow_desc"), "Spreads the anvil along the upper wind relative to the low wind. Equal winds produce an outflow around the core."));
			changed |= ImGui::InputText(T(TKEY("weather_cloud_painted_map"), "Painted weather map (DDS)"), &layout.paintedMap);
			changed |= ImGui::SliderFloat(T(TKEY("weather_cloud_painted_weight"), "Painted map influence"), &layout.paintedWeight, 0.f, 1.f);
			Util::Text::WrappedSecondary("%s", T(TKEY("weather_cloud_painted_map_hint"), "Linear RGBA DDS. Red and green are low cloud coverage and type. Use a floating-point DDS to paint negative green values for nimbostratus. Blue and alpha are cumulonimbus coverage and type. At full influence, the texture defines the entire map."));
			textures.EnsureLoaded(layout.paintedMap);
			if (!layout.paintedMap.empty() && !IsLinearTexture(textures.Query(layout.paintedMap), 4))
				Util::Text::WrappedWarning("%s", T(TKEY("weather_cloud_painted_map_missing"), "Missing or incompatible RGBA DDS. Using procedural coverage and cloud types."));
			return changed;
		});
	ImGui::TreePop();
}

template <bool middle>
static void DrawLayerWeatherState(SceneBlend& weather, TextureManager& textures)
{
	using WeatherState = std::conditional_t<middle, MidCloudWeatherState, CirrusWeatherState>;
	if (!ImGui::TreeNode(middle ? T(TKEY("middle_weather"), "Middle cloud weather") : T(TKEY("cirrus_weather_settings"), "Cirrus weather")))
		return;
	if constexpr (middle)
		globals::features::physicalSky.DrawCloudWeatherPreview(true);
	if constexpr (!middle) {
		ImGui::TextWrapped("%s", T(TKEY("cirrus_scene_hint"), "Each scene stores coverage, type and an optional local map. Weather and time transitions blend the generated maps, including changes of noise input or local texture."));
		ImGui::TextWrapped("%s", T(TKEY("cirrus_shared_noise"), "Uses the four inputs under Shared Noise & Textures > Weather Noise."));
	}
	const SceneSettingsManager::SettingIdentity identity{ "PhysicalSky", { "cloudLayer", middle ? "middle" : "cirrus" }, "weather" };
	DrawSceneWeatherState<WeatherState>(weather, identity,
		middle ? T(TKEY("middle_scene_override"), "Override middle cloud weather in this scene") : T(TKEY("cirrus_scene_override"), "Override cirrus weather in this scene"),
		[&](WeatherState& state) {
			bool changed = DrawWeatherNoise(middle ? T(TKEY("ndf_primary_coverage"), "Primary coverage") : T(TKEY("cirrus_coverage"), "Cirrus Coverage"), state.layers[0], !middle);
			if constexpr (middle) {
				changed |= DrawWeatherNoise(T(TKEY("ndf_secondary_coverage"), "Secondary coverage"), state.secondaryCoverage, false);
				changed |= DrawWeatherNoise(T(TKEY("ndf_coverage_gain"), "Coverage gain"), state.coverageGain, false);
				changed |= DrawWeatherNoise(T(TKEY("middle_thickness_variation"), "Thickness variation (fraction of layer height)"), state.thicknessVariation, false);
			}
			changed |= DrawWeatherNoise(middle ? T(TKEY("middle_type"), "Cloud Type (As to Ac)") : T(TKEY("cirrus_type"), "Cirrus Type"), state.layers[1]);
			changed |= ImGui::InputText(T(TKEY("cirrus_local_map"), "Local coverage / type / mask RGB"), &state.localMap);
			if constexpr (!middle)
				ImGui::TextWrapped("%s", T(TKEY("cirrus_local_hint"), "Optional linear RGB DDS: R coverage, G type, B blend mask. Repeats every 16384 m multiplied by Cirrus UV Size. Leave the path empty to use no local map."));
			changed |= ImGui::SliderFloat(T(TKEY("cirrus_local_weight"), "Local map influence"), &state.localWeight, 0.f, 1.f);
			if (!middle)
				changed |= ImGui::SliderFloat(T(TKEY("cirrus_local_wind"), "Local map wind scale"), &state.localWindScale, 0.f, 16.f);
			const char* modes[] = { T(TKEY("local_ndf_interpolate"), "Interpolate"), T(TKEY("local_ndf_maximum"), "Maximum") };
			int mode = static_cast<int>(std::min(state.localBlendMode, 1u));
			if (ImGui::Combo(T(TKEY("cirrus_local_blend"), "Local map blend"), &mode, modes, 2)) {
				state.localBlendMode = static_cast<uint32_t>(mode);
				changed = true;
			}
			textures.EnsureLoaded(state.localMap);
			if (!state.localMap.empty() && !CloudWeatherManager::IsLinearTexture(textures.Query(state.localMap), 3))
				Util::Text::WrappedWarning("%s", T(TKEY("cirrus_local_missing"), "Missing or incompatible local map; using the generated coverage and type."));
			return changed;
		});
	ImGui::TreePop();
}

void CirrusMapManager::DrawSettings(CirrusSettings& cirrus, CloudLightingSettings& lighting, TextureManager& textures)
{
	ImGui::Checkbox(T(TKEY("enable_cirrus"), "Enable Cirrus"), &cirrus.enabled);
	ImGui::SliderFloat(T(TKEY("cirrus_density_scale"), "Cirrus Density Scale"), &cirrus.densityScale, 0.f, 4.f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("cloud_layer_fade_hint"), "Set density to zero to hide this layer in a scene. Density blends smoothly between weathers; keep the layer enabled in the global settings."));
	ImGui::SliderFloat(T(TKEY("cirrus_altitude"), "Cirrus Altitude"), &cirrus.altitude, 1.f, 24000.f, "%.0f m");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("cirrus_altitude_hint"), "Height controls the cloud sheet and its atmospheric lighting. It does not extend the low-cloud ray march."));
	if (!SceneWidgetInterceptor::IsArmed()) {
		textures.EnsureLoaded("Data/Textures/PhysicalSky/cirrus.dds");
		if (!CloudWeatherManager::IsLinearTexture(textures.Query("Data/Textures/PhysicalSky/cirrus.dds"), 3))
			Util::Text::WrappedWarning("%s", T(TKEY("cirrus_patterns_missing"), "Cirrus requires a linear RGB cirrus.dds. If it is missing or incompatible, only cirrus is disabled."));
	}
	DrawLayerWeatherState<false>(cirrus.weather, textures);
	if (ImGui::TreeNode(T(TKEY("cloud_cirrus_phase"), "Cirrus Phase and Scattering"))) {
		ImGui::TextWrapped("%s", T(TKEY("cloud_cirrus_phase_desc"), "Cirrus uses an equally weighted, normalized dual-HG phase approximation. Positive G favors forward scattering; negative G favors backward scattering. These parameters affect only cirrus."));
		ImGui::SliderFloat(T(TKEY("cloud_phase_forward"), "Cirrus Forward Phase G"), &lighting.phaseForwardG, 0.f, 0.95f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_phase_backward"), "Cirrus Backward Phase G"), &lighting.phaseBackwardG, -0.95f, 0.f, "%.3f");
		ImGui::SliderFloat(T(TKEY("cloud_cirrus_light_thickness"), "Cirrus Lighting Thickness (m)"), &lighting.cirrusLightThickness, 1.f, 1000.f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("cloud_cirrus_thickness_desc"), "Effective geometric thickness used to estimate the sunlight path through cirrus and its attenuation, especially at grazing angles."));
		ImGui::TreePop();
	}
	if (!SceneWidgetInterceptor::IsArmed() && ImGui::TreeNode(T(TKEY("cirrus_pattern_settings"), "Pattern Scale & Texture"))) {
		ImGui::SliderFloat(T(TKEY("cirrus_uv_size"), "Cirrus UV Size"), &cirrus.uvSize, 0.1f, 32.f, "%.2f x", ImGuiSliderFlags_Logarithmic);
		Util::Text::WrappedSecondary("%s", T(TKEY("cirrus_uv_size_hint"), "Larger values enlarge both weather and detail patterns without changing wind speed. At 1x, weather repeats every 16.384 km and details every 4.29 km. This is a global setting; scenes cannot override it."));
		ImGui::TextUnformatted("Data/Textures/PhysicalSky/cirrus.dds");  // i18n-ignore: file path
		ImGui::TreePop();
	}
}

void MidCloudMapManager::DrawSettings(MidCloudSettings& settings, TextureManager& textures)
{
	ImGui::Checkbox(T(TKEY("middle_enabled"), "Enable middle clouds"), &settings.enabled);
	ImGui::SliderFloat(T(TKEY("middle_density"), "Middle cloud density"), &settings.densityScale, 0.f, 0.1f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("cloud_layer_fade_hint"), "Set density to zero to hide this layer in a scene. Density blends smoothly between weathers; keep the layer enabled in the global settings."));
	ImGui::SliderFloat(T(TKEY("middle_altitude"), "Middle cloud base"), &settings.altitude, 100.f, 16000.f, "%.0f m");
	ImGui::SliderFloat(T(TKEY("middle_thickness"), "Middle cloud height range"), &settings.thickness, 100.f, 4000.f, "%.0f m");
	ImGui::SliderFloat(T(TKEY("middle_size"), "Weather Size"), &settings.uvSize, 0.1f, 8.f, "%.2f x");
	DrawLayerWeatherState<true>(settings.weather, textures);
	if (ImGui::TreeNode(T(TKEY("middle_shape_detail"), "Shape & Detail"))) {
		ImGui::SliderFloat(T(TKEY("middle_softness"), "Top and bottom softness"), &settings.edgeSoftness, 0.01f, 0.5f);
		ImGui::SliderFloat(T(TKEY("middle_cell_frequency"), "Cloud cell frequency"), &settings.cellFrequency, 0.25f, 16.f);
		ImGui::SliderFloat(T(TKEY("middle_ac_cells"), "Ac cloud cell strength"), &settings.cellStrength, 0.f, 1.f);
		ImGui::SliderFloat(T(TKEY("middle_as_cells"), "As cloud cell strength"), &settings.stratusCellStrength, 0.f, 1.f);
		ImGui::SliderFloat(T(TKEY("middle_warp"), "Cloud cell distortion"), &settings.warpStrength, 0.f, 0.5f);
		const char* detailModes[] = { T(TKEY("middle_detail_2d"), "2D surface"), T(TKEY("middle_detail_3d"), "3D volume") };
		int detailMode = static_cast<int>(std::min(settings.detailMode, 1u));
		if (ImGui::Combo(T(TKEY("middle_detail_mode"), "Detail representation"), &detailMode, detailModes, IM_ARRAYSIZE(detailModes)))
			settings.detailMode = static_cast<uint32_t>(detailMode);
		ImGui::SliderFloat(T(TKEY("middle_detail_scale"), "Detail texture size"), &settings.detailScale, 200.f, 4000.f, "%.0f m");
		Util::Text::WrappedSecondary("%s", T(TKEY("middle_detail_hint"), "Final density uses the shared Billow and Wisp noises: As selects Wisp, Ac selects Billow. 3D shapes the volume; 2D uses a horizontal noise slice. Detail size is independent of Weather Size. Both modes follow the upper-cloud wind."));
		ImGui::SliderFloat(T(TKEY("middle_height_power"), "Coverage to thickness response"), &settings.heightPower, 0.25f, 4.f);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Bands", T(TKEY("middle_bands"), "Cloud bands"))) {  // i18n-ignore: ImGui ID
		ImGui::SliderFloat(T(TKEY("middle_band_direction"), "Band direction"), &settings.bands.direction, 0.f, 180.f, "%.0f deg");
		ImGui::SliderFloat(T(TKEY("middle_band_wavelength"), "Band spacing"), &settings.bands.wavelength, 500.f, 16000.f, "%.0f m");
		ImGui::SliderFloat(T(TKEY("middle_band_warp"), "Band bend (wavelengths)"), &settings.bands.warp, 0.f, 1.f);
		ImGui::SliderFloat(T(TKEY("middle_band_coverage"), "Band organization"), &settings.bands.coverage, 0.f, 1.f);
		ImGui::SliderFloat(T(TKEY("middle_band_thickness"), "Thickness contrast"), &settings.bands.thickness, 0.f, 1.f);
		ImGui::SliderFloat(T(TKEY("middle_band_displacement"), "Layer undulation"), &settings.bands.displacement, 0.f, .5f);
		Util::Text::WrappedSecondary("%s", T(TKEY("middle_band_hint"), "Spacing is the nominal world distance between bands, independent of Weather Size. Local weather and cloud cells vary band width and allow breaks or merged regions. Bend smoothly shifts neighboring wavefronts in world space, measured in wavelengths. Billow and Wisp shape the final volume. Thickness contrast and layer undulation also work without coverage gaps; undulation is a fraction of layer thickness."));
		ImGui::TreePop();
	}
}

#undef I18N_KEY_PREFIX
