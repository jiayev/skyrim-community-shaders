#include "Features/PhysicalSky.h"

#include "I18n/I18n.h"
#include "State.h"
#include "Util.h"

#include <DDSTextureLoader.h>
#include <cmath>
#include <imgui_stdlib.h>

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
	GenerateDefaultTextures(HpGeneratedCloudMapSettings{});
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
	ImGui::SliderFloat(T(TKEY("stratocumulus"), "Stratocumulus"), &ndfSettings.stratocumulus, 0.f, 1.f, "%.2f");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("high_coverage"), "High Coverage"), &ndfSettings.highCoverage, 0.f, 1.f, "%.2f");
	deferWhileEditing();
	ImGui::SliderFloat(T(TKEY("high_contrast"), "High Contrast"), &ndfSettings.highContrast, 0.25f, 4.f, "%.2f");
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

void NdfManager::UpdateNdf(const NdfSettings& ndfSettings)
{
	GenerateDefaultTextures(ndfSettings);
}

ID3D11ShaderResourceView* NdfManager::GetNdf(const NdfSettings& ndfSettings, TextureManager& texManager)
{
	return GetHpTextures(ndfSettings, texManager).lowWeather;
}

HpCloudTextureSet NdfManager::GetHpTextures(const NdfSettings& ndfSettings, TextureManager& texManager)
{
	GenerateDefaultTextures(ndfSettings);
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

void NdfManager::GenerateDefaultTextures(const NdfSettings& ndfSettings)
{
	const uint32_t weatherDim = std::clamp(ndfSettings.weatherDim, 128u, 1024u);
	const uint32_t profileWidth = std::clamp(ndfSettings.profileWidth, 64u, 512u);
	const uint32_t profileHeight = std::clamp(ndfSettings.profileHeight, 16u, 256u);
	const bool haveGeneratedTextures = texLowWeather && texHighWeather && texProfile && texScCell && texHighCell && texHighWarp && texHighWisp;
	// While an ImGui control is actively dragged, keep rendering the last committed
	// maps. Rebuild once on release instead of synchronously regenerating every
	// intermediate slider value.
	if (ImGui::GetCurrentContext() && deferredGenerationFrame == ImGui::GetFrameCount() && haveGeneratedTextures)
		return;

	const size_t weatherPixelCount = static_cast<size_t>(weatherDim) * weatherDim;
	const bool weatherLayoutChanged = generatedWeatherDim != weatherDim || cachedCoverageField.size() != weatherPixelCount;
	const bool profileLayoutChanged = !texProfile || generatedProfileWidth != profileWidth || generatedProfileHeight != profileHeight;
	const bool lowWeatherChanged = weatherLayoutChanged || !texLowWeather ||
	                               generatedLowCoverage != ndfSettings.lowCoverage || generatedLowContrast != ndfSettings.lowContrast ||
	                               generatedStratocumulus != ndfSettings.stratocumulus;
	const bool highWeatherChanged = weatherLayoutChanged || !texHighWeather ||
	                                generatedHighCoverage != ndfSettings.highCoverage || generatedHighContrast != ndfSettings.highContrast;
	if (!weatherLayoutChanged && !profileLayoutChanged && !lowWeatherChanged && !highWeatherChanged)
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
				const float warpX = (Fbm(u * 2.5f + 13.0f, v * 2.5f - 7.0f, 3u) - 0.5f) * 0.20f;
				const float warpY = (Fbm(u * 2.5f - 5.0f, v * 2.5f + 11.0f, 5u) - 0.5f) * 0.20f;
				const float warpedU = u + warpX;
				const float warpedV = v + warpY;
				const float macro = Fbm(warpedU * 6.0f, warpedV * 6.0f, 11u);
				const float meso = Fbm(warpedU * 14.0f + 17.0f, warpedV * 14.0f, 23u);
				const float fine = Fbm(warpedU * 28.0f, warpedV * 28.0f + 9.0f, 37u);
				const float typeMacro = Fbm(warpedU * 3.0f + 31.0f, warpedV * 3.0f - 19.0f, 43u);
				const float typeMeso = Fbm(warpedU * 7.0f - 7.0f, warpedV * 7.0f + 23.0f, 47u);
				const float scMacro = Fbm(warpedU * 5.0f - 29.0f, warpedV * 5.0f + 5.0f, 59u);
				const float scMeso = Fbm(warpedU * 12.0f + 7.0f, warpedV * 12.0f - 13.0f, 61u);
				const float highMacro = Fbm((u + warpX * 0.5f) * 4.0f + 3.0f, (v + warpY * 0.5f) * 4.0f - 5.0f, 71u);
				const float highMeso = Fbm(warpedU * 10.0f - 17.0f, warpedV * 10.0f + 29.0f, 73u);

				const size_t pixel = static_cast<size_t>(y) * weatherDim + x;
				cachedCoverageField[pixel] = macro * 0.68f + meso * 0.24f + fine * 0.08f;
				// HP expects G to select distinct Cu/Tcu/Cb profile families. A raw
				// continuous noise field spends most of the map blending profiles and
				// visually collapses them into one generic cloud. Keep broad categorical
				// plateaus with narrow, filterable transitions between the three types.
				const float typeDriver = (typeMacro * 0.72f + typeMeso * 0.28f) * 0.72f + cachedCoverageField[pixel] * 0.28f;
				const float cuToTcu = SmoothStep(0.40f, 0.47f, typeDriver);
				const float tcuToCb = SmoothStep(0.54f, 0.62f, typeDriver);
				cachedCloudType[pixel] = std::lerp(std::lerp(0.08f, 0.50f, cuToTcu), 0.92f, tcuToCb);
				cachedScRegion[pixel] = scMacro * 0.75f + scMeso * 0.25f;
				cachedHighField[pixel] = highMacro * 0.8f + highMeso * 0.2f;
				// High G is an As/Ac selector, not a generic blend weight. Preserve
				// coherent regions of both cloud families.
				cachedHighType[pixel] = SmoothStep(0.48f, 0.60f, Fbm(warpedU * 5.0f + 41.0f, warpedV * 5.0f - 37.0f, 79u));
				cachedHighScatterField[pixel] = highMacro * 0.7f + highMeso * 0.3f;

				const size_t i = pixel * 4u;
				const uint8_t cell = ToByte(PeriodicCellNoise(u, v, 4, 89u));
				const uint8_t highCell = ToByte(PeriodicCellNoise(u, v, 3, 101u));
				const uint8_t wisp = ToByte(std::pow(std::abs(PeriodicFbm(u, v, 8, 139u) * 2.0f - 1.0f), 2.0f));
				scCell[i + 0] = scCell[i + 1] = scCell[i + 2] = cell;
				scCell[i + 3] = 255;
				hiCell[i + 0] = hiCell[i + 1] = hiCell[i + 2] = highCell;
				hiCell[i + 3] = 255;
				hiWarp[i + 0] = ToByte(PeriodicFbm(u, v, 2, 113u));
				hiWarp[i + 1] = ToByte(PeriodicFbm(u + 0.37f, v + 0.61f, 2, 127u));
				hiWarp[i + 2] = 128;
				hiWarp[i + 3] = 255;
				hiWisp[i + 0] = hiWisp[i + 1] = hiWisp[i + 2] = wisp;
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
		const float lowCoverage = std::clamp(ndfSettings.lowCoverage, 0.0f, 1.0f);
		const float lowThreshold = 0.70f - lowCoverage * 0.48f;
		const float lowContrast = std::max(ndfSettings.lowContrast, 0.01f);
		const float scAmount = std::clamp(ndfSettings.stratocumulus, 0.0f, 1.0f);
		const float scThreshold = 0.72f - scAmount * 0.44f;
		for (size_t pixel = 0; pixel < weatherPixelCount; ++pixel) {
			const float coverageMask = lowCoverage > 0.0f ? std::clamp((cachedCoverageField[pixel] - lowThreshold) / 0.28f, 0.0f, 1.0f) : 0.0f;
			const float coverage = std::pow(coverageMask, lowContrast);
			const float cloudType = cachedCloudType[pixel];
			// HP low B is a spatial Sc mask. Encode full-strength regions and let the
			// independent ScWorleyStrength parameter control blending in the shader;
			// treating this setting as another amplitude multiplier attenuated it twice.
			const float scRegion = scAmount > 0.0f ? SmoothStep(scThreshold, scThreshold + 0.16f, cachedScRegion[pixel]) : 0.0f;
			const float sc = scRegion * (1.0f - SmoothStep(0.30f, 0.58f, cloudType));
			const size_t i = pixel * 4u;
			low[i + 0] = ToByte(coverage);
			low[i + 1] = ToByte(cloudType);
			low[i + 2] = ToByte(sc);
			low[i + 3] = 0;
		}
		UploadGeneratedTexture(texLowWeather, weatherDim, weatherDim, "PhysicalSky::HpLowWeather", low);
	}

	if (highWeatherChanged) {
		std::vector<uint8_t> high(weatherPixelCount * 4u);
		const float highCoverage = std::clamp(ndfSettings.highCoverage, 0.0f, 1.0f);
		const float highThreshold = 0.70f - 0.50f * highCoverage;
		const float highContrast = std::max(ndfSettings.highContrast, 0.01f);
		for (size_t pixel = 0; pixel < weatherPixelCount; ++pixel) {
			const float highMask = highCoverage > 0.0f ? std::clamp((cachedHighField[pixel] - highThreshold) / 0.22f, 0.0f, 1.0f) : 0.0f;
			const float hiCov = std::pow(highMask, highContrast);
			// A is the high-cloud multiple-scattering/thickness weight. It must be
			// zero outside the R coverage mask; the previous independent noise floor
			// affected low-cloud edge softness even where no high cloud existed.
			const float scatterWeight = hiCov * std::clamp(0.35f + cachedHighScatterField[pixel] * 0.65f, 0.0f, 1.0f);
			const size_t i = pixel * 4u;
			high[i + 0] = ToByte(hiCov);
			high[i + 1] = ToByte(cachedHighType[pixel]);
			high[i + 2] = 0;
			high[i + 3] = ToByte(scatterWeight);
		}
		UploadGeneratedTexture(texHighWeather, weatherDim, weatherDim, "PhysicalSky::HpHighWeather", high);
	}

	if (profileLayoutChanged) {
		std::vector<uint8_t> profile(static_cast<size_t>(profileWidth) * profileHeight * 4u);
		for (uint32_t y = 0; y < profileHeight; ++y) {
			for (uint32_t x = 0; x < profileWidth; ++x) {
				const float h = (static_cast<float>(x) + 0.5f) / static_cast<float>(profileWidth);
				const float radial = (static_cast<float>(y) + 0.5f) / static_cast<float>(profileHeight);
				const float bottom = SmoothStep(0.0f, 0.08f, h);
				// RGB are complete, deliberately distinct vertical profiles as required
				// by HP: Cu is shallow, Tcu occupies most of the middle slab, and Cb
				// reaches the top. Radial attenuation keeps the finite weather-map edge
				// from ending in equally tall columns without changing channel meaning.
				const float radialTopShift = 0.08f * radial * radial;
				const float cu = bottom * (1.0f - SmoothStep(0.34f - radialTopShift, 0.48f - radialTopShift, h));
				const float tcu = bottom * (1.0f - SmoothStep(0.52f - radialTopShift, 0.66f - radialTopShift, h));
				const float cb = bottom * (1.0f - SmoothStep(0.86f - radialTopShift, 0.99f - radialTopShift, h));
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
	generatedHighCoverage = ndfSettings.highCoverage;
	generatedHighContrast = ndfSettings.highContrast;
}

#undef I18N_KEY_PREFIX
