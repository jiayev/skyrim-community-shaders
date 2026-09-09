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

void NdfManager::SetupResources()
{
	logger::debug("Creating NDF resources...");
	{
		generatorCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<GenCB>());

		D3D11_TEXTURE2D_DESC tex_desc{
			.Width = kNdfDim,
			.Height = kNdfDim,
			.MipLevels = 1,
			.ArraySize = 2,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray = { .MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = 0, .ArraySize = 2 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray = { .MipSlice = 0, .FirstArraySlice = 0, .ArraySize = 2 }
		};

		texNdfOutput = eastl::make_unique<Texture2D>(tex_desc, "PhysicalSky::PackedCloudNdf");
		texNdfOutput->CreateSRV(srv_desc);
		texNdfOutput->CreateUAV(uav_desc);
	}

	accelerationCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc(16));
	const D3D11_TEXTURE2D_DESC desc{
		.Width = 64,
		.Height = 64,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R32_FLOAT,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	};
	const D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{
		.Format = desc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	const D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{
		.Format = desc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};
	texOccupancy = eastl::make_unique<Texture2D>(desc, "PhysicalSky::CloudOccupancy");
	texOccupancy->CreateSRV(srvDesc);
	texOccupancy->CreateUAV(uavDesc);
	texDistance = eastl::make_unique<Texture2D>(desc, "PhysicalSky::CloudDistance");
	texDistance->CreateSRV(srvDesc);
	texDistance->CreateUAV(uavDesc);
	accelerationValid = false;
	generatedValid = false;

	CompileShaders();
}

void NdfManager::CompileShaders()
{
	logger::debug("Compiling NDF shaders...");
	generatedValid = false;
	accelerationValid = false;
	generatorProgram = nullptr;

	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfGenerate.cs.hlsl", {}, "cs_5_0")))
		generatorProgram.attach(rawPtr);
	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfAcceleration.cs.hlsl", {}, "cs_5_0", "buildOccupancy")))
		occupancyProgram.attach(rawPtr);
	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfAcceleration.cs.hlsl", {}, "cs_5_0", "buildDistance")))
		distanceProgram.attach(rawPtr);
}

const char* NdfManager::GetSettingsTypeName(const NdfSettings& ndfSettings)
{
	return ndfSettings.type == NdfType::Texture ? "Texture" : "Procedural";
}

const char* NdfManager::GetSettingsHint(const NdfSettings& ndfSettings)
{
	if (ndfSettings.type == NdfType::Texture)
		return "Read the cloud map from dds textures. More static but you can draw arbitrary shapes.\n"
			   "Use a linear DDS Texture2DArray with exactly 5 scalar slices (typically 256x256):\n"
			   "1. min height\n"
			   "2. max height\n"
			   "3. coverage\n"
			   "4. cloud type\n"
			   "5. bottom type";
	return "Seeded cloud masses with independent weather organization, domes and vertical development.\n"
		   "Cloud spacing and weather scale are in kilometres. Wind advects the generated field without rebuilding it.";
}

void NdfManager::DrawNdfSettings(NdfSettings& ndfSettings, TextureManager& texManager)
{
	// ndf type selector
	constexpr std::array types = {
		std::pair{ "Texture", NdfType::Texture },
		std::pair{ "Procedural", NdfType::Procedural },
	};

	if (ImGui::BeginCombo(T(TKEY("cloud_map_generator"), "Cloud Map Generator"), GetSettingsTypeName(ndfSettings))) {
		for (auto& [name, type] : types)
			if (ImGui::Selectable(name, ndfSettings.type == type)) {
				ndfSettings.type = type;
				break;
			}
		ImGui::EndCombo();
	}

	ImGui::Separator();

	if (ImGui::BeginTable("NdfHint", 1, ImGuiTableFlags_BordersOuter, { -FLT_MIN, 0 })) {
		ImGui::TableNextColumn();
		ImGui::TextWrapped("%s", GetSettingsHint(ndfSettings));
		ImGui::EndTable();
	}

	if (ndfSettings.type == NdfType::Texture) {
		auto& s = ndfSettings.texture;
		if (ImGui::BeginCombo(T(TKEY("texture_path"), "Texture Path"), s.texPath.c_str())) {
			for (auto& path_choice : texManager.ListPaths())
				if (ImGui::Selectable(path_choice.c_str(), path_choice == s.texPath))
					s.texPath = path_choice;
			ImGui::EndCombo();
		}

		if (!IsTextureNdf(texManager.Query(s.texPath)))
			ImGui::TextColored({ 1, 0, 0, 1 }, "%s", T(TKEY("ndf_invalid_texture"), "Select a linear NDF texture array with five slices."));
	} else {
		auto& s = ndfSettings.procedural;
		constexpr const char* forms[] = { "Cumulus", "Stratocumulus", "Stratus" };
		int form = static_cast<int>(std::min(s.form, 2u));
		if (ImGui::Combo(T(TKEY("ndf_form"), "Cloud Form"), &form, forms, 3))
			s.form = static_cast<uint32_t>(form);
		ImGui::InputScalar(T(TKEY("ndf_seed"), "Seed"), ImGuiDataType_U32, &s.seed);
		ImGui::SliderFloat(T(TKEY("ndf_amount"), "Cloud Amount"), &s.coverage, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ndf_weather_strength"), "Weather Organization"), &s.weatherStrength, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ndf_weather_scale"), "Weather Scale"), &s.weatherScale, 1.f, 50.f, "%.1f km");
		if (s.form != 2u) {
			ImGui::SliderFloat(T(TKEY("ndf_cloud_size"), "Cloud Spacing"), &s.cloudSize, 0.25f, 8.f, "%.2f km");
			ImGui::SliderFloat(T(TKEY("ndf_size_variation"), "Size Variation"), &s.sizeVariation, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat(T(TKEY("ndf_clustering"), "Clustering"), &s.clustering, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat(T(TKEY("ndf_elongation"), "Elongation"), &s.elongation, 1.f, 3.f, "%.2f");
			ImGui::SliderAngle(T(TKEY("ndf_bearing"), "Cloud Bearing"), &s.bearing, 0.f, 360.f);
			ImGui::SliderFloat(T(TKEY("ndf_shoulders"), "Secondary Domes"), &s.shoulders, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat(T(TKEY("ndf_edge_softness"), "Outline Softness"), &s.edgeSoftness, 0.03f, 0.5f, "%.2f");
		}
		ImGui::SliderFloat(T(TKEY("ndf_development"), "Vertical Development"), &s.development, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ndf_height_variation"), "Height Variation"), &s.heightVariation, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ndf_base_variation"), "Base Variation"), &s.baseVariation, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ndf_top_type"), "Top Type"), &s.topType, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("ndf_top_type_variation"), "Top Type Variation"), &s.topTypeVariation, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat(T(TKEY("bottom_type"), "Bottom Type"), &s.bottomType, 0.f, 1.f, "%.2f");
	}
}

#undef I18N_KEY_PREFIX

bool NdfManager::UpdateNdf(const NdfSettings& ndfSettings, const LowCloudSettings& low)
{
	if (ndfSettings.type == NdfType::Texture || !generatorProgram || !generatorCb || !texNdfOutput)
		return false;

	GenCB data{};
	data.shape = ndfSettings.procedural;
	auto& shape = data.shape;
	shape.form = std::min(shape.form, 2u);
	for (auto* value : { &shape.coverage, &shape.weatherStrength, &shape.sizeVariation, &shape.clustering,
			 &shape.development, &shape.heightVariation, &shape.baseVariation, &shape.topType,
			 &shape.topTypeVariation, &shape.bottomType, &shape.shoulders })
		*value = std::isfinite(*value) ? std::clamp(*value, 0.f, 1.f) : 0.f;
	const auto finiteClamp = [](float value, float fallback, float lower, float upper) {
		return std::isfinite(value) ? std::clamp(value, lower, upper) : fallback;
	};
	shape.cloudSize = finiteClamp(shape.cloudSize, 1.6f, 0.25f, 8.f);
	shape.weatherScale = finiteClamp(shape.weatherScale, 8.f, 1.f, 50.f);
	shape.elongation = finiteClamp(shape.elongation, 1.4f, 1.f, 3.f);
	shape.edgeSoftness = finiteClamp(shape.edgeSoftness, 0.18f, 0.03f, 0.5f);
	shape.bearing = std::isfinite(shape.bearing) ? std::fmod(shape.bearing, 2.f * std::numbers::pi_v<float>) : 0.f;
	shape._pad = {};
	data.worldSize = { finiteClamp(low.ndfScale.x, 16.f, 1.f, 50.f), finiteClamp(low.ndfScale.y, 16.f, 1.f, 50.f) };
	if (generatedValid && std::memcmp(&generatedData, &data, sizeof(data)) == 0)
		return false;
	generatorCb->Update(data);

	auto* context = globals::d3d::context;
	auto* uav = texNdfOutput->uav.get();
	auto* cb = generatorCb->CB();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(generatorProgram.get(), nullptr, 0);
	globals::profiler->BeginPass("PhysicalSky::CloudNdf");
	context->Dispatch((kNdfDim + 7) >> 3, (kNdfDim + 7) >> 3, 1);
	globals::profiler->EndPass();

	uav = nullptr;
	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	generatedData = data;
	generatedValid = true;
	accelerationValid = false;
	return true;
}

void NdfManager::UpdateAcceleration(const NdfSettings& ndfSettings, TextureManager& texManager)
{
	auto* ndf = GetNdf(ndfSettings, texManager);
	if (ndfSettings.type == NdfType::Procedural && accelerationValid && acceleratedNdf == ndf)
		return;
	accelerationValid = false;
	if (!ndf || !occupancyProgram || !distanceProgram || !texOccupancy || !texDistance || !accelerationCb)
		return;
	const std::array<uint32_t, 4> data = { ndfSettings.type == NdfType::Texture ? 0u : 1u, 0u, 0u, 0u };
	accelerationCb->Update(data);
	auto* context = globals::d3d::context;
	auto* cb = accelerationCb->CB();
	auto* uav = texOccupancy->uav.get();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShaderResources(0, 1, &ndf);
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
	cb = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);
	accelerationValid = true;
	acceleratedNdf = ndf;
}

bool NdfManager::IsTextureNdf(ID3D11ShaderResourceView* srv)
{
	if (!srv)
		return false;
	D3D11_SHADER_RESOURCE_VIEW_DESC desc;
	srv->GetDesc(&desc);
	if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY || desc.Texture2DArray.ArraySize != 5)
		return false;
	switch (desc.Format) {
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_BC4_UNORM:
		return true;
	default:
		return false;
	}
}

ID3D11ShaderResourceView* NdfManager::GetNdf(const NdfSettings& ndfSettings, TextureManager& texManager)
{
	if (ndfSettings.type != NdfType::Texture)
		return generatedValid && texNdfOutput ? texNdfOutput->srv.get() : nullptr;
	auto* srv = texManager.Query(ndfSettings.texture.texPath);
	return IsTextureNdf(srv) ? srv : nullptr;
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
