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
	if (ImGui::Button(T(TKEY("load"), "Load"))) {
		LoadTexture(std::filesystem::u8path(uiPath));
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("remove"), "Remove"))) {
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
	eastl::unique_ptr<Texture2D> CreateNdfTexture(uint32_t dimension, DXGI_FORMAT format, const char* name, bool mips = false, uint32_t height = 0)
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
	texHeight = CreateNdfTexture(kNdfDim, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::NdfHeight");
	texModeling = CreateNdfTexture(kNdfDim, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::NdfModeling");
	for (auto& texture : noiseTextures)
		texture = CreateNdfTexture(kNdfDim, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::NdfNoise", true);
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
	noiseValid.fill(false);
	noiseInputs.fill(nullptr);
	for (auto& resource : noiseInputResources)
		resource = nullptr;
	generatorProgram = nullptr;
	noiseProgram = nullptr;
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfGenerate.cs.hlsl", {}, "cs_5_0")))
		generatorProgram.attach(raw);
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfNoise.cs.hlsl", {}, "cs_5_0")))
		noiseProgram.attach(raw);
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
			ImGui::TextColored({ 1, 0.3f, 0.2f, 1 }, "%s", T(TKEY("ndf_incompatible_texture"), "Missing or incompatible texture; using generated input (local influences are skipped)."));
	};
	if (settings.type == NdfType::Texture) {
		textureChoice(T(TKEY("ndf_height_rg"), "Height RG"), settings.texture.heightPath, 2);
		textureChoice(T(TKEY("ndf_modeling_rgb"), "Modeling RGB"), settings.texture.modelingPath, 3);
		if (!IsTexturePair({ textures.Query(settings.texture.heightPath), textures.Query(settings.texture.modelingPath) }))
			ImGui::TextWrapped("%s", T(TKEY("ndf_select_both_textures"), "Select matching linear 2D Height RG and Modeling RGB textures; invalid pairs use the procedural cloud map."));
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
	rangeControl(T(TKEY("ndf_height_auxiliary"), "Bottom shaping start from top type"), parameters.heightAuxiliaryRange);
	ImGui::SliderFloat2(T(TKEY("ndf_base_height_rg"), "Base height RG"), &parameters.baseHeight.x, 0.f, 1.f);
	bool fromCoverage = parameters.heightFromCoverage != 0u;
	if (ImGui::Checkbox(T(TKEY("ndf_height_from_coverage"), "Height variation from coverage"), &fromCoverage))
		parameters.heightFromCoverage = fromCoverage ? 1u : 0u;
	layerControl(T(TKEY("ndf_bottom_height_variation"), "Bottom height variation"), parameters.heightVariation, true);
	ImGui::DragFloat2(T(TKEY("ndf_weather_offset"), "Weather offset (m)"), &parameters.windOffset.x, 1.f);
	if (ImGui::TreeNode(T(TKEY("ndf_local_influence"), "Global generator overlay"))) {
		ImGui::TextWrapped("%s", T(TKEY("ndf_overlay_hint"), "This overlay repeats with the generated global field. Use Local Cloud Instances for world placement."));
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

void NdfManager::DrawPreview()
{
	const char* channels[] = {
		T(TKEY("ndf_preview_off"), "Off"),
		T(TKEY("ndf_preview_coverage_a"), "Coverage A"),
		T(TKEY("ndf_preview_coverage_b"), "Coverage B"),
		T(TKEY("ndf_coverage_gain"), "Coverage gain"),
		T(TKEY("ndf_type_gain"), "Type gain"),
		T(TKEY("ndf_preview_coverage"), "Coverage"),
		T(TKEY("ndf_preview_top_type"), "Top type"),
		T(TKEY("ndf_bottom_type"), "Bottom type"),
		T(TKEY("ndf_preview_bottom_height"), "Bottom height"),
		T(TKEY("ndf_preview_top_height"), "Top height"),
		T(TKEY("ndf_preview_shaping_start"), "Bottom shaping start"),
		T(TKEY("ndf_preview_height_variation"), "Height variation")
	};
	ImGui::Combo(T(TKEY("ndf_preview"), "NDF component"), &preview, channels, IM_ARRAYSIZE(channels));
	if (preview && texPreview && generatedValid && generatedData.preview == static_cast<uint32_t>(preview))
		ImGui::Image(texPreview->srv.get(), { 384.f, 384.f * texPreview->desc.Height / texPreview->desc.Width });
}

#undef I18N_KEY_PREFIX

bool NdfManager::UpdateNdf(const NdfSettings& settings, TextureManager& textures)
{
	std::array<std::string, 9> paths = {};
	if (settings.type == NdfType::Texture) {
		paths[0] = settings.texture.heightPath;
		paths[1] = settings.texture.modelingPath;
	}
	{
		for (uint32_t i = 0; i < 4; ++i)
			paths[i + 2] = settings.procedural.noise[i].texturePath;
		paths[6] = settings.procedural.local.heightPath;
		paths[7] = settings.procedural.local.modelingPath;
		paths[8] = settings.procedural.localMaskPath;
	}
	for (const auto& path : paths)
		textures.EnsureLoaded(path);
	sourcePaths = std::move(paths);
	if (!generatorProgram || !noiseProgram || !generatorCb || !noiseCb || !sampler || !texHeight || !texModeling)
		return false;
	auto* context = globals::d3d::context;
	std::array<ID3D11ShaderResourceView*, 7> sources = {};
	const auto& procedural = settings.procedural;
	for (uint32_t i = 0; i < 4; ++i) {
		const auto& input = procedural.noise[i];
		if (auto* inputTexture = textures.Query(input.texturePath); IsTextureNdf(inputTexture, 1)) {
			sources[i] = inputTexture;
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
		if (!IsTextureNdf(sources[i + 4], localInputs[i].second))
			sources[i + 4] = nullptr;
	}
	auto data = procedural.parameters;
	data.imported = 0;
	if (settings.type == NdfType::Texture) {
		if (const auto maps = QueryTextures(settings.texture, textures)) {
			sources[4] = maps.modeling;
			sources[5] = maps.height;
			data.imported = 1;
		}
	}
	uint32_t width = kNdfDim, height = kNdfDim;
	if (data.imported) {
		winrt::com_ptr<ID3D11Resource> resource;
		sources[5]->GetResource(resource.put());
		D3D11_TEXTURE2D_DESC desc;
		resource.as<ID3D11Texture2D>()->GetDesc(&desc);
		D3D11_SHADER_RESOURCE_VIEW_DESC view;
		sources[5]->GetDesc(&view);
		width = std::max(desc.Width >> view.Texture2D.MostDetailedMip, 1u);
		height = std::max(desc.Height >> view.Texture2D.MostDetailedMip, 1u);
	}
	if (texHeight->desc.Width != width || texHeight->desc.Height != height) {
		texHeight = CreateNdfTexture(width, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::NdfHeight", false, height);
		texModeling = CreateNdfTexture(width, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::NdfModeling", false, height);
		texPreview = nullptr;
		generatedValid = false;
	}
	data.preview = static_cast<uint32_t>(preview);
	if (preview && !texPreview)
		texPreview = CreateNdfTexture(width, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::NdfPreview", false, height);
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
	SanitizeRange(data.heightAuxiliaryRange);
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
	data.padding = {};
	auto previousData = generatedData;
	previousData.preview = data.preview;
	const bool changed = !generatedValid || sources != generatedSources || textureRevision != textures.revision || std::memcmp(&previousData, &data, sizeof(data)) != 0;
	if (!changed && generatedData.preview == data.preview)
		return false;
	generatorCb->Update(data);
	auto* cb = generatorCb->CB();
	auto* samp = sampler.get();
	winrt::com_ptr<ID3D11SamplerState> previousSampler;
	context->CSGetSamplers(0, 1, previousSampler.put());
	ID3D11UnorderedAccessView* uavs[3] = { texHeight->uav.get(), texModeling->uav.get(), preview ? texPreview->uav.get() : nullptr };
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &samp);
	context->CSSetShaderResources(0, static_cast<uint32_t>(sources.size()), sources.data());
	context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
	context->CSSetShader(generatorProgram.get(), nullptr, 0);
	globals::profiler->BeginPass("PhysicalSky::CloudNdf");
	context->Dispatch((width + 7u) >> 3, (height + 7u) >> 3, 1);
	globals::profiler->EndPass();
	ID3D11ShaderResourceView* nullSrvs[7] = {};
	ID3D11UnorderedAccessView* nullUavs[3] = {};
	context->CSSetShaderResources(0, 7, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);
	samp = previousSampler.get();
	context->CSSetSamplers(0, 1, &samp);
	generatedData = data;
	generatedSources = sources;
	textureRevision = textures.revision;
	if (changed)
		++generatedRevision;
	std::copy_n(sources.begin(), 4, noiseInputs.begin());
	for (size_t i = 0; i < noiseInputs.size(); ++i)
		noiseInputResources[i].copy_from(noiseInputs[i]);
	generatedValid = true;
	return changed;
}

bool NdfManager::IsTextureNdf(ID3D11ShaderResourceView* srv, uint32_t channels)
{
	if (!srv)
		return false;
	D3D11_SHADER_RESOURCE_VIEW_DESC desc;
	srv->GetDesc(&desc);
	if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
		return false;
	return IsLinearFormat(desc.Format, channels);
}

bool NdfManager::IsLinearFormat(DXGI_FORMAT format, uint32_t channels)
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

bool NdfManager::IsTexturePair(NdfTextureSet maps)
{
	if (!IsTextureNdf(maps.height, 2) || !IsTextureNdf(maps.modeling, 3))
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

NdfTextureSet NdfManager::QueryTextures(const TexNdfSettings& settings, TextureManager& textures)
{
	NdfTextureSet maps{ textures.Query(settings.heightPath), textures.Query(settings.modelingPath) };
	return IsTexturePair(maps) ? maps : NdfTextureSet{};
}

NdfTextureSet NdfManager::GetNdf(const NdfSettings& settings, TextureManager& textures)
{
	if (!generatedValid || !texHeight || !texModeling)
		return {};
	if (settings.type == NdfType::Texture && generatedData.imported) {
		if (const auto maps = QueryTextures(settings.texture, textures); maps && maps.height == generatedSources[5] && maps.modeling == generatedSources[4])
			return { texHeight->srv.get(), maps.modeling };
		return {};
	}
	return { texHeight->srv.get(), texModeling->srv.get() };
}

void CirrusMapManager::SetupResources()
{
	generationCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<GenerationParameters>());
	texWeather = CreateNdfTexture(kDimension, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::CirrusWeather", true);
	texPatterns = CreateNdfTexture(kDimension, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CirrusPatterns", true);
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
	outputs = {};
	for (auto& resource : outputResources)
		resource = nullptr;
	weatherProgram = nullptr;
	patternsProgram = nullptr;
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\CirrusGenerate.cs.hlsl", {}, "cs_5_0", "generateWeather")))
		weatherProgram.attach(raw);
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\CirrusGenerate.cs.hlsl", {}, "cs_5_0", "generatePatterns")))
		patternsProgram.attach(raw);
}

bool CirrusMapManager::ShadersReady(const CirrusSettings&) const
{
	return weatherProgram && patternsProgram && generationCb && sampler && texWeather && texPatterns;
}

CirrusTextureSet CirrusMapManager::GetTextures() const
{
	return outputs;
}

bool CirrusMapManager::Update(const CirrusSettings& settings, TextureManager& textures, const NdfManager& ndf)
{
	if (!ShadersReady(settings))
		return false;
	const std::array<std::string, 2> paths{ settings.weatherPath, settings.patternsPath };
	for (const auto& path : paths)
		textures.EnsureLoaded(path);
	sourcePaths = paths;
	std::array<ID3D11ShaderResourceView*, 6> sources = {};
	std::copy(ndf.GetNoiseInputs().begin(), ndf.GetNoiseInputs().end(), sources.begin());
	for (uint32_t i = 0; i < 2; ++i) {
		auto* source = textures.Query(paths[i]);
		if (NdfManager::IsTextureNdf(source, i == 0 ? 2u : 3u))
			sources[i + 4] = source;
	}
	if (!sources[4] && std::ranges::any_of(ndf.GetNoiseInputs(), [](auto* input) { return !input; }))
		return false;
	auto* context = globals::d3d::context;
	GenerationParameters data{
		.weather = settings.weather,
		.seed = settings.patternSeed,
		.warp = FiniteClamp(settings.patternWarp, 0.f, 0.5f, 0.15f),
		.detail = FiniteClamp(settings.patternDetail, 0.f, 1.f, 0.35f),
		.windOffset = ndf.GetWeatherOffset()
	};
	for (uint32_t i = 0; i < 2; ++i) {
		auto& layer = data.weather[i];
		layer.noise = std::min(layer.noise, 4u);
		layer.frequency = FiniteClamp(layer.frequency, 0.f, 64.f, 1.f);
		layer.exponent = 1.f;
		layer.padding0 = 0.f;
		layer.offset.x = FiniteClamp(layer.offset.x, -10000.f, 10000.f, 0.f);
		layer.offset.y = FiniteClamp(layer.offset.y, -10000.f, 10000.f, 0.f);
		layer.padding1 = {};
		SanitizeRange(layer.range);
	}
	if (generatedValid && generatedSources == sources && generatedRevision == textures.revision && noiseRevision == ndf.GetRevision() && std::memcmp(&data, &generatedData, sizeof(data)) == 0)
		return false;
	generationCb->Update(data);
	auto* cb = generationCb->CB();
	auto* samp = sampler.get();
	winrt::com_ptr<ID3D11SamplerState> previousSampler;
	context->CSGetSamplers(0, 1, previousSampler.put());
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &samp);
	if (!sources[4]) {
		auto* uav = texWeather->uav.get();
		context->CSSetShaderResources(0, 4, sources.data());
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(weatherProgram.get(), nullptr, 0);
		context->Dispatch((kDimension + 7u) >> 3, (kDimension + 7u) >> 3, 1);
		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->GenerateMips(texWeather->srv.get());
	}
	if (!sources[5]) {
		auto* uav = texPatterns->uav.get();
		context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
		context->CSSetShader(patternsProgram.get(), nullptr, 0);
		context->Dispatch((kDimension + 7u) >> 3, (kDimension + 7u) >> 3, 1);
		uav = nullptr;
		context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
		context->GenerateMips(texPatterns->srv.get());
	}
	ID3D11ShaderResourceView* nullSrvs[4] = {};
	context->CSSetShaderResources(0, 4, nullSrvs);
	cb = nullptr;
	samp = previousSampler.get();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, &samp);
	context->CSSetShader(nullptr, nullptr, 0);
	outputs = { sources[4] ? sources[4] : texWeather->srv.get(), sources[5] ? sources[5] : texPatterns->srv.get() };
	outputResources[0].copy_from(outputs.weather);
	outputResources[1].copy_from(outputs.patterns);
	generatedSources = sources;
	generatedData = data;
	generatedRevision = textures.revision;
	noiseRevision = ndf.GetRevision();
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
			ImGui::TextColored({ 1, 0.3f, 0.2f, 1 }, "%s", T(TKEY("ndf_incompatible_texture"), "Missing or incompatible texture; using generated input (local influences are skipped)."));
	};
	if (ImGui::TreeNode(T(TKEY("cirrus_texture_inputs"), "Cirrus texture inputs"))) {
		textures.DrawUI();
		ImGui::TreePop();
	}
	textureChoice(T(TKEY("cirrus_weather_rg"), "Coverage / Type RG"), settings.weatherPath, 2);
	textureChoice(T(TKEY("cirrus_patterns_rgb"), "Wispy / Round / Streaky RGB"), settings.patternsPath, 3);
	if (!NdfManager::IsTextureNdf(textures.Query(settings.patternsPath), 3)) {
		ImGui::InputScalar(T(TKEY("cirrus_pattern_seed"), "Pattern Seed"), ImGuiDataType_U32, &settings.patternSeed);
		ImGui::SliderFloat(T(TKEY("cirrus_pattern_warp"), "Pattern Warp"), &settings.patternWarp, 0.f, 0.5f);
		ImGui::SliderFloat(T(TKEY("cirrus_pattern_detail"), "Pattern Detail"), &settings.patternDetail, 0.f, 1.f);
	}
	if (NdfManager::IsTextureNdf(textures.Query(settings.weatherPath), 2))
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
		const char* slots[] = { "0", "1", "2", "3", T(TKEY("ndf_noise_slot_zero"), "Zero") };
		int slot = static_cast<int>(std::min(layer.noise, 4u));
		if (ImGui::Combo(T(TKEY("ndf_noise_slot"), "Noise slot"), &slot, slots, IM_ARRAYSIZE(slots)))
			layer.noise = static_cast<uint32_t>(slot);
		ImGui::TextWrapped("%s", T(TKEY("cirrus_shared_noise"), "Uses the four Cloud Map noise inputs."));
		ImGui::TreePop();
	}
}

#undef I18N_KEY_PREFIX
