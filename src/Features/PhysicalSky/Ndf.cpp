#include "Features/PhysicalSky.h"

#include "I18n/I18n.h"
#include "State.h"
#include "Util.h"

#include <DDSTextureLoader.h>
#include <algorithm>
#include <cmath>
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
	constexpr uint32_t kHistogramBins = 256u;
	constexpr uint32_t kHistogramGroups = 5u;
	constexpr uint32_t kThresholdCount = 6u;

	void HashCombine(size_t& seed, size_t value)
	{
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
	}

	void HashValue(size_t& seed, float value)
	{
		HashCombine(seed, std::hash<float>{}(value));
	}

	void HashValue(size_t& seed, uint32_t value)
	{
		HashCombine(seed, std::hash<uint32_t>{}(value));
	}

	/**
	 * @brief Creates an R8G8B8A8_UNORM texture usable as both a compute target and
	 *        a mipped shader input.
	 *
	 * The UAV format must match the resource format for typed UNORM writes, and
	 * the SRV must expose the full chain because the renderer samples High Weather
	 * at mip 2 as a whole-ray high-cloud gate.
	 */
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

		auto tex = eastl::make_unique<Texture2D>(desc, name);
		tex->CreateSRV({
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = static_cast<UINT>(-1) },
		});
		tex->CreateUAV({
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 },
		});
		return tex;
	}

	/** @brief Creates a transient full-precision field target for the generator. */
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

		auto tex = eastl::make_unique<Texture2D>(desc, name);
		tex->CreateSRV({
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 },
		});
		tex->CreateUAV({
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 },
		});
		return tex;
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

	/**
	 * @brief Draws a slider for a normalized [0, 1] setting using percent units.
	 *
	 * ImGui formats the raw backing value, so a 0-1 fraction shown with a percent
	 * format string would display 0.38 as "0 %". Editing a scaled copy keeps the
	 * stored value normalized while the control reads and drags in percent.
	 */
	void SliderPercent(const char* label, float& normalizedValue)
	{
		float percent = std::clamp(normalizedValue, 0.0f, 1.0f) * 100.0f;
		if (ImGui::SliderFloat(label, &percent, 0.0f, 100.0f, "%.0f %%"))
			normalizedValue = std::clamp(percent, 0.0f, 100.0f) / 100.0f;
	}

	// Temporary authoring aid: one-click starting points for the generator.
	//
	// A preset only touches the fields that describe the weather itself. Map
	// resolution, world size, centre, seed, and texture overrides are deliberately
	// left alone so applying one does not discard the user's rig or reshuffle the
	// pattern they are looking at.
	struct CloudMapPreset
	{
		// Localized strings are not stored here: tools/extract-i18n.py only sees
		// literal T("key", "default") pairs, so they live in DrawPresetLabel below.
		int id;

		float skyCoverage;
		float cloudSize;
		float instability;
		float character;
		float breakup;
		float highCoverage;

		float coverageEdgeWidth;
		float highCoverageEdgeWidth;
		float frontStrength;
		float domeStrength;

		float stratocumulus;
		float cumulusWeight;
		float toweringCumulusWeight;
		float cumulonimbusWeight;

		float cumulusDepth;
		float toweringCumulusDepth;
		float cumulonimbusDepth;

		float altostratusWeight;
		float altocumulusWeight;
	};

	constexpr std::array kCloudMapPresets = {
		// Fair-weather cumulus: small, well separated, shallow, no deep convection.
		CloudMapPreset{ 0,
			0.22f, 1.6f, 0.20f, 0.10f, 0.65f, 0.12f,
			0.35f, 0.5f, 0.10f, 0.95f,
			0.05f, 0.90f, 0.10f, 0.00f,
			0.9f, 2.0f, 6.0f,
			0.55f, 0.45f },
		// Broken cumulus: a lively midpoint, more sky covered and some towers.
		CloudMapPreset{ 1,
			0.45f, 2.8f, 0.45f, 0.25f, 0.45f, 0.28f,
			0.45f, 0.5f, 0.25f, 0.90f,
			0.15f, 0.60f, 0.32f, 0.08f,
			1.3f, 3.2f, 8.0f,
			0.60f, 0.40f },
		// Overcast stratocumulus: continuous sheet, almost no vertical development.
		CloudMapPreset{ 2,
			0.92f, 7.0f, 0.08f, 0.92f, 0.15f, 0.45f,
			0.75f, 0.7f, 0.35f, 0.25f,
			0.85f, 0.15f, 0.00f, 0.00f,
			0.7f, 1.5f, 4.0f,
			0.85f, 0.15f },
		// Warm front: layered stratiform bands plus a heavy altostratus veil.
		CloudMapPreset{ 3,
			0.70f, 5.0f, 0.22f, 0.75f, 0.25f, 0.78f,
			0.65f, 0.8f, 0.85f, 0.45f,
			0.55f, 0.30f, 0.15f, 0.00f,
			1.0f, 2.6f, 7.0f,
			0.90f, 0.10f },
		// Storm: deep convection, large cells, wide separation, anvil-heavy above.
		CloudMapPreset{ 4,
			0.55f, 6.0f, 0.95f, 0.15f, 0.70f, 0.60f,
			0.40f, 0.6f, 0.45f, 0.80f,
			0.05f, 0.20f, 0.35f, 0.45f,
			1.8f, 5.0f, 12.0f,
			0.45f, 0.55f },
	};

	/** @brief Localized button label for a preset. */
	const char* GetPresetLabel(int id)
	{
		switch (id) {
		case 0:
			return T(TKEY("preset_fair"), "Fair Weather");
		case 1:
			return T(TKEY("preset_broken"), "Broken Cumulus");
		case 2:
			return T(TKEY("preset_overcast"), "Overcast");
		case 3:
			return T(TKEY("preset_front"), "Frontal Band");
		default:
			return T(TKEY("preset_storm"), "Thunderstorm");
		}
	}

	/** @brief Localized hover description for a preset. */
	const char* GetPresetTooltip(int id)
	{
		switch (id) {
		case 0:
			return T(TKEY("preset_fair_tooltip"), "Scattered fair-weather cumulus. Small, well-separated, shallow clouds under a mostly clear sky.");
		case 1:
			return T(TKEY("preset_broken_tooltip"), "A lively convective sky. Larger cumulus with some towering development and moderate high cloud.");
		case 2:
			return T(TKEY("preset_overcast_tooltip"), "A continuous stratocumulus deck. Nearly full coverage, flat, with very little vertical development.");
		case 3:
			return T(TKEY("preset_front_tooltip"), "An approaching warm front. Layered stratiform bands with an extensive altostratus veil above.");
		default:
			return T(TKEY("preset_storm_tooltip"), "Deep convection. Large, widely separated cells reaching cumulonimbus depth with heavy anvil outflow.");
		}
	}

	void ApplyCloudMapPreset(NdfSettings& s, const CloudMapPreset& p)
	{
		s.skyCoverage = p.skyCoverage;
		s.cloudSize = p.cloudSize;
		s.instability = p.instability;
		s.character = p.character;
		s.breakup = p.breakup;
		s.highCoverage = p.highCoverage;

		s.coverageEdgeWidth = p.coverageEdgeWidth;
		s.highCoverageEdgeWidth = p.highCoverageEdgeWidth;
		s.frontStrength = p.frontStrength;
		s.domeStrength = p.domeStrength;

		s.stratocumulus = p.stratocumulus;
		s.cumulusWeight = p.cumulusWeight;
		s.toweringCumulusWeight = p.toweringCumulusWeight;
		s.cumulonimbusWeight = p.cumulonimbusWeight;

		s.cumulusDepth = p.cumulusDepth;
		s.toweringCumulusDepth = p.toweringCumulusDepth;
		s.cumulonimbusDepth = p.cumulonimbusDepth;

		s.altostratusWeight = p.altostratusWeight;
		s.altocumulusWeight = p.altocumulusWeight;
	}
}

void NdfManager::SetupResources()
{
	logger::debug("Creating generated cloud map resources...");

	cbGen = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<GenCB>(), "PhysicalSky::CloudMapGenCB");

	{
		D3D11_BUFFER_DESC desc{
			.ByteWidth = kHistogramBins * kHistogramGroups * sizeof(uint32_t),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
			.StructureByteStride = 0
		};
		bufHistogram = eastl::make_unique<Buffer>(desc, nullptr, "PhysicalSky::CloudMapHistogram");
		bufHistogram->CreateUAV({
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = { .FirstElement = 0, .NumElements = kHistogramBins * kHistogramGroups, .Flags = D3D11_BUFFER_UAV_FLAG_RAW },
		});
	}

	{
		D3D11_BUFFER_DESC desc{
			.ByteWidth = kThresholdCount * sizeof(float),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
			.StructureByteStride = sizeof(float)
		};
		bufThresholds = eastl::make_unique<Buffer>(desc, nullptr, "PhysicalSky::CloudMapThresholds");
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
	}
}

void NdfManager::CompileShaders()
{
	logger::debug("Compiling cloud map generation shaders...");

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
		PassInfo{ &csProfile, "4" },
	};

	const auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky\\CloudMapGen.cs.hlsl");
	for (const auto& pass : passes) {
		*pass.target = nullptr;
		std::vector<std::pair<const char*, const char*>> defines{ { "CLOUDMAPGEN", pass.mode } };
		if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), defines, "cs_5_0", "main")))
			pass.target->attach(raw);
	}

	// Force a rebuild: freshly compiled shaders have not written the textures yet.
	generatedHash = 0;
}

bool NdfManager::ShadersReady() const
{
	return csFields && csHistogram && csSolve && csCompose && csProfile;
}

const char* NdfManager::GetSettingsTypeName(const NdfSettings&)
{
	return T(TKEY("generated"), "Generated");
}

const char* NdfManager::GetSettingsHint(const NdfSettings&)
{
	return T(TKEY("generated_cloud_map_hint"), "Generates weather, profile, cell, warp, and wisp textures on the GPU. Loaded DDS textures can override each generated input.");
}

void NdfManager::DrawNdfSettings(NdfSettings& ndfSettings, TextureManager& texManager)
{
	ImGui::TextWrapped("%s", GetSettingsHint(ndfSettings));

	ImGui::SeparatorText(T(TKEY("cloud_map_presets"), "Presets"));
	{
		// Buttons wrap to the panel width rather than assuming a fixed column count.
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float available = ImGui::GetContentRegionAvail().x;
		float lineWidth = 0.0f;
		for (size_t i = 0; i < kCloudMapPresets.size(); ++i) {
			const auto& preset = kCloudMapPresets[i];
			const char* label = GetPresetLabel(preset.id);
			const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			if (i > 0 && lineWidth + spacing + width <= available) {
				ImGui::SameLine();
				lineWidth += spacing + width;
			} else {
				lineWidth = width;
			}
			if (ImGui::Button(label))
				ApplyCloudMapPreset(ndfSettings, preset);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", GetPresetTooltip(preset.id));
		}
		ImGui::TextWrapped("%s", T(TKEY("cloud_map_presets_hint"), "Presets only set the weather description. Map resolution, world size, centre, seed, and texture overrides are left unchanged."));
	}

	ImGui::SeparatorText(T(TKEY("cloud_field"), "Cloud Field"));
	{
		SliderPercent(T(TKEY("sky_coverage"), "Sky Coverage"), ndfSettings.skyCoverage);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("sky_coverage_tooltip"), "Fraction of the sky carrying low cloud. Solved exactly, so the value matches what you see."));

		ImGui::SliderFloat(T(TKEY("cloud_size"), "Cloud Size"), &ndfSettings.cloudSize, 0.4f, 24.f, "%.1f km", ImGuiSliderFlags_Logarithmic);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("cloud_size_tooltip"), "Horizontal diameter of a single convective cloud body."));

		ImGui::SliderFloat(T(TKEY("instability"), "Instability"), &ndfSettings.instability, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("instability_tooltip"), "Atmospheric instability. Higher values shift the species mix toward towering cumulus and cumulonimbus and deepen every cloud."));

		ImGui::SliderFloat(T(TKEY("character"), "Character"), &ndfSettings.character, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("character_tooltip"), "0 = discrete convective cells, 1 = continuous stratiform sheets."));

		ImGui::SliderFloat(T(TKEY("breakup"), "Break-up"), &ndfSettings.breakup, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("breakup_tooltip"), "Separation between cloud bodies. Coverage stays fixed: the same cloud area is redistributed into fewer, denser bodies."));

		SliderPercent(T(TKEY("high_coverage"), "High Cloud Coverage"), ndfSettings.highCoverage);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("high_coverage_tooltip"), "Fraction of the sky carrying altostratus and altocumulus."));
	}

	if (ImGui::CollapsingHeader(T(TKEY("cloud_map_advanced"), "Advanced"))) {
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
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("weather_world_size_tooltip"), "Side length of one weather-map tile. The map repeats seamlessly, so this sets the scale at which the synoptic pattern recurs."));
		ImGui::SliderFloat2(T(TKEY("weather_center"), "Weather Center"), &ndfSettings.center.x, -256.f, 256.f, "%.1f km");
		uint32_t seedMin = 0u;
		uint32_t seedMax = 65535u;
		ImGui::SliderScalar(T(TKEY("cloud_map_seed"), "Pattern Seed"), ImGuiDataType_U32, &ndfSettings.seed, &seedMin, &seedMax, "%u");

		ImGui::SeparatorText(T(TKEY("cloud_field_shaping"), "Field Shaping"));
		ImGui::SliderFloat(T(TKEY("coverage_edge_width"), "Coverage Edge Width"), &ndfSettings.coverageEdgeWidth, 0.05f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("coverage_edge_width_tooltip"), "Softness of the cloud edge. Cannot change how much sky is covered."));
		ImGui::SliderFloat(T(TKEY("high_coverage_edge_width"), "High Coverage Edge Width"), &ndfSettings.highCoverageEdgeWidth, 0.05f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("front_strength"), "Frontal Band Strength"), &ndfSettings.frontStrength, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("front_bearing"), "Frontal Band Bearing"), &ndfSettings.frontBearing, 0.f, 180.f, "%.0f deg");
		ImGui::SliderFloat(T(TKEY("dome_strength"), "Dome Strength"), &ndfSettings.domeStrength, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("dome_strength_tooltip"), "How strongly a cloud thins toward its own edge. 0 produces flat slabs."));

		ImGui::SeparatorText(T(TKEY("low_cloud_species"), "Low Cloud Species"));
		ImGui::SliderFloat(T(TKEY("stratocumulus_share"), "Stratocumulus Share"), &ndfSettings.stratocumulus, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("cumulus_weight"), "Cumulus Weight"), &ndfSettings.cumulusWeight, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("towering_cumulus_weight"), "Towering Cumulus Weight"), &ndfSettings.toweringCumulusWeight, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("cumulonimbus_weight"), "Cumulonimbus Weight"), &ndfSettings.cumulonimbusWeight, 0.f, 1.f, "%.2f");
		ImGui::TextWrapped("%s", T(TKEY("cloud_species_weight_hint"), "Weights are normalized within cloud-covered regions and biased by Instability. Set a weight to zero to disable that species."));
		ImGui::SliderFloat(T(TKEY("cumulus_vertical_depth"), "Cumulus Vertical Depth"), &ndfSettings.cumulusDepth, 0.2f, 5.f, "%.2f km");
		ImGui::SliderFloat(T(TKEY("towering_cumulus_vertical_depth"), "Towering Cumulus Vertical Depth"), &ndfSettings.toweringCumulusDepth, 0.5f, 10.f, "%.2f km");
		ImGui::SliderFloat(T(TKEY("cumulonimbus_vertical_depth"), "Cumulonimbus Vertical Depth"), &ndfSettings.cumulonimbusDepth, 1.f, 16.f, "%.2f km");

		ImGui::SeparatorText(T(TKEY("high_cloud_species"), "High Cloud Species"));
		ImGui::SliderFloat(T(TKEY("altostratus_weight"), "Altostratus Weight"), &ndfSettings.altostratusWeight, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("altocumulus_weight"), "Altocumulus Weight"), &ndfSettings.altocumulusWeight, 0.f, 1.f, "%.2f");

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
}

void NdfManager::UpdateNdf(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer)
{
	GenerateTextures(ndfSettings, cloudLayer);
}

ID3D11ShaderResourceView* NdfManager::GetNdf(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer, TextureManager& texManager)
{
	return GetHpTextures(ndfSettings, cloudLayer, texManager).lowWeather;
}

HpCloudTextureSet NdfManager::GetHpTextures(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer, TextureManager& texManager)
{
	GenerateTextures(ndfSettings, cloudLayer);
	if (!texLowWeather || !texHighWeather || !texProfile || !texScCell || !texHighCell || !texHighWarp || !texHighWisp)
		return {};

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

bool NdfManager::EnsureResources(const NdfSettings& ndfSettings)
{
	const uint32_t weatherDim = std::clamp(ndfSettings.weatherDim, 128u, 1024u);
	const uint32_t profileWidth = std::clamp(ndfSettings.profileWidth, 64u, 512u);
	const uint32_t profileHeight = std::clamp(ndfSettings.profileHeight, 16u, 256u);

	if (generatedWeatherDim != weatherDim || !texLowWeather) {
		texLowWeather = CreateCloudMapTexture(weatherDim, weatherDim, "PhysicalSky::HpLowWeather");
		texHighWeather = CreateCloudMapTexture(weatherDim, weatherDim, "PhysicalSky::HpHighWeather");
		texScCell = CreateCloudMapTexture(weatherDim, weatherDim, "PhysicalSky::HpScCell");
		texHighCell = CreateCloudMapTexture(weatherDim, weatherDim, "PhysicalSky::HpHighCell");
		texHighWarp = CreateCloudMapTexture(weatherDim, weatherDim, "PhysicalSky::HpHighWarp");
		texHighWisp = CreateCloudMapTexture(weatherDim, weatherDim, "PhysicalSky::HpHighWisp");
		texFieldLow = CreateFieldTexture(weatherDim, weatherDim, "PhysicalSky::CloudMapFieldLow");
		texFieldHigh = CreateFieldTexture(weatherDim, weatherDim, "PhysicalSky::CloudMapFieldHigh");
		generatedWeatherDim = weatherDim;
	}

	if (generatedProfileWidth != profileWidth || generatedProfileHeight != profileHeight || !texProfile) {
		texProfile = CreateCloudMapTexture(profileWidth, profileHeight, "PhysicalSky::HpProfile");
		generatedProfileWidth = profileWidth;
		generatedProfileHeight = profileHeight;
	}

	return texLowWeather && texHighWeather && texProfile && texScCell && texHighCell && texHighWarp && texHighWisp && texFieldLow && texFieldHigh;
}

void NdfManager::GenerateTextures(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer)
{
	if (!ShadersReady() || !cbGen || !bufHistogram || !bufThresholds)
		return;

	const float layerDepth = std::max(cloudLayer.highestAltitude - cloudLayer.lowestAltitude, 0.05f);

	size_t hash = 0;
	HashValue(hash, ndfSettings.weatherDim);
	HashValue(hash, ndfSettings.profileWidth);
	HashValue(hash, ndfSettings.profileHeight);
	HashValue(hash, ndfSettings.worldSize);
	HashValue(hash, ndfSettings.seed);
	HashValue(hash, ndfSettings.skyCoverage);
	HashValue(hash, ndfSettings.cloudSize);
	HashValue(hash, ndfSettings.instability);
	HashValue(hash, ndfSettings.character);
	HashValue(hash, ndfSettings.breakup);
	HashValue(hash, ndfSettings.highCoverage);
	HashValue(hash, ndfSettings.coverageEdgeWidth);
	HashValue(hash, ndfSettings.highCoverageEdgeWidth);
	HashValue(hash, ndfSettings.frontStrength);
	HashValue(hash, ndfSettings.frontBearing);
	HashValue(hash, ndfSettings.domeStrength);
	HashValue(hash, ndfSettings.stratocumulus);
	HashValue(hash, ndfSettings.cumulusWeight);
	HashValue(hash, ndfSettings.toweringCumulusWeight);
	HashValue(hash, ndfSettings.cumulonimbusWeight);
	HashValue(hash, ndfSettings.cumulusDepth);
	HashValue(hash, ndfSettings.toweringCumulusDepth);
	HashValue(hash, ndfSettings.cumulonimbusDepth);
	HashValue(hash, ndfSettings.altostratusWeight);
	HashValue(hash, ndfSettings.altocumulusWeight);
	HashValue(hash, layerDepth);
	// Zero is the "never generated" sentinel, so a hash that lands on it must not
	// be treated as up to date.
	if (hash == 0)
		hash = 1;
	if (hash == generatedHash)
		return;

	if (!EnsureResources(ndfSettings))
		return;

	auto context = globals::d3d::context;
	auto state = globals::state;

	const uint32_t weatherDim = generatedWeatherDim;
	const uint32_t profileWidth = generatedProfileWidth;
	const uint32_t profileHeight = generatedProfileHeight;

	// Convective cell period, in cells across one map tile. The generator works in
	// tile space, so the physical cloud size has to be expressed as a count.
	const float worldSize = std::max(ndfSettings.worldSize, 1.0f);
	const uint32_t cellPeriod = static_cast<uint32_t>(std::clamp(std::round(worldSize / std::max(ndfSettings.cloudSize, 0.05f)), 2.0f, 128.0f));

	// Fronts are stretched along an integer lattice vector so the anisotropic
	// noise still tiles. Snapping the bearing to a small rational direction keeps
	// the band orientation close to what the slider asked for.
	const float bearingRadians = ndfSettings.frontBearing * (std::numbers::pi_v<float> / 180.0f);
	const float rawX = std::cos(bearingRadians);
	const float rawY = std::sin(bearingRadians);
	const float dominant = std::max(std::abs(rawX), std::abs(rawY));
	const float normalX = std::round(rawX / std::max(dominant, 1e-3f) * 2.0f);
	const float normalY = std::round(rawY / std::max(dominant, 1e-3f) * 2.0f);

	// Instability biases the species mix toward deeper convection without
	// overriding the authored weights entirely.
	const float instability = std::clamp(ndfSettings.instability, 0.0f, 1.0f);
	const float cuWeight = std::max(ndfSettings.cumulusWeight, 0.0f) * (1.0f - instability * 0.7f);
	const float tcuWeight = std::max(ndfSettings.toweringCumulusWeight, 0.0f) * (1.0f + instability * 0.5f);
	const float cbWeight = std::max(ndfSettings.cumulonimbusWeight, 0.0f) * (1.0f + instability * 2.0f);
	const float weightSum = cuWeight + tcuWeight + cbWeight;
	const float cuShare = weightSum > 1e-5f ? cuWeight / weightSum : 1.0f;
	const float tcuShare = weightSum > 1e-5f ? tcuWeight / weightSum : 0.0f;

	const float asWeight = std::max(ndfSettings.altostratusWeight, 0.0f);
	const float acWeight = std::max(ndfSettings.altocumulusWeight, 0.0f);
	const float highWeightSum = asWeight + acWeight;
	const float asShare = highWeightSum > 1e-5f ? asWeight / highWeightSum : 1.0f;

	// Instability also deepens every species: the same weather pattern under a
	// more unstable sounding builds taller clouds, not just different ones.
	const float depthScale = 1.0f + instability * 0.6f;

	GenCB cb{
		.weatherDim = { weatherDim, weatherDim },
		.profileDim = { profileWidth, profileHeight },
		.cellPeriod = cellPeriod,
		.seed = ndfSettings.seed,
		.solveRound = 0u,
		.skyCoverage = std::clamp(ndfSettings.skyCoverage, 0.0f, 1.0f),
		.highCoverage = std::clamp(ndfSettings.highCoverage, 0.0f, 1.0f),
		.instability = instability,
		.character = std::clamp(ndfSettings.character, 0.0f, 1.0f),
		.breakup = std::clamp(ndfSettings.breakup, 0.0f, 1.0f),
		.coverageEdgeWidth = std::max(ndfSettings.coverageEdgeWidth, 0.05f),
		.highCoverageEdgeWidth = std::max(ndfSettings.highCoverageEdgeWidth, 0.05f),
		.frontStrength = std::clamp(ndfSettings.frontStrength, 0.0f, 1.0f),
		.domeStrength = std::clamp(ndfSettings.domeStrength, 0.0f, 1.0f),
		.frontNormal = { normalX, normalY },
		.frontTangent = { -normalY, normalX },
		.scShare = std::clamp(ndfSettings.stratocumulus, 0.0f, 1.0f),
		.cuShare = cuShare,
		.tcuShare = tcuShare,
		.asShare = asShare,
		.cumulusDepth = std::max(ndfSettings.cumulusDepth, 0.05f) * depthScale,
		.toweringCumulusDepth = std::max(ndfSettings.toweringCumulusDepth, 0.05f) * depthScale,
		.cumulonimbusDepth = std::max(ndfSettings.cumulonimbusDepth, 0.05f) * depthScale,
		.layerDepth = layerDepth,
	};

	state->BeginPerfEvent("Physical Sky: Cloud Map Generation");
	globals::profiler->BeginPass("PhysicalSky::CloudMapGen");

	const uint32_t weatherGroups = (weatherDim + 7u) >> 3;
	ID3D11UnorderedAccessView* nullUavs[6] = {};
	ID3D11ShaderResourceView* nullSrvs[3] = {};

	const auto uploadCB = [&](uint32_t round) {
		cb.solveRound = round;
		cbGen->Update(cb);
		ID3D11Buffer* buffer = cbGen->CB();
		context->CSSetConstantBuffers(1, 1, &buffer);
	};

	uploadCB(0u);

	// Pass 1: morphology fields.
	{
		std::array<ID3D11UnorderedAccessView*, 2> uavs = { texFieldLow->uav.get(), texFieldHigh->uav.get() };
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csFields.get(), nullptr, 0);
		context->Dispatch(weatherGroups, weatherGroups, 1);
		context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	}

	// Passes 2 and 3, three times. Each round's histogram is gated on thresholds
	// the previous round solved: coverage first, then the stratocumulus split over
	// the covered area, then the species split over what Sc did not claim.
	for (uint32_t round = 0u; round < 3u; ++round) {
		uploadCB(round);

		const UINT clearValues[4] = { 0u, 0u, 0u, 0u };
		context->ClearUnorderedAccessViewUint(bufHistogram->uav.get(), clearValues);

		std::array<ID3D11ShaderResourceView*, 3> srvs = { texFieldLow->srv.get(), texFieldHigh->srv.get(), bufThresholds->srv.get() };
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		ID3D11UnorderedAccessView* histogramUav = bufHistogram->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &histogramUav, nullptr);
		context->CSSetShader(csHistogram.get(), nullptr, 0);
		context->Dispatch(weatherGroups, weatherGroups, 1);

		context->CSSetShaderResources(0, 3, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

		std::array<ID3D11UnorderedAccessView*, 2> solveUavs = { bufHistogram->uav.get(), bufThresholds->uav.get() };
		context->CSSetUnorderedAccessViews(0, (uint)solveUavs.size(), solveUavs.data(), nullptr);
		context->CSSetShader(csSolve.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);
		context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	}

	// Pass 4: final weather and auxiliary maps.
	{
		std::array<ID3D11ShaderResourceView*, 3> srvs = { texFieldLow->srv.get(), texFieldHigh->srv.get(), bufThresholds->srv.get() };
		std::array<ID3D11UnorderedAccessView*, 6> uavs = {
			texLowWeather->uav.get(), texHighWeather->uav.get(), texScCell->uav.get(),
			texHighCell->uav.get(), texHighWarp->uav.get(), texHighWisp->uav.get()
		};
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(csCompose.get(), nullptr, 0);
		context->Dispatch(weatherGroups, weatherGroups, 1);
		context->CSSetShaderResources(0, 3, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 6, nullUavs, nullptr);
	}

	// Pass 5: profile LUT.
	{
		ID3D11UnorderedAccessView* profileUav = texProfile->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &profileUav, nullptr);
		context->CSSetShader(csProfile.get(), nullptr, 0);
		context->Dispatch((profileWidth + 7u) >> 3, (profileHeight + 7u) >> 3, 1);
		context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
	}

	context->CSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(1, 1, &nullCb);

	// The renderer gates whole rays on High Weather mip 2, so the chain is part of
	// the contract rather than an optimization.
	context->GenerateMips(texLowWeather->srv.get());
	context->GenerateMips(texHighWeather->srv.get());
	context->GenerateMips(texProfile->srv.get());
	context->GenerateMips(texScCell->srv.get());
	context->GenerateMips(texHighCell->srv.get());
	context->GenerateMips(texHighWarp->srv.get());
	context->GenerateMips(texHighWisp->srv.get());

	globals::profiler->EndPass();
	state->EndPerfEvent();

	generatedHash = hash;
}

#undef I18N_KEY_PREFIX
