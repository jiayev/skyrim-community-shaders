#include "Features/PhysicalSky.h"

#include "I18n/I18n.h"
#include "State.h"
#include "Util.h"

#include <DDSTextureLoader.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <imgui_stdlib.h>
#include <numbers>

#define I18N_KEY_PREFIX "feature.physical_sky."

template <class... Ts>
struct overloads : Ts...
{
	using Ts::operator()...;
};

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
		cumuliformCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<CumuliformNdfSettings>());

		D3D11_TEXTURE2D_DESC tex_desc{
			.Width = kNdfDim,
			.Height = kNdfDim,
			.MipLevels = 1,
			.ArraySize = 5,
			.Format = DXGI_FORMAT_R8_UNORM,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray = { .MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = 0, .ArraySize = 5 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray = { .MipSlice = 0, .FirstArraySlice = 0, .ArraySize = 5 }
		};

		texNdfOutput = eastl::make_unique<Texture2D>(tex_desc);
		texNdfOutput->CreateSRV(srv_desc);
		texNdfOutput->CreateUAV(uav_desc);
	}

	CompileShaders();
}

void NdfManager::CompileShaders()
{
	logger::debug("Compiling NDF shaders...");

	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfCumuliform.cs.hlsl", {}, "cs_5_0")))
		cumuliformProgram.attach(rawPtr);
}

const char* NdfManager::GetSettingsTypeName(const NdfSettings& ndfSettings)
{
	auto visitor = overloads{
		[&](const TexNdfSettings&) { return "Texture"; },
		[&](const CumuliformNdfSettings&) { return "Cumuliform"; }
	};

	return std::visit(visitor, ndfSettings);
}

const char* NdfManager::GetSettingsHint(const NdfSettings& ndfSettings)
{
	auto visitor = overloads{
		[&](const TexNdfSettings&) {
			return "Read the cloud map from dds textures. More static but you can draw arbitrary shapes.\n"
				   "The texture should be a 256x256 Texture2DArray consists of 5 grayscale images:\n"
				   "1. min height\n"
				   "2. max height\n"
				   "3. coverage\n"
				   "4. cloud type\n"
				   "5. bottom type";
		},
		[&](const CumuliformNdfSettings&) {
			return "A simple-yet-versatile cloud map generator that gets you from billowy cumulus to thick stratus sheets.";
		}
	};

	return std::visit(visitor, ndfSettings);
}

void NdfManager::DrawNdfSettings(NdfSettings& ndfSettings, TextureManager& texManager)
{
	// ndf type selector
	const static auto types = []() {
		std::vector<std::pair<std::string, NdfSettings>> retval = {
			{ "", TexNdfSettings() },
			{ "", CumuliformNdfSettings() },
		};
		for (auto& [name, s] : retval)
			name = GetSettingsTypeName(s);
		return retval;
	}();

	if (ImGui::BeginCombo(T(TKEY("cloud_map_generator"), "Cloud Map Generator"), GetSettingsTypeName(ndfSettings))) {
		for (auto& [name, s] : types)
			if (ImGui::Selectable(name.c_str(), false)) {
				ndfSettings = s;
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

	auto visitor = overloads{
		[&](TexNdfSettings& s) {
			if (ImGui::BeginCombo(T(TKEY("texture_path"), "Texture Path"), s.texPath.c_str())) {
				for (auto& path_choice : texManager.ListPaths())
					if (ImGui::Selectable(path_choice.c_str(), path_choice == s.texPath))
						s.texPath = path_choice;
				ImGui::EndCombo();
			}

			if (!texManager.Query(s.texPath))
				ImGui::TextColored({ 1, 0, 0, 1 }, "%s", T(TKEY("failed_to_load_texture"), "Failed to load texture."));
		},
		[&](CumuliformNdfSettings& s) {
			constexpr uint32_t pmin = 2;
			constexpr uint32_t pmax = 50;
			ImGui::SliderScalarN(T(TKEY("layer_1_frequency"), "Layer 1 - Frequency"), ImGuiDataType_U32, (void*)&s.scale0.x, 2, &pmin, &pmax, "%u");
			ImGui::SliderFloat2(T(TKEY("layer_1_velocity"), "Layer 1 - Velocity"), &s.offset0.x, -100.f, 100.f, "%.1f");
			ImGui::SliderAngle(T(TKEY("layer_1_rotation"), "Layer 1 - Rotation"), &s.rot0, 0.f, 360.f);

			ImGui::SliderScalarN(T(TKEY("layer_2_frequency"), "Layer 2 - Frequency"), ImGuiDataType_U32, (void*)&s.scale1.x, 2, &pmin, &pmax, "%u");
			ImGui::SliderFloat2(T(TKEY("layer_2_velocity"), "Layer 2 - Velocity"), &s.offset1.x, -100.f, 100.f, "%.1f");
			ImGui::SliderAngle(T(TKEY("layer_2_rotation"), "Layer 2 - Rotation"), &s.rot1, 0.f, 360.f);

			ImGui::SliderScalarN(T(TKEY("layer_3_frequency"), "Layer 3 - Frequency"), ImGuiDataType_U32, (void*)&s.scale2.x, 2, &pmin, &pmax, "%u");
			ImGui::SliderFloat2(T(TKEY("layer_3_velocity"), "Layer 3 - Velocity"), &s.offset2.x, -100.f, 100.f, "%.1f");
			ImGui::SliderAngle(T(TKEY("layer_3_rotation"), "Layer 3 - Rotation"), &s.rot2, 0.f, 360.f);

			ImGui::SliderFloat2(T(TKEY("coverage_clamping"), "Coverage Clamping"), &s.clipRange.x, 0, 1, "%.2f");
			ImGui::SliderFloat(T(TKEY("power"), "Power"), &s.power, 0.2f, 5, "%.2f");
			ImGui::SliderFloat(T(TKEY("bottom_type"), "Bottom Type"), &s.wispiness, 0.f, 1.f, "%.2f");
		},
		[&](auto&) {}
	};
	std::visit(visitor, ndfSettings);
}

#undef I18N_KEY_PREFIX

void NdfManager::UpdateNdf(const NdfSettings& ndfSettings)
{
	auto visitor = overloads{
		[&](const TexNdfSettings&) {},
		[&](const CumuliformNdfSettings& s) {
			CumuliformNdfSettings data = s;
			data.offset0 *= -globals::state->timer * 1e-3f;
			data.offset1 *= -globals::state->timer * 1e-3f;
			data.offset2 *= -globals::state->timer * 1e-3f;
			cumuliformCb->Update(data);

			auto context = globals::d3d::context;

			auto uav = texNdfOutput->uav.get();
			auto cb = cumuliformCb->CB();
			context->CSSetConstantBuffers(1, 1, &cb);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(cumuliformProgram.get(), nullptr, 0);
			globals::profiler->BeginPass("PhysicalSky::CloudNdf");
			context->Dispatch((kNdfDim + 7) >> 3, (kNdfDim + 7) >> 3, 1);
			globals::profiler->EndPass();

			uav = nullptr;
			cb = nullptr;
			context->CSSetConstantBuffers(1, 1, &cb);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
		}
	};
	std::visit(visitor, ndfSettings);
}

ID3D11ShaderResourceView* NdfManager::GetNdf(const NdfSettings& ndfSettings, TextureManager& texManager)
{
	auto visitor = overloads{
		[&](const TexNdfSettings& s) { return texManager.Query(s.texPath); },
		[&](const auto&) { return texNdfOutput->srv.get(); },
	};
	return std::visit(visitor, ndfSettings);
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

bool HighCloudMapManager::EnsureResources(const HpHighCloudSettings& settings)
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

void HighCloudMapManager::GenerateTextures(const HpHighCloudSettings& settings)
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

HighCloudTextureSet HighCloudMapManager::GetTextures(const HpHighCloudSettings& settings)
{
	GenerateTextures(settings);
	return {
		.highWeather = texHighWeather ? texHighWeather->srv.get() : nullptr,
		.highCell = texHighCell ? texHighCell->srv.get() : nullptr,
		.highWarp = texHighWarp ? texHighWarp->srv.get() : nullptr,
		.highWisp = texHighWisp ? texHighWisp->srv.get() : nullptr,
	};
}
