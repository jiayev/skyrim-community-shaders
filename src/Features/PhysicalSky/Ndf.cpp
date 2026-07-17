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
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};

		D3D11_SUBRESOURCE_DATA initData{
			.pSysMem = data.data(),
			.SysMemPitch = width * 4u,
			.SysMemSlicePitch = width * height * 4u
		};

		winrt::com_ptr<ID3D11Texture2D> raw;
		DX::ThrowIfFailed(device->CreateTexture2D(&desc, &initData, raw.put()));

		auto tex = eastl::make_unique<Texture2D>(raw.detach(), name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		tex->CreateSRV(srvDesc);
		return tex;
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
	ImGui::TextWrapped("%s", GetSettingsHint(ndfSettings));
	ImGui::SeparatorText(T(TKEY("generated_weather"), "Generated Weather"));
	uint32_t weatherMin = 128u;
	uint32_t weatherMax = 1024u;
	uint32_t profileWidthMin = 64u;
	uint32_t profileWidthMax = 512u;
	uint32_t profileHeightMin = 16u;
	uint32_t profileHeightMax = 256u;
	ImGui::SliderScalar(T(TKEY("weather_dimension"), "Weather Dimension"), ImGuiDataType_U32, &ndfSettings.weatherDim, &weatherMin, &weatherMax, "%u");
	ImGui::SliderScalar(T(TKEY("profile_width"), "Profile Width"), ImGuiDataType_U32, &ndfSettings.profileWidth, &profileWidthMin, &profileWidthMax, "%u");
	ImGui::SliderScalar(T(TKEY("profile_height"), "Profile Height"), ImGuiDataType_U32, &ndfSettings.profileHeight, &profileHeightMin, &profileHeightMax, "%u");
	ImGui::SliderFloat(T(TKEY("weather_world_size"), "Weather World Size"), &ndfSettings.worldSize, 8.f, 256.f, "%.1f km");
	ImGui::SliderFloat2(T(TKEY("weather_center"), "Weather Center"), &ndfSettings.center.x, -256.f, 256.f, "%.1f km");
	ImGui::SliderFloat(T(TKEY("low_coverage"), "Low Coverage"), &ndfSettings.lowCoverage, 0.f, 1.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("low_contrast"), "Low Contrast"), &ndfSettings.lowContrast, 0.25f, 4.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("stratocumulus"), "Stratocumulus"), &ndfSettings.stratocumulus, 0.f, 1.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("high_coverage"), "High Coverage"), &ndfSettings.highCoverage, 0.f, 1.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("high_contrast"), "High Contrast"), &ndfSettings.highContrast, 0.25f, 4.f, "%.2f");

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
	return {
		.lowWeather = texManager.Query(ndfSettings.overrides.lowWeatherPath) ? texManager.Query(ndfSettings.overrides.lowWeatherPath) : texLowWeather->srv.get(),
		.highWeather = texManager.Query(ndfSettings.overrides.highWeatherPath) ? texManager.Query(ndfSettings.overrides.highWeatherPath) : texHighWeather->srv.get(),
		.profile = texManager.Query(ndfSettings.overrides.profilePath) ? texManager.Query(ndfSettings.overrides.profilePath) : texProfile->srv.get(),
		.scCell = texManager.Query(ndfSettings.overrides.scCellPath) ? texManager.Query(ndfSettings.overrides.scCellPath) : texScCell->srv.get(),
		.highCell = texManager.Query(ndfSettings.overrides.highCellPath) ? texManager.Query(ndfSettings.overrides.highCellPath) : texHighCell->srv.get(),
		.highWarp = texManager.Query(ndfSettings.overrides.highWarpPath) ? texManager.Query(ndfSettings.overrides.highWarpPath) : texHighWarp->srv.get(),
		.highWisp = texManager.Query(ndfSettings.overrides.highWispPath) ? texManager.Query(ndfSettings.overrides.highWispPath) : texHighWisp->srv.get(),
	};
}

void NdfManager::GenerateDefaultTextures(const NdfSettings& ndfSettings)
{
	const uint32_t weatherDim = std::clamp(ndfSettings.weatherDim, 128u, 1024u);
	const uint32_t profileWidth = std::clamp(ndfSettings.profileWidth, 64u, 512u);
	const uint32_t profileHeight = std::clamp(ndfSettings.profileHeight, 16u, 256u);
	if (texLowWeather &&
		generatedWeatherDim == weatherDim && generatedProfileWidth == profileWidth && generatedProfileHeight == profileHeight &&
		generatedLowCoverage == ndfSettings.lowCoverage && generatedLowContrast == ndfSettings.lowContrast &&
		generatedStratocumulus == ndfSettings.stratocumulus &&
		generatedHighCoverage == ndfSettings.highCoverage && generatedHighContrast == ndfSettings.highContrast)
		return;

	generatedWeatherDim = weatherDim;
	generatedProfileWidth = profileWidth;
	generatedProfileHeight = profileHeight;
	generatedLowCoverage = ndfSettings.lowCoverage;
	generatedLowContrast = ndfSettings.lowContrast;
	generatedStratocumulus = ndfSettings.stratocumulus;
	generatedHighCoverage = ndfSettings.highCoverage;
	generatedHighContrast = ndfSettings.highContrast;

	std::vector<uint8_t> low(weatherDim * weatherDim * 4u);
	std::vector<uint8_t> high(weatherDim * weatherDim * 4u);
	std::vector<uint8_t> scCell(weatherDim * weatherDim * 4u);
	std::vector<uint8_t> hiCell(weatherDim * weatherDim * 4u);
	std::vector<uint8_t> hiWarp(weatherDim * weatherDim * 4u);
	std::vector<uint8_t> hiWisp(weatherDim * weatherDim * 4u);

	for (uint32_t y = 0; y < weatherDim; ++y) {
		for (uint32_t x = 0; x < weatherDim; ++x) {
			const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(weatherDim);
			const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(weatherDim);
			// Build the large-scale distribution first, then add smaller variation. The
			// former generator started at a much higher frequency and imposed a radial
			// stratocumulus mask, making the whole cloud field visibly centre-biased.
			const float warpX = (Fbm(u * 2.5f + 13.0f, v * 2.5f - 7.0f, 3u) - 0.5f) * 0.20f;
			const float warpY = (Fbm(u * 2.5f - 5.0f, v * 2.5f + 11.0f, 5u) - 0.5f) * 0.20f;
			const float warpedU = u + warpX;
			const float warpedV = v + warpY;
			// At the default 64 km map extent these bands are roughly 10.7 km,
			// 4.6 km, and 2.3 km across. That keeps horizontal weather features in
			// proportion with the 2.5 km shared cloud slab instead of producing
			// tens-of-kilometres-wide flat sheets.
			const float macro = Fbm(warpedU * 6.0f, warpedV * 6.0f, 11u);
			const float meso = Fbm(warpedU * 14.0f + 17.0f, warpedV * 14.0f, 23u);
			const float fine = Fbm(warpedU * 28.0f, warpedV * 28.0f + 9.0f, 37u);
			const float coverageField = macro * 0.68f + meso * 0.24f + fine * 0.08f;
			const float lowCoverage = std::clamp(ndfSettings.lowCoverage, 0.0f, 1.0f);
			const float lowThreshold = 0.70f - lowCoverage * 0.48f;
			const float coverageMask = lowCoverage > 0.0f ? std::clamp((coverageField - lowThreshold) / 0.28f, 0.0f, 1.0f) : 0.0f;
			const float coverage = std::pow(coverageMask, std::max(ndfSettings.lowContrast, 0.01f));

			const float typeMacro = Fbm(warpedU * 3.0f + 31.0f, warpedV * 3.0f - 19.0f, 43u);
			const float typeMeso = Fbm(warpedU * 7.0f - 7.0f, warpedV * 7.0f + 23.0f, 47u);
			const float cloudType = SmoothStep(0.28f, 0.78f, typeMacro * 0.72f + typeMeso * 0.28f);
			const float scMacro = Fbm(warpedU * 5.0f - 29.0f, warpedV * 5.0f + 5.0f, 59u);
			const float scMeso = Fbm(warpedU * 12.0f + 7.0f, warpedV * 12.0f - 13.0f, 61u);
			const float scRegion = SmoothStep(0.34f, 0.72f, scMacro * 0.75f + scMeso * 0.25f);
			const float sc = scRegion * (1.0f - SmoothStep(0.55f, 0.95f, cloudType)) * std::clamp(ndfSettings.stratocumulus, 0.0f, 1.0f);

			// High clouds use their own broad mask and a continuous type field. A hard
			// binary type boundary caused abrupt changes in band thickness and erosion.
			const float highMacro = Fbm((u + warpX * 0.5f) * 4.0f + 3.0f, (v + warpY * 0.5f) * 4.0f - 5.0f, 71u);
			const float highMeso = Fbm(warpedU * 10.0f - 17.0f, warpedV * 10.0f + 29.0f, 73u);
			const float highField = highMacro * 0.8f + highMeso * 0.2f;
			const float highCoverage = std::clamp(ndfSettings.highCoverage, 0.0f, 1.0f);
			const float highThreshold = 0.70f - 0.50f * highCoverage;
			const float highMask = highCoverage > 0.0f ? std::clamp((highField - highThreshold) / 0.22f, 0.0f, 1.0f) : 0.0f;
			const float hiCov = std::pow(highMask, std::max(ndfSettings.highContrast, 0.01f));
			const float hiType = SmoothStep(0.30f, 0.75f, Fbm(warpedU * 5.0f + 41.0f, warpedV * 5.0f - 37.0f, 79u));

			// These textures are generic repeatable motifs; their world frequency is
			// controlled later by the corresponding scale parameters.
			const float cell = PeriodicCellNoise(u, v, 4, 89u);
			const float hiCellValue = PeriodicCellNoise(u, v, 3, 101u);
			const float warpR = PeriodicFbm(u, v, 2, 113u);
			const float warpG = PeriodicFbm(u + 0.37f, v + 0.61f, 2, 127u);
			const float wisp = std::pow(std::abs(PeriodicFbm(u, v, 8, 139u) * 2.0f - 1.0f), 2.0f);
			const float scatterWeight = std::clamp(hiCov * 0.65f + (highMacro * 0.7f + highMeso * 0.3f) * 0.35f, 0.0f, 1.0f);

			const size_t i = (static_cast<size_t>(y) * weatherDim + x) * 4u;
			low[i + 0] = ToByte(coverage);
			low[i + 1] = ToByte(cloudType);
			low[i + 2] = ToByte(sc);
			low[i + 3] = 0;
			high[i + 0] = ToByte(hiCov);
			high[i + 1] = ToByte(hiType);
			high[i + 2] = 0;
			high[i + 3] = ToByte(scatterWeight);
			scCell[i + 0] = scCell[i + 1] = scCell[i + 2] = ToByte(cell);
			scCell[i + 3] = 255;
			hiCell[i + 0] = hiCell[i + 1] = hiCell[i + 2] = ToByte(hiCellValue);
			hiCell[i + 3] = 255;
			hiWarp[i + 0] = ToByte(warpR);
			hiWarp[i + 1] = ToByte(warpG);
			hiWarp[i + 2] = 128;
			hiWarp[i + 3] = 255;
			hiWisp[i + 0] = hiWisp[i + 1] = hiWisp[i + 2] = ToByte(wisp);
			hiWisp[i + 3] = 255;
		}
	}

	std::vector<uint8_t> profile(profileWidth * profileHeight * 4u);
	for (uint32_t y = 0; y < profileHeight; ++y) {
		for (uint32_t x = 0; x < profileWidth; ++x) {
			const float h = (static_cast<float>(x) + 0.5f) / static_cast<float>(profileWidth);
			// All three profiles use the complete normalized layer. Cloud type changes
			// how strongly density is concentrated around the middle of that layer; it
			// must not silently reinterpret the authored physical layer thickness.
			// Rows stay identical so the generated fallback does not introduce a radial
			// height bias. Custom profile textures may still use the second coordinate.
			const float layerShape = std::sin(std::clamp(h, 0.0f, 1.0f) * 3.14159265f);
			const float cu = layerShape;
			const float tcu = std::pow(layerShape, 0.65f);
			const float cb = std::pow(layerShape, 0.35f);
			const size_t i = (static_cast<size_t>(y) * profileWidth + x) * 4u;
			profile[i + 0] = ToByte(cu);
			profile[i + 1] = ToByte(tcu);
			profile[i + 2] = ToByte(cb);
			profile[i + 3] = 255;
		}
	}

	texLowWeather = CreateGeneratedTexture(weatherDim, weatherDim, "PhysicalSky::HpLowWeather", low);
	texHighWeather = CreateGeneratedTexture(weatherDim, weatherDim, "PhysicalSky::HpHighWeather", high);
	texProfile = CreateGeneratedTexture(profileWidth, profileHeight, "PhysicalSky::HpProfile", profile);
	texScCell = CreateGeneratedTexture(weatherDim, weatherDim, "PhysicalSky::HpScCell", scCell);
	texHighCell = CreateGeneratedTexture(weatherDim, weatherDim, "PhysicalSky::HpHighCell", hiCell);
	texHighWarp = CreateGeneratedTexture(weatherDim, weatherDim, "PhysicalSky::HpHighWarp", hiWarp);
	texHighWisp = CreateGeneratedTexture(weatherDim, weatherDim, "PhysicalSky::HpHighWisp", hiWisp);
}

#undef I18N_KEY_PREFIX
