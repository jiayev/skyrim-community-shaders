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

void CirrusMapManager::SetupResources()
{
	generationCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<GenerationParameters>());
	noiseCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<NdfNoiseParameters>());
	texWeather = CreateNdfTexture(kDimension, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::CirrusWeather", true);
	texPatterns = CreateNdfTexture(kDimension, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CirrusPatterns", true);
	for (auto& texture : noiseTextures)
		texture = CreateNdfTexture(kDimension, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::CirrusWeatherNoise", true);
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

void CirrusMapManager::CompileShaders()
{
	generatedValid = false;
	noiseValid.fill(false);
	outputs = {};
	weatherProgram = nullptr;
	patternsProgram = nullptr;
	noiseProgram = nullptr;
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\CirrusGenerate.cs.hlsl", {}, "cs_5_0", "generateWeather")))
		weatherProgram.attach(raw);
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\CirrusGenerate.cs.hlsl", {}, "cs_5_0", "generatePatterns")))
		patternsProgram.attach(raw);
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfNoise.cs.hlsl", {}, "cs_5_0")))
		noiseProgram.attach(raw);
}

bool CirrusMapManager::ShadersReady() const
{
	return weatherProgram && patternsProgram && noiseProgram && generationCb && noiseCb && sampler && texWeather && texPatterns;
}

CirrusTextureSet CirrusMapManager::GetTextures() const
{
	return outputs;
}

bool CirrusMapManager::Update(const CirrusSettings& settings, TextureManager& textures)
{
	const bool hadOutputs = bool(outputs);
	const auto fail = [&]() {
		outputs = {};
		generatedValid = false;
		return hadOutputs;
	};
	if (!ShadersReady())
		return fail();
	const std::array<std::string, 4> paths{ settings.noise[0].texturePath, settings.noise[1].texturePath, settings.weatherPath, settings.patternsPath };
	for (size_t i = 0; i < paths.size(); ++i)
		if (paths[i] != sourcePaths[i] && !paths[i].empty() && !textures.texList.contains(paths[i]))
			if (!textures.LoadTexture(paths[i]))
				logger::warn("Cirrus input could not be loaded: {}", paths[i]);
	sourcePaths = paths;
	std::array<ID3D11ShaderResourceView*, 4> sources = {};
	for (uint32_t i = 2; i < 4; ++i) {
		if (paths[i].empty())
			continue;
		sources[i] = textures.Query(paths[i]);
		if (!NdfManager::IsTextureNdf(sources[i], i == 2 ? 2u : 3u))
			return fail();
	}
	auto* context = globals::d3d::context;
	if (!sources[2]) {
		for (uint32_t i = 0; i < 2; ++i) {
			if (!paths[i].empty()) {
				sources[i] = textures.Query(paths[i]);
				if (!NdfManager::IsTextureNdf(sources[i], 1))
					return fail();
				continue;
			}
			auto data = settings.noise[i].parameters;
			data.type = std::min(data.type, 2u);
			data.repetitions = std::clamp(data.repetitions, 1u, 8u);
			data.frequency = std::clamp(data.frequency, 1u, std::min(128u, kDimension / (2u * data.repetitions)));
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
				context->Dispatch((kDimension + 7u) >> 3, (kDimension + 7u) >> 3, 1);
				uav = nullptr;
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				context->GenerateMips(noiseTextures[i]->srv.get());
				cb = nullptr;
				context->CSSetConstantBuffers(1, 1, &cb);
				context->CSSetShader(nullptr, nullptr, 0);
				generatedNoise[i] = data;
				noiseValid[i] = true;
				generatedValid = false;
			}
			sources[i] = noiseTextures[i]->srv.get();
		}
	}
	GenerationParameters data{
		.weather = settings.weather,
		.seed = settings.patternSeed,
		.warp = FiniteClamp(settings.patternWarp, 0.f, 0.5f, 0.15f),
		.detail = FiniteClamp(settings.patternDetail, 0.f, 1.f, 0.35f)
	};
	for (uint32_t i = 0; i < 2; ++i) {
		auto& layer = data.weather[i];
		layer.noise = i;
		layer.frequency = FiniteClamp(layer.frequency, 0.f, 64.f, 1.f);
		layer.exponent = 1.f;
		layer.padding0 = 0.f;
		layer.offset.x = FiniteClamp(layer.offset.x, -10000.f, 10000.f, 0.f);
		layer.offset.y = FiniteClamp(layer.offset.y, -10000.f, 10000.f, 0.f);
		layer.padding1 = {};
		SanitizeRange(layer.range);
	}
	if (generatedValid && generatedSources == sources && generatedRevision == textures.revision && std::memcmp(&data, &generatedData, sizeof(data)) == 0)
		return false;
	generationCb->Update(data);
	auto* cb = generationCb->CB();
	auto* samp = sampler.get();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &samp);
	if (!sources[2]) {
		auto* uav = texWeather->uav.get();
		context->CSSetShaderResources(0, 2, sources.data());
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(weatherProgram.get(), nullptr, 0);
		context->Dispatch((kDimension + 7u) >> 3, (kDimension + 7u) >> 3, 1);
		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->GenerateMips(texWeather->srv.get());
	}
	if (!sources[3]) {
		auto* uav = texPatterns->uav.get();
		context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
		context->CSSetShader(patternsProgram.get(), nullptr, 0);
		context->Dispatch((kDimension + 7u) >> 3, (kDimension + 7u) >> 3, 1);
		uav = nullptr;
		context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
		context->GenerateMips(texPatterns->srv.get());
	}
	ID3D11ShaderResourceView* nullSrvs[2] = {};
	context->CSSetShaderResources(0, 2, nullSrvs);
	cb = nullptr;
	samp = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &samp);
	context->CSSetShader(nullptr, nullptr, 0);
	outputs = { sources[2] ? sources[2] : texWeather->srv.get(), sources[3] ? sources[3] : texPatterns->srv.get() };
	generatedSources = sources;
	generatedData = data;
	generatedRevision = textures.revision;
	generatedValid = true;
	return true;
}

#define I18N_KEY_PREFIX "feature.physical_sky."

void CirrusMapManager::DrawSettings(CirrusSettings& settings, TextureManager& textures)
{
	ImGui::SeparatorText(T(TKEY("cirrus"), "Cirrus"));
	ImGui::Checkbox(T(TKEY("enable_cirrus"), "Enable Cirrus"), &settings.enabled);
	ImGui::SliderFloat(T(TKEY("cirrus_altitude"), "Cirrus Altitude"), &settings.altitude, 1.f, 24000.f, "%.0f m");
	ImGui::SliderFloat(T(TKEY("cirrus_pattern_scale"), "Pattern Repeat Length"), &settings.patternScale, 100.f, 64000.f, "%.0f m", ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat(T(TKEY("cirrus_density_scale"), "Cirrus Density Scale"), &settings.densityScale, 0.f, 4.f, "%.3f");
	ImGui::SliderFloat(T(TKEY("cirrus_lighting_scale"), "Cirrus Lighting Scale"), &settings.lightingScale, 0.f, 4.f, "%.3f");
	const auto textureChoice = [&](const char* label, std::string& path, uint32_t channels) {
		const char* generated = T(TKEY("ndf_type_procedural"), "Procedural");
		if (ImGui::BeginCombo(label, path.empty() ? generated : path.c_str())) {
			if (ImGui::Selectable(generated, path.empty()))
				path.clear();
			for (const auto& choice : textures.ListPaths())
				if (ImGui::Selectable(choice.c_str(), path == choice))
					path = choice;
			ImGui::EndCombo();
		}
		if (!path.empty() && !NdfManager::IsTextureNdf(textures.Query(path), channels))
			ImGui::TextColored({ 1, 0.3f, 0.2f, 1 }, "%s", T(TKEY("ndf_incompatible_texture"), "Missing or incompatible linear 2D texture."));
	};
	if (ImGui::TreeNode(T(TKEY("cirrus_texture_inputs"), "Cirrus texture inputs"))) {
		textures.DrawUI();
		ImGui::TreePop();
	}
	textureChoice(T(TKEY("cirrus_weather_rg"), "Coverage / Type RG"), settings.weatherPath, 2);
	textureChoice(T(TKEY("cirrus_patterns_rgb"), "Wispy / Round / Streaky RGB"), settings.patternsPath, 3);
	if (settings.patternsPath.empty()) {
		ImGui::InputScalar(T(TKEY("cirrus_pattern_seed"), "Pattern Seed"), ImGuiDataType_U32, &settings.patternSeed);
		ImGui::SliderFloat(T(TKEY("cirrus_pattern_warp"), "Pattern Warp"), &settings.patternWarp, 0.f, 0.5f);
		ImGui::SliderFloat(T(TKEY("cirrus_pattern_detail"), "Pattern Detail"), &settings.patternDetail, 0.f, 1.f);
	}
	if (!settings.weatherPath.empty())
		return;
	const char* labels[] = { T(TKEY("cirrus_coverage"), "Cirrus Coverage"), T(TKEY("cirrus_type"), "Cirrus Type") };
	for (uint32_t i = 0; i < 2; ++i) {
		if (!ImGui::TreeNode(labels[i]))
			continue;
		auto& layer = settings.weather[i];
		ImGui::DragFloat2(T(TKEY("ndf_input_interval"), "Input interval"), &layer.range.x, 0.01f, -1.f, 1.f);
		ImGui::DragFloat2(T(TKEY("ndf_output_interval"), "Output interval"), &layer.range.z, 0.01f, 0.f, 1.f);
		ImGui::DragFloat(T(TKEY("ndf_frequency"), "Frequency"), &layer.frequency, 0.05f, 0.f, 64.f);
		ImGui::DragFloat2(T(TKEY("ndf_offset"), "Offset"), &layer.offset.x, 0.005f);
		auto& input = settings.noise[i];
		textureChoice(T(TKEY("cirrus_noise_source"), "Weather noise source"), input.texturePath, 1);
		if (input.texturePath.empty()) {
			auto& noise = input.parameters;
			const char* types[] = { "Alligator", "Perlin", "Perlin-Worley" };
			int type = static_cast<int>(std::min(noise.type, 2u));
			if (ImGui::Combo(T(TKEY("ndf_noise_type"), "Noise type"), &type, types, IM_ARRAYSIZE(types)))
				noise.type = static_cast<uint32_t>(type);
			ImGui::InputScalar(T(TKEY("ndf_seed"), "Seed"), ImGuiDataType_U32, &noise.seed);
			const uint32_t one = 1, maxFrequency = 128, maxOctaves = 8, maxLacunarity = 4, maxRepetitions = 8;
			ImGui::SliderScalar(T(TKEY("ndf_base_frequency"), "Base frequency"), ImGuiDataType_U32, &noise.frequency, &one, &maxFrequency);
			ImGui::SliderScalar(T(TKEY("ndf_octaves"), "Octaves"), ImGuiDataType_U32, &noise.octaves, &one, &maxOctaves);
			ImGui::SliderScalar(T(TKEY("ndf_lacunarity"), "Lacunarity"), ImGuiDataType_U32, &noise.lacunarity, &one, &maxLacunarity);
			ImGui::SliderScalar(T(TKEY("ndf_tile_repetitions"), "Tile repetitions"), ImGuiDataType_U32, &noise.repetitions, &one, &maxRepetitions);
			ImGui::SliderFloat(T(TKEY("ndf_persistence"), "Persistence"), &noise.persistence, 0.f, 1.f);
			ImGui::SliderFloat(T(TKEY("ndf_contrast"), "Contrast"), &noise.contrast, 0.1f, 8.f);
			ImGui::SliderFloat(T(TKEY("ndf_response_exponent"), "Response exponent"), &noise.responseExponent, 0.1f, 8.f);
			ImGui::SliderFloat(T(TKEY("ndf_bias"), "Bias"), &noise.bias, -1.f, 1.f);
		}
		ImGui::TreePop();
	}
}

#undef I18N_KEY_PREFIX
