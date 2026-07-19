#include "Features/PhysicalSky.h"

#include "I18n/I18n.h"
#include "State.h"
#include "Util.h"

#include <DDSTextureLoader.h>
#include <array>
#include <cmath>
#include <imgui_stdlib.h>
#include <numeric>

#define I18N_KEY_PREFIX "feature.physical_sky."

bool TextureManager::LoadTexture(std::filesystem::path path)
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	auto path_str = path.string();
	if (!texList.contains(path_str))
		texList.emplace(path_str, nullptr);

	return SUCCEEDED(DirectX::CreateDDSTextureFromFile(device, context, path.wstring().c_str(), nullptr, texList.at(path_str).put()));
}

void TextureManager::DrawUI()
{
	ImGui::InputText(T(TKEY("path"), "Path"), &uiPath, 0);
	if (ImGui::Button(T(TKEY("load"), "Load"))) {
		LoadTexture(uiPath);
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("remove"), "Remove"))) {
		texList.erase(uiPath);
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
	float Hash21(uint32_t x, uint32_t y, uint32_t seed)
	{
		uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
		h = (h ^ (h >> 13u)) * 1274126177u;
		return static_cast<float>(h ^ (h >> 16u)) / 4294967295.0f;
	}

	float ValueNoise(float x, float y, uint32_t seed)
	{
		const int ix = static_cast<int>(std::floor(x));
		const int iy = static_cast<int>(std::floor(y));
		const float fx = x - static_cast<float>(ix);
		const float fy = y - static_cast<float>(iy);
		const float sx = fx * fx * (3.0f - 2.0f * fx);
		const float sy = fy * fy * (3.0f - 2.0f * fy);
		const auto h = [&](int ox, int oy) { return Hash21(static_cast<uint32_t>(ix + ox), static_cast<uint32_t>(iy + oy), seed); };
		const float a = std::lerp(h(0, 0), h(1, 0), sx);
		const float b = std::lerp(h(0, 1), h(1, 1), sx);
		return std::lerp(a, b, sy);
	}

	float Fbm(float x, float y, uint32_t seed)
	{
		float amp = 0.5f;
		float sum = 0.0f;
		float freq = 1.0f;
		for (uint32_t i = 0; i < 5; ++i) {
			sum += amp * ValueNoise(x * freq, y * freq, seed + i * 17u);
			freq *= 2.03f;
			amp *= 0.5f;
		}
		return std::clamp(sum, 0.0f, 1.0f);
	}

	int WrapCoordinate(int value, int period)
	{
		value %= period;
		return value < 0 ? value + period : value;
	}

	float PeriodicValueNoise(float x, float y, int period, uint32_t seed)
	{
		const int ix = static_cast<int>(std::floor(x));
		const int iy = static_cast<int>(std::floor(y));
		const float fx = x - static_cast<float>(ix);
		const float fy = y - static_cast<float>(iy);
		const float sx = fx * fx * (3.0f - 2.0f * fx);
		const float sy = fy * fy * (3.0f - 2.0f * fy);
		const auto h = [&](int ox, int oy) {
			return Hash21(
				static_cast<uint32_t>(WrapCoordinate(ix + ox, period)),
				static_cast<uint32_t>(WrapCoordinate(iy + oy, period)), seed);
		};
		return std::lerp(std::lerp(h(0, 0), h(1, 0), sx), std::lerp(h(0, 1), h(1, 1), sx), sy);
	}

	float PeriodicFbm(float u, float v, int basePeriod, uint32_t seed)
	{
		float amplitude = 0.5f;
		float sum = 0.0f;
		int period = std::max(basePeriod, 1);
		for (uint32_t octave = 0; octave < 5; ++octave) {
			sum += amplitude * PeriodicValueNoise(u * period, v * period, period, seed + octave * 17u);
			period *= 2;
			amplitude *= 0.5f;
		}
		return std::clamp(sum, 0.0f, 1.0f);
	}

	float PeriodicCellNoise(float u, float v, int cellCount, uint32_t seed)
	{
		const float px = u * cellCount;
		const float py = v * cellCount;
		const int ix = static_cast<int>(std::floor(px));
		const int iy = static_cast<int>(std::floor(py));
		float nearestDistanceSq = 8.0f;
		for (int oy = -1; oy <= 1; ++oy) {
			for (int ox = -1; ox <= 1; ++ox) {
				const int cellX = ix + ox;
				const int cellY = iy + oy;
				const uint32_t wrappedX = static_cast<uint32_t>(WrapCoordinate(cellX, cellCount));
				const uint32_t wrappedY = static_cast<uint32_t>(WrapCoordinate(cellY, cellCount));
				const float featureX = static_cast<float>(cellX) + Hash21(wrappedX, wrappedY, seed);
				const float featureY = static_cast<float>(cellY) + Hash21(wrappedX, wrappedY, seed + 41u);
				const float dx = featureX - px;
				const float dy = featureY - py;
				nearestDistanceSq = std::min(nearestDistanceSq, dx * dx + dy * dy);
			}
		}
		// Bright cell centres and dark cell boundaries match the shader's thickness convention.
		return std::clamp(1.0f - std::sqrt(nearestDistanceSq) * 1.25f, 0.0f, 1.0f);
	}

	eastl::unique_ptr<Texture2D> CreateGeneratedTexture(uint32_t width, uint32_t height, const char* name, const std::vector<uint8_t>& data)
	{
		auto device = globals::d3d::device;
		D3D11_TEXTURE2D_DESC desc{
			.Width = width,
			.Height = height,
			.MipLevels = 0,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS
		};

		winrt::com_ptr<ID3D11Texture2D> raw;
		DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, raw.put()));
		globals::d3d::context->UpdateSubresource(raw.get(), 0, nullptr, data.data(), width * 4u, width * height * 4u);

		auto tex = eastl::make_unique<Texture2D>(raw.detach(), name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = static_cast<UINT>(-1) }
		};
		tex->CreateSRV(srvDesc);
		globals::d3d::context->GenerateMips(tex->srv.get());
		return tex;
	}

	void UploadGeneratedTexture(eastl::unique_ptr<Texture2D>& texture, uint32_t width, uint32_t height, const char* name, const std::vector<uint8_t>& data)
	{
		if (texture && texture->desc.Width == width && texture->desc.Height == height && texture->desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
			(texture->desc.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) != 0) {
			globals::d3d::context->UpdateSubresource(texture->resource.get(), 0, nullptr, data.data(), width * 4u, 0);
			globals::d3d::context->GenerateMips(texture->srv.get());
			return;
		}
		texture = CreateGeneratedTexture(width, height, name, data);
	}

	uint8_t ToByte(float v)
	{
		return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
	}

	float SmoothStep(float edge0, float edge1, float value)
	{
		const float t = std::clamp((value - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	uint32_t ScoreBin(float value)
	{
		return std::min(static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 256.0f), 255u);
	}

	float HistogramQuantile(const std::array<uint32_t, 256>& histogram, float fractionBelow)
	{
		const uint64_t total = std::accumulate(histogram.begin(), histogram.end(), uint64_t{ 0 });
		if (total == 0)
			return 0.5f;
		const uint64_t target = static_cast<uint64_t>(std::clamp(fractionBelow, 0.0f, 1.0f) * static_cast<float>(total - 1));
		uint64_t cumulative = 0;
		for (uint32_t bin = 0; bin < histogram.size(); ++bin) {
			cumulative += histogram[bin];
			if (cumulative > target)
				return (static_cast<float>(bin) + 0.5f) / 256.0f;
		}
		return 1.0f;
	}

	float HistogramSplitThreshold(const std::array<uint32_t, 256>& histogram, float fractionBelow)
	{
		if (fractionBelow <= 0.0f)
			return -1.0f;
		if (fractionBelow >= 1.0f)
			return 2.0f;
		return HistogramQuantile(histogram, fractionBelow);
	}

	void DrawTextureOverride(const char* label, std::string& path, TextureManager& texManager)
	{
		const char* generated = T(TKEY("generated"), "Generated");
		if (ImGui::BeginCombo(label, path.empty() ? generated : path.c_str())) {
			if (ImGui::Selectable(generated, path.empty()))
				path.clear();
			for (auto& choice : texManager.ListPaths()) {
				if (ImGui::Selectable(choice.c_str(), choice == path))
					path = choice;
			}
			ImGui::EndCombo();
		}
	}
}

void NdfManager::SetupResources()
{
	logger::debug("Creating generated cloud map resources...");
	GenerateDefaultTextures(HpGeneratedCloudMapSettings{}, CloudLayer{});
}

void NdfManager::CompileShaders()
{
}

const char* NdfManager::GetSettingsTypeName(const NdfSettings&)
{
	return T(TKEY("generated"), "Generated");
}

const char* NdfManager::GetSettingsHint(const NdfSettings&)
{
	return T(TKEY("generated_cloud_map_hint"), "Generates weather, profile, cell, warp, and wisp textures by default. Loaded DDS textures can override each generated input.");
}

void NdfManager::DrawNdfSettings(NdfSettings& ndfSettings, TextureManager& texManager)
{
	deferredGenerationFrame = -1;
	const auto deferWhileEditing = [&]() {
		if (ImGui::IsItemActive())
			deferredGenerationFrame = ImGui::GetFrameCount();
	};
	ImGui::TextWrapped("%s", GetSettingsHint(ndfSettings));
	ImGui::SeparatorText(T(TKEY("generated_weather"), "Generated Weather"));
	uint32_t weatherMin = 128u;
	uint32_t weatherMax = 1024u;
	uint32_t profileWidthMin = 64u;
	uint32_t profileWidthMax = 512u;
	uint32_t profileHeightMin = 16u;
	uint32_t profileHeightMax = 256u;
	ImGui::SliderScalar(T(TKEY("weather_dimension"), "Weather Dimension"), ImGuiDataType_U32, &ndfSettings.weatherDim, &weatherMin, &weatherMax, "%u");
	deferWhileEditing();
	ImGui::SliderScalar(T(TKEY("profile_width"), "Profile Width"), ImGuiDataType_U32, &ndfSettings.profileWidth, &profileWidthMin, &profileWidthMax, "%u");
	deferWhileEditing();
	ImGui::SliderScalar(T(TKEY("profile_height"), "Profile Height"), ImGuiDataType_U32, &ndfSettings.profileHeight, &profileHeightMin, &profileHeightMax, "%u");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("weather_world_size"), "Weather World Size"), &ndfSettings.worldSize, 8.f, 256.f, "%.1f km");
	ImGui::SliderFloat2(T(TKEY("weather_center"), "Weather Center"), &ndfSettings.center.x, -256.f, 256.f, "%.1f km");
	ImGui::SliderFloat(T(TKEY("low_coverage"), "Low Coverage"), &ndfSettings.lowCoverage, 0.f, 1.f, "%.2f");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("low_contrast"), "Low Contrast"), &ndfSettings.lowContrast, 0.25f, 4.f, "%.2f");
	deferWhileEditing();
	ImGui::SeparatorText(T(TKEY("low_cloud_species"), "Low Cloud Species"));
	ImGui::SliderFloat(T(TKEY("stratocumulus_share"), "Stratocumulus Share"), &ndfSettings.stratocumulus, 0.f, 1.f, "%.2f");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("cumulus_weight"), "Cumulus Weight"), &ndfSettings.cumulusWeight, 0.f, 1.f, "%.2f");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("towering_cumulus_weight"), "Towering Cumulus Weight"), &ndfSettings.toweringCumulusWeight, 0.f, 1.f, "%.2f");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("cumulonimbus_weight"), "Cumulonimbus Weight"), &ndfSettings.cumulonimbusWeight, 0.f, 1.f, "%.2f");
	deferWhileEditing();
	ImGui::TextWrapped("%s", T(TKEY("cloud_species_weight_hint"), "Weights are normalized within cloud-covered regions. Set a weight to zero to disable that species."));
	ImGui::SliderFloat(T(TKEY("cumulus_vertical_depth"), "Cumulus Vertical Depth"), &ndfSettings.cumulusDepth, 0.2f, 5.f, "%.2f km");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("towering_cumulus_vertical_depth"), "Towering Cumulus Vertical Depth"), &ndfSettings.toweringCumulusDepth, 0.5f, 10.f, "%.2f km");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("cumulonimbus_vertical_depth"), "Cumulonimbus Vertical Depth"), &ndfSettings.cumulonimbusDepth, 1.f, 16.f, "%.2f km");
	deferWhileEditing();
	ImGui::SeparatorText(T(TKEY("high_cloud_species"), "High Cloud Species"));
	ImGui::SliderFloat(T(TKEY("high_coverage"), "High Coverage"), &ndfSettings.highCoverage, 0.f, 1.f, "%.2f");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("high_contrast"), "High Contrast"), &ndfSettings.highContrast, 0.25f, 4.f, "%.2f");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("altostratus_weight"), "Altostratus Weight"), &ndfSettings.altostratusWeight, 0.f, 1.f, "%.2f");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("altocumulus_weight"), "Altocumulus Weight"), &ndfSettings.altocumulusWeight, 0.f, 1.f, "%.2f");
	deferWhileEditing();

	ImGui::SeparatorText(T(TKEY("texture_overrides"), "Texture Overrides"));
	texManager.DrawUI();
	DrawTextureOverride(T(TKEY("low_weather"), "Low Weather"), ndfSettings.overrides.lowWeatherPath, texManager);
	DrawTextureOverride(T(TKEY("high_weather"), "High Weather"), ndfSettings.overrides.highWeatherPath, texManager);
	DrawTextureOverride(T(TKEY("profile_lut"), "Profile LUT"), ndfSettings.overrides.profilePath, texManager);
	DrawTextureOverride(T(TKEY("sc_cell"), "Sc Cell"), ndfSettings.overrides.scCellPath, texManager);
	DrawTextureOverride(T(TKEY("high_cell"), "High Cell"), ndfSettings.overrides.highCellPath, texManager);
	DrawTextureOverride(T(TKEY("high_warp"), "High Warp"), ndfSettings.overrides.highWarpPath, texManager);
	DrawTextureOverride(T(TKEY("high_wisp"), "High Wisp"), ndfSettings.overrides.highWispPath, texManager);
}

void NdfManager::UpdateNdf(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer)
{
	GenerateDefaultTextures(ndfSettings, cloudLayer);
}

ID3D11ShaderResourceView* NdfManager::GetNdf(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer, TextureManager& texManager)
{
	return GetHpTextures(ndfSettings, cloudLayer, texManager).lowWeather;
}

HpCloudTextureSet NdfManager::GetHpTextures(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer, TextureManager& texManager)
{
	GenerateDefaultTextures(ndfSettings, cloudLayer);
	const auto resolveTexture = [&](const std::string& path, ID3D11ShaderResourceView* fallback) {
		if (path.empty())
			return fallback;
		if (!texManager.texList.contains(path))
			texManager.LoadTexture(path);
		if (auto* texture = texManager.Query(path))
			return texture;
		return fallback;
	};
	return {
		.lowWeather = resolveTexture(ndfSettings.overrides.lowWeatherPath, texLowWeather->srv.get()),
		.highWeather = resolveTexture(ndfSettings.overrides.highWeatherPath, texHighWeather->srv.get()),
		.profile = resolveTexture(ndfSettings.overrides.profilePath, texProfile->srv.get()),
		.scCell = resolveTexture(ndfSettings.overrides.scCellPath, texScCell->srv.get()),
		.highCell = resolveTexture(ndfSettings.overrides.highCellPath, texHighCell->srv.get()),
		.highWarp = resolveTexture(ndfSettings.overrides.highWarpPath, texHighWarp->srv.get()),
		.highWisp = resolveTexture(ndfSettings.overrides.highWispPath, texHighWisp->srv.get()),
	};
}

void NdfManager::GenerateDefaultTextures(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer)
{
	const uint32_t weatherDim = std::clamp(ndfSettings.weatherDim, 128u, 1024u);
	const uint32_t profileWidth = std::clamp(ndfSettings.profileWidth, 64u, 512u);
	const uint32_t profileHeight = std::clamp(ndfSettings.profileHeight, 16u, 256u);
	const float layerDepth = std::max(cloudLayer.highestAltitude - cloudLayer.lowestAltitude, 0.05f);
	const bool haveGeneratedTextures = texLowWeather && texHighWeather && texProfile && texScCell && texHighCell && texHighWarp && texHighWisp;
	// While an ImGui control is actively dragged, keep rendering the last committed
	// maps. Rebuild once on release instead of synchronously regenerating every
	// intermediate slider value.
	if (ImGui::GetCurrentContext() && deferredGenerationFrame == ImGui::GetFrameCount() && haveGeneratedTextures)
		return;

	const size_t weatherPixelCount = static_cast<size_t>(weatherDim) * weatherDim;
	const bool weatherLayoutChanged = generatedWeatherDim != weatherDim || cachedCoverageField.size() != weatherPixelCount;
	const bool profileChanged = !texProfile || generatedProfileWidth != profileWidth || generatedProfileHeight != profileHeight ||
	                            generatedLayerDepth != layerDepth || generatedCumulusDepth != ndfSettings.cumulusDepth ||
	                            generatedToweringCumulusDepth != ndfSettings.toweringCumulusDepth ||
	                            generatedCumulonimbusDepth != ndfSettings.cumulonimbusDepth;
	const bool lowWeatherChanged = weatherLayoutChanged || !texLowWeather ||
	                               generatedLowCoverage != ndfSettings.lowCoverage || generatedLowContrast != ndfSettings.lowContrast ||
	                               generatedStratocumulus != ndfSettings.stratocumulus ||
	                               generatedCumulusWeight != ndfSettings.cumulusWeight ||
	                               generatedToweringCumulusWeight != ndfSettings.toweringCumulusWeight ||
	                               generatedCumulonimbusWeight != ndfSettings.cumulonimbusWeight;
	const bool highWeatherChanged = weatherLayoutChanged || !texHighWeather ||
	                                generatedHighCoverage != ndfSettings.highCoverage || generatedHighContrast != ndfSettings.highContrast ||
	                                generatedAltostratusWeight != ndfSettings.altostratusWeight ||
	                                generatedAltocumulusWeight != ndfSettings.altocumulusWeight;
	if (!weatherLayoutChanged && !profileChanged && !lowWeatherChanged && !highWeatherChanged)
		return;

	if (weatherLayoutChanged) {
		cachedCoverageField.resize(weatherPixelCount);
		cachedCloudType.resize(weatherPixelCount);
		cachedScRegion.resize(weatherPixelCount);
		cachedHighField.resize(weatherPixelCount);
		cachedHighType.resize(weatherPixelCount);
		cachedHighScatterField.resize(weatherPixelCount);
		std::vector<uint8_t> scCell(weatherPixelCount * 4u);
		std::vector<uint8_t> hiCell(weatherPixelCount * 4u);
		std::vector<uint8_t> hiWarp(weatherPixelCount * 4u);
		std::vector<uint8_t> hiWisp(weatherPixelCount * 4u);

		for (uint32_t y = 0; y < weatherDim; ++y) {
			for (uint32_t x = 0; x < weatherDim; ++x) {
				const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(weatherDim);
				const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(weatherDim);
				// Build a hierarchy instead of treating every channel as unrelated FBM:
				// synoptic moisture and fronts define cloud systems, while meso-scale
				// convection selects the vertical family inside those systems.
				const float warpX = (Fbm(u * 1.6f + 13.0f, v * 1.6f - 7.0f, 3u) - 0.5f) * 0.28f;
				const float warpY = (Fbm(u * 1.6f - 5.0f, v * 1.6f + 11.0f, 5u) - 0.5f) * 0.28f;
				const float warpedU = u + warpX;
				const float warpedV = v + warpY;
				const float synopticMoisture = Fbm(warpedU * 2.1f + 3.0f, warpedV * 2.1f - 5.0f, 11u);
				const float mesoMoisture = Fbm(warpedU * 5.4f + 17.0f, warpedV * 5.4f, 23u);
				const float fineMoisture = Fbm(warpedU * 13.0f, warpedV * 13.0f + 9.0f, 37u);
				const float dryIntrusion = Fbm(warpedU * 3.2f - 11.0f, warpedV * 3.2f + 19.0f, 41u);

				// Narrow contours of another large-scale field form broken frontal bands.
				const float frontDriver = Fbm(warpedU * 1.35f + 31.0f, warpedV * 1.35f - 19.0f, 43u);
				const float frontDistance = std::abs(frontDriver - 0.5f) * 2.0f;
				const float frontCore = 1.0f - SmoothStep(0.03f, 0.14f, frontDistance);
				const float frontBreakup = 0.35f + 0.65f * SmoothStep(0.28f, 0.72f, Fbm(warpedU * 6.5f - 7.0f, warpedV * 6.5f + 23.0f, 47u));
				const float frontalBand = frontCore * frontBreakup;

				const float coverageField = std::clamp(
					0.05f + synopticMoisture * 0.58f + mesoMoisture * 0.22f + fineMoisture * 0.12f + frontalBand * 0.22f - dryIntrusion * 0.12f,
					0.0f, 1.0f);
				const float convectionNoise = Fbm(warpedU * 8.5f + 7.0f, warpedV * 8.5f - 13.0f, 53u);
				const float convection = SmoothStep(0.34f, 0.64f, convectionNoise) * SmoothStep(0.28f, 0.64f, coverageField);
				const float typeVariation = Fbm(warpedU * 4.0f - 29.0f, warpedV * 4.0f + 5.0f, 59u);
				const float typeDriver = std::clamp(
					0.12f + convection * 0.58f + coverageField * 0.34f + typeVariation * 0.16f - frontalBand * 0.22f,
					0.0f, 1.0f);

				const float upperWarpX = (Fbm(u * 1.2f + 37.0f, v * 1.2f - 17.0f, 67u) - 0.5f) * 0.20f;
				const float upperWarpY = (Fbm(u * 1.2f - 23.0f, v * 1.2f + 41.0f, 69u) - 0.5f) * 0.20f;
				const float upperU = u + upperWarpX;
				const float upperV = v + upperWarpY;
				const float upperSynoptic = Fbm(upperU * 1.8f + 3.0f, upperV * 1.8f - 5.0f, 71u);
				const float upperMeso = Fbm(upperU * 5.5f - 17.0f, upperV * 5.5f + 29.0f, 73u);
				const float anvilOutflow = SmoothStep(0.56f, 0.82f, typeDriver) * SmoothStep(0.36f, 0.70f, coverageField);
				const float highField = std::clamp(
					upperSynoptic * 0.52f + upperMeso * 0.20f + frontalBand * 0.30f + anvilOutflow * 0.34f - dryIntrusion * 0.08f,
					0.0f, 1.0f);
				const float upperCellular = Fbm(upperU * 7.0f + 41.0f, upperV * 7.0f - 37.0f, 79u);
				const float highTypeDriver = std::clamp(
					0.10f + upperCellular * 0.72f + anvilOutflow * 0.50f + typeVariation * 0.10f - frontalBand * 0.28f,
					0.0f, 1.0f);

				// The finite generated map fades before its boundary so sampling outside
				// the map cannot reveal a rectangular wall of cloud.
				const float edgeDistance = std::max(std::abs(u - 0.5f), std::abs(v - 0.5f)) * 2.0f;
				const float edgeFade = 1.0f - SmoothStep(0.86f, 1.0f, edgeDistance);

				const size_t pixel = static_cast<size_t>(y) * weatherDim + x;
				cachedCoverageField[pixel] = coverageField * edgeFade;
				// Cache suitability rather than a pre-baked type. Coverage-aware
				// quantiles assign the requested species shares when the low map is built.
				cachedCloudType[pixel] = typeDriver;
				const float stableNoise = Fbm(warpedU * 3.8f + 5.0f, warpedV * 3.8f - 31.0f, 61u);
				cachedScRegion[pixel] = std::clamp(
											0.14f + stableNoise * 0.48f + frontalBand * 0.50f + coverageField * 0.18f - convection * 0.42f,
											0.0f, 1.0f) *
				                        edgeFade;
				cachedHighField[pixel] = highField * edgeFade;
				cachedHighType[pixel] = highTypeDriver;
				cachedHighScatterField[pixel] = std::clamp(upperSynoptic * 0.45f + frontalBand * 0.25f + anvilOutflow * 0.30f, 0.0f, 1.0f) * edgeFade;

				const size_t i = pixel * 4u;
				const float cellWarpX = (PeriodicFbm(u, v, 2, 83u) - 0.5f) * 0.08f;
				const float cellWarpY = (PeriodicFbm(u + 0.31f, v + 0.57f, 2, 87u) - 0.5f) * 0.08f;
				const float cell = PeriodicCellNoise(u + cellWarpX, v + cellWarpY, 4, 89u) * 0.78f +
				                   PeriodicCellNoise(u + cellWarpX + 0.17f, v + cellWarpY - 0.23f, 9, 97u) * 0.22f;
				const float highCell = PeriodicCellNoise(u - cellWarpY, v + cellWarpX, 3, 101u) * 0.82f +
				                       PeriodicCellNoise(u - cellWarpY + 0.29f, v + cellWarpX + 0.11f, 7, 107u) * 0.18f;
				const float wispField = PeriodicFbm(u, v, 7, 139u);
				const float wispCross = PeriodicFbm(u + v, v - u, 6, 149u);
				const float wisp = std::pow(std::clamp(1.0f - std::abs(wispField * 2.0f - 1.0f), 0.0f, 1.0f), 4.0f) *
				                   std::clamp(0.55f + wispCross * 0.45f, 0.0f, 1.0f);
				const uint8_t cellByte = ToByte(cell);
				const uint8_t highCellByte = ToByte(highCell);
				const uint8_t wispByte = ToByte(wisp);
				scCell[i + 0] = scCell[i + 1] = scCell[i + 2] = cellByte;
				scCell[i + 3] = 255;
				hiCell[i + 0] = hiCell[i + 1] = hiCell[i + 2] = highCellByte;
				hiCell[i + 3] = 255;
				hiWarp[i + 0] = ToByte(PeriodicFbm(u, v, 2, 113u));
				hiWarp[i + 1] = ToByte(PeriodicFbm(u + 0.37f, v + 0.61f, 2, 127u));
				hiWarp[i + 2] = 128;
				hiWarp[i + 3] = 255;
				hiWisp[i + 0] = hiWisp[i + 1] = hiWisp[i + 2] = wispByte;
				hiWisp[i + 3] = 255;
			}
		}

		UploadGeneratedTexture(texScCell, weatherDim, weatherDim, "PhysicalSky::HpScCell", scCell);
		UploadGeneratedTexture(texHighCell, weatherDim, weatherDim, "PhysicalSky::HpHighCell", hiCell);
		UploadGeneratedTexture(texHighWarp, weatherDim, weatherDim, "PhysicalSky::HpHighWarp", hiWarp);
		UploadGeneratedTexture(texHighWisp, weatherDim, weatherDim, "PhysicalSky::HpHighWisp", hiWisp);
	}

	if (lowWeatherChanged) {
		std::vector<uint8_t> low(weatherPixelCount * 4u);
		std::vector<float> coverageValues(weatherPixelCount);
		const float lowCoverage = std::clamp(ndfSettings.lowCoverage, 0.0f, 1.0f);
		const float lowThreshold = 0.70f - lowCoverage * 0.48f;
		const float lowContrast = std::max(ndfSettings.lowContrast, 0.01f);
		const float scAmount = std::clamp(ndfSettings.stratocumulus, 0.0f, 1.0f);
		std::array<uint32_t, 256> scHistogram{};
		for (size_t pixel = 0; pixel < weatherPixelCount; ++pixel) {
			const float coverageMask = lowCoverage > 0.0f ? std::clamp((cachedCoverageField[pixel] - lowThreshold) / 0.28f, 0.0f, 1.0f) : 0.0f;
			coverageValues[pixel] = std::pow(coverageMask, lowContrast);
			if (coverageValues[pixel] >= 0.05f)
				++scHistogram[ScoreBin(cachedScRegion[pixel])];
		}
		const float scThreshold = HistogramSplitThreshold(scHistogram, 1.0f - scAmount);

		// Rank the remaining covered area by convective suitability. This keeps Cb
		// in moist, unstable regions while making the requested weights correspond
		// to actual map area instead of opaque noise thresholds.
		std::array<uint32_t, 256> typeHistogram{};
		for (size_t pixel = 0; pixel < weatherPixelCount; ++pixel) {
			const bool sc = coverageValues[pixel] >= 0.05f && cachedScRegion[pixel] >= scThreshold;
			if (coverageValues[pixel] >= 0.05f && !sc)
				++typeHistogram[ScoreBin(cachedCloudType[pixel])];
		}
		const float cuWeight = std::max(ndfSettings.cumulusWeight, 0.0f);
		const float tcuWeight = std::max(ndfSettings.toweringCumulusWeight, 0.0f);
		const float cbWeight = std::max(ndfSettings.cumulonimbusWeight, 0.0f);
		const float weightSum = cuWeight + tcuWeight + cbWeight;
		const float cuShare = weightSum > 1e-5f ? cuWeight / weightSum : 1.0f;
		const float tcuShare = weightSum > 1e-5f ? tcuWeight / weightSum : 0.0f;
		const float cuThreshold = HistogramSplitThreshold(typeHistogram, cuShare);
		const float tcuThreshold = HistogramSplitThreshold(typeHistogram, cuShare + tcuShare);

		for (size_t pixel = 0; pixel < weatherPixelCount; ++pixel) {
			const bool scRegion = coverageValues[pixel] >= 0.05f && cachedScRegion[pixel] >= scThreshold;
			const float typeScore = cachedCloudType[pixel];
			const float cloudType = scRegion ? 0.0f : (typeScore < cuThreshold ? 0.0f : (typeScore < tcuThreshold ? 0.5f : 1.0f));
			const size_t i = pixel * 4u;
			low[i + 0] = ToByte(coverageValues[pixel]);
			low[i + 1] = ToByte(cloudType);
			low[i + 2] = scRegion ? 255 : 0;
			low[i + 3] = 0;
		}
		UploadGeneratedTexture(texLowWeather, weatherDim, weatherDim, "PhysicalSky::HpLowWeather", low);
	}

	if (highWeatherChanged) {
		std::vector<uint8_t> high(weatherPixelCount * 4u);
		std::vector<float> highCoverageValues(weatherPixelCount);
		const float highCoverage = std::clamp(ndfSettings.highCoverage, 0.0f, 1.0f);
		const float highThreshold = 0.66f - 0.50f * highCoverage;
		const float highContrast = std::max(ndfSettings.highContrast, 0.01f);
		std::array<uint32_t, 256> highTypeHistogram{};
		for (size_t pixel = 0; pixel < weatherPixelCount; ++pixel) {
			const float highMask = highCoverage > 0.0f ? std::clamp((cachedHighField[pixel] - highThreshold) / 0.22f, 0.0f, 1.0f) : 0.0f;
			highCoverageValues[pixel] = std::pow(highMask, highContrast);
			if (highCoverageValues[pixel] >= 0.05f)
				++highTypeHistogram[ScoreBin(cachedHighType[pixel])];
		}
		const float asWeight = std::max(ndfSettings.altostratusWeight, 0.0f);
		const float acWeight = std::max(ndfSettings.altocumulusWeight, 0.0f);
		const float highWeightSum = asWeight + acWeight;
		const float asShare = highWeightSum > 1e-5f ? asWeight / highWeightSum : 1.0f;
		const float asThreshold = HistogramSplitThreshold(highTypeHistogram, asShare);
		for (size_t pixel = 0; pixel < weatherPixelCount; ++pixel) {
			const float hiCov = highCoverageValues[pixel];
			const float highType = cachedHighType[pixel] < asThreshold ? 0.0f : 1.0f;
			// A is the high-cloud multiple-scattering/thickness weight. It must be
			// zero outside the R coverage mask; the previous independent noise floor
			// affected low-cloud edge softness even where no high cloud existed.
			const float scatterWeight = hiCov * std::clamp(0.35f + cachedHighScatterField[pixel] * 0.65f, 0.0f, 1.0f);
			const size_t i = pixel * 4u;
			high[i + 0] = ToByte(hiCov);
			high[i + 1] = ToByte(highType);
			high[i + 2] = 0;
			high[i + 3] = ToByte(scatterWeight);
		}
		UploadGeneratedTexture(texHighWeather, weatherDim, weatherDim, "PhysicalSky::HpHighWeather", high);
	}

	if (profileChanged) {
		std::vector<uint8_t> profile(static_cast<size_t>(profileWidth) * profileHeight * 4u);
		for (uint32_t y = 0; y < profileHeight; ++y) {
			for (uint32_t x = 0; x < profileWidth; ++x) {
				const float h = (static_cast<float>(x) + 0.5f) / static_cast<float>(profileWidth);
				const float radial = (static_cast<float>(y) + 0.5f) / static_cast<float>(profileHeight);
				const auto profileForDepth = [&](float requestedDepth) {
					const float radialVariation = 1.0f - 0.12f * radial * radial;
					const float effectiveDepth = std::clamp(requestedDepth * radialVariation, 0.05f, layerDepth);
					const float localHeight = h * layerDepth / effectiveDepth;
					const float bottom = SmoothStep(0.0f, 0.10f, localHeight);
					const float top = 1.0f - SmoothStep(0.70f, 1.0f, localHeight);
					return bottom * top;
				};
				const float cu = profileForDepth(std::max(ndfSettings.cumulusDepth, 0.05f));
				const float tcu = profileForDepth(std::max(ndfSettings.toweringCumulusDepth, 0.05f));
				const float cb = profileForDepth(std::max(ndfSettings.cumulonimbusDepth, 0.05f));
				const size_t i = (static_cast<size_t>(y) * profileWidth + x) * 4u;
				profile[i + 0] = ToByte(cu);
				profile[i + 1] = ToByte(tcu);
				profile[i + 2] = ToByte(cb);
				profile[i + 3] = 255;
			}
		}
		UploadGeneratedTexture(texProfile, profileWidth, profileHeight, "PhysicalSky::HpProfile", profile);
	}

	generatedWeatherDim = weatherDim;
	generatedProfileWidth = profileWidth;
	generatedProfileHeight = profileHeight;
	generatedLowCoverage = ndfSettings.lowCoverage;
	generatedLowContrast = ndfSettings.lowContrast;
	generatedStratocumulus = ndfSettings.stratocumulus;
	generatedCumulusWeight = ndfSettings.cumulusWeight;
	generatedToweringCumulusWeight = ndfSettings.toweringCumulusWeight;
	generatedCumulonimbusWeight = ndfSettings.cumulonimbusWeight;
	generatedHighCoverage = ndfSettings.highCoverage;
	generatedHighContrast = ndfSettings.highContrast;
	generatedAltostratusWeight = ndfSettings.altostratusWeight;
	generatedAltocumulusWeight = ndfSettings.altocumulusWeight;
	generatedCumulusDepth = ndfSettings.cumulusDepth;
	generatedToweringCumulusDepth = ndfSettings.toweringCumulusDepth;
	generatedCumulonimbusDepth = ndfSettings.cumulonimbusDepth;
	generatedLayerDepth = layerDepth;
}

#undef I18N_KEY_PREFIX
