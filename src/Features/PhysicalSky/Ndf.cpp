#include "Features/PhysicalSky.h"

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

bool TextureManager::LoadTexture(std::filesystem::path path)
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	auto path_str = path.string();
	if (!texList.contains(path_str))
		texList.emplace(path_str, nullptr);

	winrt::com_ptr<ID3D11ShaderResourceView> loaded;
	if (FAILED(DirectX::CreateDDSTextureFromFile(device, context, path.wstring().c_str(), nullptr, loaded.put())))
		return false;
	texList.at(path_str) = std::move(loaded);
	++revision;
	return true;
}

void TextureManager::DrawUI()
{
	ImGui::InputText(T(TKEY("path"), "Path"), &uiPath, 0);
	if (ImGui::Button(T(TKEY("load"), "Load"))) {
		LoadTexture(uiPath);
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("remove"), "Remove"))) {
		if (texList.erase(uiPath))
			++revision;
	}

	if (ImGui::BeginListBox(T(TKEY("loaded_textures"), "Loaded Textures"))) {
		for (auto& [path, srv] : texList) {
			if (ImGui::Selectable(path.c_str(), uiPath == path))
				uiPath = path;
		}
		ImGui::EndListBox();
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
			if (!v.LoadTexture(tex))
				logger::warn("Loading texture manager from config: Texture {} missing.", tex);
	}
}

namespace
{
	eastl::unique_ptr<Texture2D> CreateNdfTexture(uint32_t dimension, DXGI_FORMAT format, const char* name, bool mips = false)
	{
		D3D11_TEXTURE2D_DESC desc{
			.Width = dimension,
			.Height = dimension,
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

	void SanitizeRange(float4& range)
	{
		range.x = FiniteClamp(range.x, -16.f, 16.f, -1.f);
		range.y = FiniteClamp(range.y, -16.f, 16.f, 1.f);
		if (std::abs(range.y - range.x) < 1e-5f)
			range.y = range.x + 1e-5f;
		range.z = FiniteClamp(range.z, -16.f, 16.f, 0.f);
		range.w = FiniteClamp(range.w, -16.f, 16.f, 1.f);
	}
}

void NdfManager::SetupResources()
{
	generatorCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<NdfGenerationParameters>());
	noiseCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<NdfNoiseParameters>());
	texHeight = CreateNdfTexture(kNdfDim, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::NdfHeight");
	texModeling = CreateNdfTexture(kNdfDim, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::NdfModeling");
	for (auto& texture : noiseTextures)
		texture = CreateNdfTexture(kNdfDim, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::NdfNoise", true);
	texOccupancy = CreateNdfTexture(64, DXGI_FORMAT_R32_FLOAT, "PhysicalSky::CloudOccupancy");
	texDistance = CreateNdfTexture(64, DXGI_FORMAT_R32_FLOAT, "PhysicalSky::CloudDistance");
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
	CompileShaders();
}

void NdfManager::CompileShaders()
{
	generatedValid = false;
	accelerationValid = false;
	noiseValid.fill(false);
	generatorProgram = nullptr;
	noiseProgram = nullptr;
	occupancyProgram = nullptr;
	distanceProgram = nullptr;
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfGenerate.cs.hlsl", {}, "cs_5_0")))
		generatorProgram.attach(raw);
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfNoise.cs.hlsl", {}, "cs_5_0")))
		noiseProgram.attach(raw);
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfAcceleration.cs.hlsl", {}, "cs_5_0", "buildOccupancy")))
		occupancyProgram.attach(raw);
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfAcceleration.cs.hlsl", {}, "cs_5_0", "buildDistance")))
		distanceProgram.attach(raw);
}

const char* NdfManager::GetSettingsTypeName(const NdfSettings& settings)
{
	return settings.type == NdfType::Texture ?
	           T(TKEY("ndf_type_texture_pair"), "Texture Pair") :
	           T(TKEY("ndf_type_procedural"), "Procedural");
}

const char* NdfManager::GetSettingsHint(const NdfSettings& settings)
{
	return settings.type == NdfType::Texture ?
	           T(TKEY("ndf_hint_texture"),
				   "Two linear 2D textures: height RG = bottom/top, modeling RGB = coverage/top type/bottom type. Heights use the NDF base altitude and height span in Low Clouds.") :
	           T(TKEY("ndf_hint_procedural"),
				   "Two coverage signals and a shared type signal drive the profiles. Height variation changes only the bottom bound. Noise slots can be generated locally or supplied as DDS textures.");
}

void NdfManager::DrawNdfSettings(NdfSettings& settings, TextureManager& textures)
{
	const char* generatorNames[] = {
		T(TKEY("ndf_type_texture_pair"), "Texture Pair"),
		T(TKEY("ndf_type_procedural"), "Procedural")
	};
	int source = settings.type == NdfType::Texture ? 0 : 1;
	if (ImGui::Combo(T(TKEY("cloud_map_generator"), "Cloud Map Generator"), &source, generatorNames, IM_ARRAYSIZE(generatorNames)))
		settings.type = source == 0 ? NdfType::Texture : NdfType::Procedural;
	ImGui::TextWrapped("%s", GetSettingsHint(settings));
	if (ImGui::TreeNode(T(TKEY("ndf_texture_inputs"), "Texture inputs"))) {
		textures.DrawUI();
		ImGui::TreePop();
	}
	const auto textureChoice = [&](const char* label, std::string& path, uint32_t channels, const char* emptyLabel = T(TKEY("ndf_none"), "None")) {
		if (ImGui::BeginCombo(label, path.empty() ? emptyLabel : path.c_str())) {
			if (ImGui::Selectable(emptyLabel, path.empty()))
				path.clear();
			for (const auto& choice : textures.ListPaths())
				if (ImGui::Selectable(choice.c_str(), path == choice))
					path = choice;
			ImGui::EndCombo();
		}
		if (!path.empty() && !IsTextureNdf(textures.Query(path), channels))
			ImGui::TextColored({ 1, 0.3f, 0.2f, 1 }, "%s", T(TKEY("ndf_incompatible_texture"), "Missing or incompatible linear 2D texture."));
	};
	if (settings.type == NdfType::Texture) {
		textureChoice(T(TKEY("ndf_height_rg"), "Height RG"), settings.texture.heightPath, 2);
		textureChoice(T(TKEY("ndf_modeling_rgb"), "Modeling RGB"), settings.texture.modelingPath, 3);
		if (settings.texture.heightPath.empty() || settings.texture.modelingPath.empty())
			ImGui::TextWrapped("%s", T(TKEY("ndf_select_both_textures"), "Select both height and modeling textures."));
		return;
	}
	auto& procedural = settings.procedural;
	auto& parameters = procedural.parameters;
	const auto rangeControl = [](const char* label, float4& range) {
		ImGui::PushID(label);
		ImGui::TextUnformatted(label);
		ImGui::DragFloat2(T(TKEY("ndf_input_interval"), "Input interval"), &range.x, 0.01f, -16.f, 16.f);
		ImGui::DragFloat2(T(TKEY("ndf_output_interval"), "Output interval"), &range.z, 0.01f, -16.f, 16.f);
		ImGui::PopID();
	};
	const auto layerControl = [&](const char* label, NdfNoiseLayer& layer, bool power) {
		if (!ImGui::TreeNode(label))
			return;
		const char* slotNames[] = { "0", "1", "2", "3", T(TKEY("ndf_noise_slot_zero"), "Zero") };
		int slot = static_cast<int>(std::min(layer.noise, 4u));
		if (ImGui::Combo(T(TKEY("ndf_noise_slot"), "Noise slot"), &slot, slotNames, IM_ARRAYSIZE(slotNames)))
			layer.noise = static_cast<uint32_t>(slot);
		ImGui::DragFloat(T(TKEY("ndf_frequency"), "Frequency"), &layer.frequency, 0.05f, 0.f, 64.f);
		ImGui::DragFloat2(T(TKEY("ndf_offset"), "Offset"), &layer.offset.x, 0.005f);
		if (power)
			ImGui::SliderFloat(T(TKEY("ndf_exponent"), "Exponent"), &layer.exponent, 0.01f, 8.f);
		rangeControl(T(TKEY("ndf_remap"), "Remap"), layer.range);
		ImGui::TreePop();
	};
	layerControl(T(TKEY("ndf_primary_coverage"), "Primary coverage"), parameters.primary, true);
	layerControl(T(TKEY("ndf_secondary_coverage"), "Secondary coverage"), parameters.secondary, true);
	layerControl(T(TKEY("ndf_coverage_gain"), "Coverage gain"), parameters.coverageGain, false);
	layerControl(T(TKEY("ndf_top_type_noise"), "Top type / shared type noise"), parameters.modeling, true);
	if (ImGui::TreeNode(T(TKEY("ndf_bottom_type"), "Bottom type"))) {
		rangeControl(T(TKEY("ndf_remap"), "Remap"), parameters.bottomTypeRange);
		ImGui::SliderFloat(T(TKEY("ndf_exponent"), "Exponent"), &parameters.bottomTypeExponent, 0.01f, 8.f);
		ImGui::TreePop();
	}
	layerControl(T(TKEY("ndf_type_gain"), "Type gain"), parameters.modelingGain, false);
	ImGui::SliderFloat2(T(TKEY("ndf_base_height_rg"), "Base height RG"), &parameters.baseHeight.x, 0.f, 1.f);
	bool fromCoverage = parameters.heightFromCoverage != 0u;
	if (ImGui::Checkbox(T(TKEY("ndf_height_from_coverage"), "Height variation from coverage"), &fromCoverage))
		parameters.heightFromCoverage = fromCoverage ? 1u : 0u;
	layerControl(T(TKEY("ndf_bottom_height_variation"), "Bottom height variation"), parameters.heightVariation, true);
	ImGui::DragFloat2(T(TKEY("ndf_weather_offset"), "Weather offset (m)"), &parameters.windOffset.x, 1.f);
	if (ImGui::TreeNode(T(TKEY("ndf_local_influence"), "Local NDF influence"))) {
		textureChoice(T(TKEY("ndf_height_rg"), "Height RG"), procedural.local.heightPath, 2);
		textureChoice(T(TKEY("ndf_modeling_rgb"), "Modeling RGB"), procedural.local.modelingPath, 3);
		textureChoice(T(TKEY("ndf_influence_mask"), "Influence mask R (optional)"), procedural.localMaskPath, 1);
		const char* blendModes[] = {
			T(TKEY("ndf_blend_interpolate"), "Interpolate"),
			T(TKEY("ndf_blend_maximum"), "Maximum")
		};
		int mode = parameters.localBlendMode == 0u ? 0 : 1;
		if (ImGui::Combo(T(TKEY("ndf_modeling_blend"), "Modeling blend"), &mode, blendModes, IM_ARRAYSIZE(blendModes)))
			parameters.localBlendMode = static_cast<uint32_t>(mode);
		ImGui::SliderFloat(T(TKEY("ndf_modeling_weight"), "Modeling weight"), &parameters.localModelingWeight, 0.f, 1.f);
		ImGui::SliderFloat(T(TKEY("ndf_height_weight"), "Height weight"), &parameters.localHeightWeight, 0.f, 1.f);
		ImGui::DragFloat(T(TKEY("ndf_local_offset_multiplier"), "Local offset multiplier"), &parameters.localWindScale, 0.01f, -4.f, 4.f);
		ImGui::TreePop();
	}
	for (uint32_t index = 0; index < 4; ++index) {
		ImGui::PushID(static_cast<int>(index));
		if (ImGui::TreeNode("Noise", T(TKEY("ndf_noise_input"), "Noise input %u"), index)) {
			auto& input = procedural.noise[index];
			textureChoice(T(TKEY("ndf_dds_override"), "DDS override"), input.texturePath, 1, T(TKEY("ndf_generated"), "Generated"));
			auto& noise = input.parameters;
			if (ImGui::Button(T(TKEY("ndf_reset_noise_parameters"), "Reset noise parameters")))
				noise = ProceduralNdfSettings{}.noise[index].parameters;
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
			const uint32_t maxFrequency = std::min(128u, kNdfDim / (2u * std::clamp(noise.repetitions, 1u, maxRepetitions)));
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

bool NdfManager::UpdateNdf(const NdfSettings& settings, TextureManager& textures)
{
	std::array<std::string, 9> paths = {};
	if (settings.type == NdfType::Texture) {
		paths[0] = settings.texture.heightPath;
		paths[1] = settings.texture.modelingPath;
	} else {
		for (uint32_t i = 0; i < 4; ++i)
			paths[i + 2] = settings.procedural.noise[i].texturePath;
		paths[6] = settings.procedural.local.heightPath;
		paths[7] = settings.procedural.local.modelingPath;
		paths[8] = settings.procedural.localMaskPath;
	}
	for (size_t i = 0; i < paths.size(); ++i)
		if (paths[i] != sourcePaths[i] && !paths[i].empty() && !textures.texList.contains(paths[i]))
			if (!textures.LoadTexture(paths[i]))
				logger::warn("NDF input could not be loaded: {}", paths[i]);
	sourcePaths = std::move(paths);
	if (settings.type == NdfType::Texture) {
		const bool changed = importedRevision != textures.revision;
		importedRevision = textures.revision;
		return changed;
	}
	if (!generatorProgram || !noiseProgram || !generatorCb || !noiseCb || !sampler || !texHeight || !texModeling)
		return false;
	auto* context = globals::d3d::context;
	std::array<ID3D11ShaderResourceView*, 7> sources = {};
	const auto& procedural = settings.procedural;
	for (uint32_t i = 0; i < 4; ++i) {
		const auto& input = procedural.noise[i];
		if (!input.texturePath.empty()) {
			sources[i] = textures.Query(input.texturePath);
			if (!IsTextureNdf(sources[i], 1)) {
				generatedValid = false;
				return false;
			}
			continue;
		}
		auto data = input.parameters;
		data.type = std::min(data.type, 2u);
		data.repetitions = std::clamp(data.repetitions, 1u, 8u);
		data.frequency = std::clamp(data.frequency, 1u, std::min(128u, kNdfDim / (2u * data.repetitions)));
		data.octaves = std::clamp(data.octaves, 1u, 8u);
		data.lacunarity = std::clamp(data.lacunarity, 1u, 4u);
		data.persistence = FiniteClamp(data.persistence, 0.f, 1.f, 0.5f);
		data.contrast = FiniteClamp(data.contrast, 0.1f, 8.f, 1.f);
		data.bias = FiniteClamp(data.bias, -1.f, 1.f, 0.f);
		data.responseExponent = FiniteClamp(data.responseExponent, 0.1f, 8.f, 1.f);
		data.padding = {};
		if (!noiseValid[i] || std::memcmp(&generatedNoise[i], &data, sizeof(data)) != 0) {
			noiseCb->Update(data);
			auto* cb = noiseCb->CB();
			auto* uav = noiseTextures[i]->uav.get();
			context->CSSetConstantBuffers(1, 1, &cb);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(noiseProgram.get(), nullptr, 0);
			context->Dispatch((kNdfDim + 7u) >> 3, (kNdfDim + 7u) >> 3, 1);
			uav = nullptr;
			cb = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetConstantBuffers(1, 1, &cb);
			context->CSSetShader(nullptr, nullptr, 0);
			context->GenerateMips(noiseTextures[i]->srv.get());
			generatedNoise[i] = data;
			noiseValid[i] = true;
			generatedValid = false;
		}
		sources[i] = noiseTextures[i]->srv.get();
	}
	const std::array<std::pair<const std::string*, uint32_t>, 3> localInputs = { { { &procedural.local.modelingPath, 3u }, { &procedural.local.heightPath, 2u }, { &procedural.localMaskPath, 1u } } };
	for (uint32_t i = 0; i < 3; ++i) {
		if (localInputs[i].first->empty())
			continue;
		sources[i + 4] = textures.Query(*localInputs[i].first);
		if (!IsTextureNdf(sources[i + 4], localInputs[i].second)) {
			generatedValid = false;
			return false;
		}
	}
	auto data = procedural.parameters;
	for (auto* layer : { &data.primary, &data.secondary, &data.coverageGain, &data.modeling, &data.modelingGain, &data.heightVariation }) {
		layer->noise = std::min(layer->noise, 4u);
		layer->frequency = FiniteClamp(layer->frequency, 0.f, 64.f, 1.f);
		layer->exponent = FiniteClamp(layer->exponent, 0.01f, 8.f, 1.f);
		layer->offset.x = FiniteClamp(layer->offset.x, -10000.f, 10000.f, 0.f);
		layer->offset.y = FiniteClamp(layer->offset.y, -10000.f, 10000.f, 0.f);
		layer->padding0 = 0.f;
		layer->padding1 = {};
		SanitizeRange(layer->range);
	}
	SanitizeRange(data.bottomTypeRange);
	data.baseHeight.x = FiniteClamp(data.baseHeight.x, 0.f, 1.f, 0.f);
	data.baseHeight.y = FiniteClamp(data.baseHeight.y, 0.f, 1.f, 0.95f);
	data.bottomTypeExponent = FiniteClamp(data.bottomTypeExponent, 0.01f, 8.f, 1.f);
	data.heightFromCoverage = data.heightFromCoverage != 0u ? 1u : 0u;
	data.localBlendMode = data.localBlendMode != 0u ? 1u : 0u;
	data.localModelingWeight = sources[4] ? FiniteClamp(data.localModelingWeight, 0.f, 1.f, 1.f) : 0.f;
	data.localHeightWeight = sources[5] ? FiniteClamp(data.localHeightWeight, 0.f, 1.f, 1.f) : 0.f;
	data.localWindScale = FiniteClamp(data.localWindScale, -4.f, 4.f, 1.f);
	data.windOffset.x = FiniteClamp(data.windOffset.x, -1e7f, 1e7f, 0.f);
	data.windOffset.y = FiniteClamp(data.windOffset.y, -1e7f, 1e7f, 0.f);
	data.hasLocalMask = sources[6] ? 1u : 0u;
	data.padding = 0.f;
	if (generatedValid && sources == generatedSources && generatedRevision == textures.revision && std::memcmp(&generatedData, &data, sizeof(data)) == 0)
		return false;
	generatorCb->Update(data);
	auto* cb = generatorCb->CB();
	auto* samp = sampler.get();
	winrt::com_ptr<ID3D11SamplerState> previousSampler;
	context->CSGetSamplers(0, 1, previousSampler.put());
	ID3D11UnorderedAccessView* uavs[2] = { texHeight->uav.get(), texModeling->uav.get() };
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &samp);
	context->CSSetShaderResources(0, static_cast<uint32_t>(sources.size()), sources.data());
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->CSSetShader(generatorProgram.get(), nullptr, 0);
	globals::profiler->BeginPass("PhysicalSky::CloudNdf");
	context->Dispatch((kNdfDim + 7u) >> 3, (kNdfDim + 7u) >> 3, 1);
	globals::profiler->EndPass();
	ID3D11ShaderResourceView* nullSrvs[7] = {};
	ID3D11UnorderedAccessView* nullUavs[2] = {};
	context->CSSetShaderResources(0, 7, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);
	samp = previousSampler.get();
	context->CSSetSamplers(0, 1, &samp);
	generatedData = data;
	generatedSources = sources;
	generatedRevision = textures.revision;
	generatedValid = true;
	accelerationValid = false;
	return true;
}

void NdfManager::UpdateAcceleration(const NdfSettings& settings, TextureManager& textures)
{
	const auto maps = GetNdf(settings, textures);
	auto* model = maps.modeling;
	if (settings.type == NdfType::Procedural && accelerationValid && acceleratedNdf == model)
		return;
	accelerationValid = false;
	if (!maps || !occupancyProgram || !distanceProgram || !texOccupancy || !texDistance)
		return;
	auto* context = globals::d3d::context;
	auto* uav = texOccupancy->uav.get();
	context->CSSetShaderResources(0, 1, &model);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(occupancyProgram.get(), nullptr, 0);
	globals::profiler->BeginPass("PhysicalSky::CloudAcceleration");
	context->Dispatch(8, 8, 1);
	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	auto* occupancy = texOccupancy->srv.get();
	context->CSSetShaderResources(1, 1, &occupancy);
	uav = texDistance->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(distanceProgram.get(), nullptr, 0);
	context->Dispatch(8, 8, 1);
	globals::profiler->EndPass();
	ID3D11ShaderResourceView* nullSrvs[2] = {};
	context->CSSetShaderResources(0, 2, nullSrvs);
	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	accelerationValid = true;
	acceleratedNdf = model;
}

bool NdfManager::IsTextureNdf(ID3D11ShaderResourceView* srv, uint32_t channels)
{
	if (!srv)
		return false;
	D3D11_SHADER_RESOURCE_VIEW_DESC desc;
	srv->GetDesc(&desc);
	if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
		return false;
	switch (desc.Format) {
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
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32_FLOAT:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC7_UNORM:
		return true;
	default:
		return false;
	}
}

NdfTextureSet NdfManager::QueryTextures(const TexNdfSettings& settings, TextureManager& textures)
{
	NdfTextureSet maps{ textures.Query(settings.heightPath), textures.Query(settings.modelingPath) };
	return IsTextureNdf(maps.height, 2) && IsTextureNdf(maps.modeling, 3) ? maps : NdfTextureSet{};
}

NdfTextureSet NdfManager::GetNdf(const NdfSettings& settings, TextureManager& textures)
{
	if (settings.type == NdfType::Texture)
		return QueryTextures(settings.texture, textures);
	return generatedValid && texHeight && texModeling ? NdfTextureSet{ texHeight->srv.get(), texModeling->srv.get() } : NdfTextureSet{};
}

namespace
{
	constexpr uint32_t kHistogramBins = 256u;
	constexpr uint32_t kHistogramGroups = 2u;
	constexpr uint32_t kThresholdCount = 2u;

	void HashCombine(size_t& seed, size_t value)
	{
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
	}

	template <class T>
	void HashValue(size_t& seed, const T& value)
	{
		HashCombine(seed, std::hash<T>{}(value));
	}

	eastl::unique_ptr<Texture2D> CreateCloudMapTexture(uint32_t width, uint32_t height, const char* name)
	{
		D3D11_TEXTURE2D_DESC desc{
			.Width = width,
			.Height = height,
			.MipLevels = 0,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS
		};
		auto texture = eastl::make_unique<Texture2D>(desc, name);
		texture->CreateSRV({
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = static_cast<UINT>(-1) },
		});
		texture->CreateUAV({
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 },
		});
		return texture;
	}

	eastl::unique_ptr<Texture2D> CreateFieldTexture(uint32_t width, uint32_t height, const char* name)
	{
		D3D11_TEXTURE2D_DESC desc{
			.Width = width,
			.Height = height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		auto texture = eastl::make_unique<Texture2D>(desc, name);
		texture->CreateSRV({
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 },
		});
		texture->CreateUAV({
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 },
		});
		return texture;
	}
}

void HighCloudMapManager::SetupResources()
{
	cbGen = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<GenCB>(), "PhysicalSky::HighCloudMapGenCB");

	D3D11_BUFFER_DESC histogramDesc{
		.ByteWidth = kHistogramBins * kHistogramGroups * sizeof(uint32_t),
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
		.CPUAccessFlags = 0,
		.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
		.StructureByteStride = 0
	};
	bufHistogram = eastl::make_unique<Buffer>(histogramDesc, nullptr, "PhysicalSky::HighCloudMapHistogram");
	bufHistogram->CreateUAV({
		.Format = DXGI_FORMAT_R32_TYPELESS,
		.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
		.Buffer = { .FirstElement = 0, .NumElements = kHistogramBins * kHistogramGroups, .Flags = D3D11_BUFFER_UAV_FLAG_RAW },
	});

	D3D11_BUFFER_DESC thresholdDesc{
		.ByteWidth = kThresholdCount * sizeof(float),
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		.CPUAccessFlags = 0,
		.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
		.StructureByteStride = sizeof(float)
	};
	bufThresholds = eastl::make_unique<Buffer>(thresholdDesc, nullptr, "PhysicalSky::HighCloudMapThresholds");
	bufThresholds->CreateSRV({
		.Format = DXGI_FORMAT_UNKNOWN,
		.ViewDimension = D3D11_SRV_DIMENSION_BUFFER,
		.Buffer = { .FirstElement = 0, .NumElements = kThresholdCount },
	});
	bufThresholds->CreateUAV({
		.Format = DXGI_FORMAT_UNKNOWN,
		.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
		.Buffer = { .FirstElement = 0, .NumElements = kThresholdCount, .Flags = 0 },
	});

	CompileShaders();
}

void HighCloudMapManager::CompileShaders()
{
	struct PassInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* target;
		const char* mode;
	};
	const std::array passes = {
		PassInfo{ &csFields, "0" },
		PassInfo{ &csHistogram, "1" },
		PassInfo{ &csSolve, "2" },
		PassInfo{ &csCompose, "3" },
	};
	const auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky\\HighCloudMapGen.cs.hlsl");
	for (const auto& pass : passes) {
		*pass.target = nullptr;
		std::vector<std::pair<const char*, const char*>> defines{ { "HIGHCLOUDMAPGEN", pass.mode } };
		if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), defines, "cs_5_0", "main")))
			pass.target->attach(raw);
	}
	generatedHash = 0;
}

bool HighCloudMapManager::ShadersReady() const
{
	return csFields && csHistogram && csSolve && csCompose;
}

bool HighCloudMapManager::EnsureResources(const HighCloudSettings& settings)
{
	const uint32_t dimension = std::clamp(settings.weatherDim, 128u, 1024u);
	if (generatedWeatherDim != dimension || !texHighWeather || !texHighCell || !texHighWarp || !texHighWisp || !texFieldHigh) {
		texHighWeather = CreateCloudMapTexture(dimension, dimension, "PhysicalSky::HighWeather");
		texHighCell = CreateCloudMapTexture(dimension, dimension, "PhysicalSky::HighCell");
		texHighWarp = CreateCloudMapTexture(dimension, dimension, "PhysicalSky::HighWarp");
		texHighWisp = CreateCloudMapTexture(dimension, dimension, "PhysicalSky::HighWisp");
		texFieldHigh = CreateFieldTexture(dimension, dimension, "PhysicalSky::HighCloudField");
		generatedWeatherDim = dimension;
	}
	return texHighWeather && texHighCell && texHighWarp && texHighWisp && texFieldHigh;
}

void HighCloudMapManager::GenerateTextures(const HighCloudSettings& settings)
{
	if (!ShadersReady() || !cbGen || !bufHistogram || !bufThresholds)
		return;

	size_t hash = 0;
	HashValue(hash, settings.weatherDim);
	HashValue(hash, settings.weatherSeed);
	HashValue(hash, settings.coverage);
	HashValue(hash, settings.coverageEdgeWidth);
	HashValue(hash, settings.frontStrength);
	HashValue(hash, settings.frontBearing);
	HashValue(hash, settings.altostratusWeight);
	HashValue(hash, settings.altocumulusWeight);
	if (hash == 0)
		hash = 1;
	if (hash == generatedHash)
		return;
	if (!EnsureResources(settings))
		return;

	const float bearingRadians = settings.frontBearing * (std::numbers::pi_v<float> / 180.0f);
	const float rawX = std::cos(bearingRadians);
	const float rawY = std::sin(bearingRadians);
	const float dominant = std::max(std::abs(rawX), std::abs(rawY));
	const float normalX = std::round(rawX / std::max(dominant, 1e-3f) * 2.0f);
	const float normalY = std::round(rawY / std::max(dominant, 1e-3f) * 2.0f);
	const float asWeight = std::max(settings.altostratusWeight, 0.0f);
	const float acWeight = std::max(settings.altocumulusWeight, 0.0f);
	const float weightSum = asWeight + acWeight;

	GenCB data{
		.weatherDim = { generatedWeatherDim, generatedWeatherDim },
		.seed = settings.weatherSeed,
		.solveRound = 0u,
		.coverage = std::clamp(settings.coverage, 0.0f, 1.0f),
		.highCoverageEdgeWidth = std::max(settings.coverageEdgeWidth, 0.05f),
		.frontStrength = std::clamp(settings.frontStrength, 0.0f, 1.0f),
		.asShare = weightSum > 1e-5f ? asWeight / weightSum : 1.0f,
		.frontNormal = { normalX, normalY },
		.frontTangent = { -normalY, normalX },
		.padding = { 0.0f, 0.0f, 0.0f, 0.0f },
	};

	auto* context = globals::d3d::context;
	const uint32_t groups = (generatedWeatherDim + 7u) >> 3;
	ID3D11UnorderedAccessView* nullUavs[4] = {};
	ID3D11ShaderResourceView* nullSrvs[2] = {};
	const auto upload = [&](uint32_t round) {
		data.solveRound = round;
		cbGen->Update(data);
		ID3D11Buffer* cb = cbGen->CB();
		context->CSSetConstantBuffers(1, 1, &cb);
	};

	upload(0u);
	{
		ID3D11UnorderedAccessView* uav = texFieldHigh->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csFields.get(), nullptr, 0);
		context->Dispatch(groups, groups, 1);
		context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
	}

	for (uint32_t round = 0u; round < 2u; ++round) {
		upload(round);
		const UINT clearValues[4] = {};
		context->ClearUnorderedAccessViewUint(bufHistogram->uav.get(), clearValues);
		std::array<ID3D11ShaderResourceView*, 2> srvs = { texFieldHigh->srv.get(), bufThresholds->srv.get() };
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		ID3D11UnorderedAccessView* histogram = bufHistogram->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &histogram, nullptr);
		context->CSSetShader(csHistogram.get(), nullptr, 0);
		context->Dispatch(groups, groups, 1);
		context->CSSetShaderResources(0, 2, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

		std::array<ID3D11UnorderedAccessView*, 2> solveUavs = { bufHistogram->uav.get(), bufThresholds->uav.get() };
		context->CSSetUnorderedAccessViews(0, (uint)solveUavs.size(), solveUavs.data(), nullptr);
		context->CSSetShader(csSolve.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);
		context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	}

	{
		std::array<ID3D11ShaderResourceView*, 2> srvs = { texFieldHigh->srv.get(), bufThresholds->srv.get() };
		std::array<ID3D11UnorderedAccessView*, 4> uavs = { texHighWeather->uav.get(), texHighCell->uav.get(), texHighWarp->uav.get(), texHighWisp->uav.get() };
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csCompose.get(), nullptr, 0);
		context->Dispatch(groups, groups, 1);
		context->CSSetShaderResources(0, 2, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 4, nullUavs, nullptr);
	}

	context->CSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(1, 1, &nullCb);
	context->GenerateMips(texHighWeather->srv.get());
	context->GenerateMips(texHighCell->srv.get());
	context->GenerateMips(texHighWarp->srv.get());
	context->GenerateMips(texHighWisp->srv.get());
	generatedHash = hash;
}

HighCloudTextureSet HighCloudMapManager::GetTextures(const HighCloudSettings& settings)
{
	GenerateTextures(settings);
	return {
		.highWeather = texHighWeather ? texHighWeather->srv.get() : nullptr,
		.highCell = texHighCell ? texHighCell->srv.get() : nullptr,
		.highWarp = texHighWarp ? texHighWarp->srv.get() : nullptr,
		.highWisp = texHighWisp ? texHighWisp->srv.get() : nullptr,
	};
}
