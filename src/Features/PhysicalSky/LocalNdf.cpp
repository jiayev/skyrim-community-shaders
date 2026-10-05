#include "Features/PhysicalSky.h"

#include "I18n/I18n.h"
#include "Util.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <imgui_stdlib.h>

#define I18N_KEY_PREFIX "feature.physical_sky."

namespace
{
	constexpr const char* kAssetRoot = "Data/Textures/PhysicalSky/LocalNdf";

	float ClampFinite(float value, float minimum, float maximum, float fallback)
	{
		return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
	}

	eastl::unique_ptr<Texture2D> CreateLocalTexture(uint32_t dimension, const char* name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = dimension;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		auto texture = eastl::make_unique<Texture2D>(desc, name);
		texture->CreateSRV({ .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MipLevels = 1 } });
		texture->CreateUAV({ .Format = desc.Format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D });
		return texture;
	}
}

std::filesystem::path LocalNdfManager::AssetPath(const std::string& asset)
{
	if (asset.empty() || !std::ranges::all_of(asset, [](unsigned char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		}))
		return {};
	return std::filesystem::path(kAssetRoot) / asset;
}

void LocalNdfManager::RefreshAssets()
{
	assets.clear();
	std::error_code error;
	for (std::filesystem::directory_iterator it(kAssetRoot, error), end; !error && it != end; it.increment(error)) {
		if (it->is_directory(error)) {
			const auto id = it->path().filename().string();
			if (!AssetPath(id).empty())
				assets.push_back(id);
		}
	}
	std::ranges::sort(assets);
	scanned = true;
	key.clear();
}

void LocalNdfManager::SetupResources()
{
	cb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<Parameters>());
	height = CreateLocalTexture(1, "PhysicalSky::LocalNdfHeight");
	modeling = CreateLocalTexture(1, "PhysicalSky::LocalNdfModeling");
	const float clear[4] = {};
	globals::d3d::context->ClearUnorderedAccessViewFloat(height->uav.get(), clear);
	globals::d3d::context->ClearUnorderedAccessViewFloat(modeling->uav.get(), clear);
	D3D11_SAMPLER_DESC desc{};
	desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	desc.MaxLOD = D3D11_FLOAT32_MAX;
	desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	desc.MaxAnisotropy = 1;
	sampler = nullptr;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&desc, sampler.put()));
	CompileShaders();
}

void LocalNdfManager::CompileShaders()
{
	program = nullptr;
	key.clear();
	altitude = {};
	if (auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\LocalNdf.cs.hlsl", {}, "cs_5_0")))
		program.attach(raw);
}

NdfTextureSet LocalNdfManager::GetTextures() const
{
	return { height ? height->srv.get() : nullptr, modeling ? modeling->srv.get() : nullptr };
}

bool LocalNdfManager::Update(const NdfSettings& settings, TextureManager& textures, const std::string& worldspace)
{
	if (!program || !cb || !sampler)
		return false;
	struct Layer
	{
		Parameters data;
		std::array<ID3D11ShaderResourceView*, 3> sources;
		int priority;
		std::string asset;
	};
	std::vector<Layer> layers;
	rejectedAssets.clear();
	float2 minimum = { FLT_MAX, FLT_MAX }, maximum = { -FLT_MAX, -FLT_MAX };
	float minAltitude = FLT_MAX, maxAltitude = -FLT_MAX;
	for (const auto& instance : settings.instances) {
		if (!instance.enabled || instance.worldspace.empty() || instance.worldspace != worldspace)
			continue;
		const auto path = AssetPath(instance.asset);
		if (path.empty()) {
			rejectedAssets.push_back(instance.asset);
			continue;
		}
		const auto heightPath = (path / "Height.dds").string();
		const auto modelingPath = (path / "Modeling.dds").string();
		const auto maskPath = (path / "Mask.dds").string();
		textures.EnsureLoaded(heightPath);
		textures.EnsureLoaded(modelingPath);
		std::error_code error;
		const bool hasMask = std::filesystem::exists(maskPath, error);
		if (error) {
			rejectedAssets.push_back(instance.asset);
			continue;
		}
		if (hasMask)
			textures.EnsureLoaded(maskPath);
		NdfTextureSet maps{ textures.Query(heightPath), textures.Query(modelingPath) };
		auto* mask = hasMask ? textures.Query(maskPath) : nullptr;
		if (!NdfManager::IsTexturePair(maps) || (hasMask && !NdfManager::IsTextureNdf(mask, 1))) {
			rejectedAssets.push_back(instance.asset);
			continue;
		}
		Parameters data{};
		data.centerSize = { ClampFinite(instance.center.x, -1e7f, 1e7f, 0.f), ClampFinite(instance.center.y, -1e7f, 1e7f, 0.f),
			ClampFinite(instance.size.x, 1.f, 128000.f, 8000.f), ClampFinite(instance.size.y, 1.f, 128000.f, 8000.f) };
		const float angle = ClampFinite(instance.rotation, -36000.f, 36000.f, 0.f) * 0.01745329252f;
		const float strength = ClampFinite(instance.strength, 0.f, 1.f, 1.f);
		data.rotationWeights = { std::cos(angle), std::sin(angle), strength * ClampFinite(instance.modelingWeight, 0.f, 1.f, 1.f),
			strength * ClampFinite(instance.heightWeight, 0.f, 1.f, 1.f) };
		if (data.rotationWeights.z == 0.f && data.rotationWeights.w == 0.f)
			continue;
		data.heightFeather = { ClampFinite(instance.altitude, -24000.f, 48000.f, 256.f), ClampFinite(instance.heightSpan, 1.f, 24000.f, 1792.f),
			ClampFinite(instance.feather, 0.f, std::min(data.centerSize.z, data.centerSize.w) * 0.5f, 200.f), 0.f };
		data.auxiliary = settings.procedural.parameters.heightAuxiliaryRange;
		data.auxiliary.x = ClampFinite(data.auxiliary.x, -16.f, 16.f, 0.f);
		data.auxiliary.y = ClampFinite(data.auxiliary.y, -16.f, 16.f, 1.f);
		if (std::abs(data.auxiliary.y - data.auxiliary.x) < 1e-5f)
			data.auxiliary.y = data.auxiliary.x + 1e-5f;
		data.auxiliary.z = ClampFinite(data.auxiliary.z, -16.f, 16.f, 0.f);
		data.auxiliary.w = ClampFinite(data.auxiliary.w, -16.f, 16.f, 0.f);
		data.hasMask = hasMask ? 1u : 0u;
		data.modelingAlpha = instance.modelingAlpha ? 1u : 0u;
		const float2 radius = { (std::abs(data.rotationWeights.x) * data.centerSize.z + std::abs(data.rotationWeights.y) * data.centerSize.w) * 0.5f,
			(std::abs(data.rotationWeights.y) * data.centerSize.z + std::abs(data.rotationWeights.x) * data.centerSize.w) * 0.5f };
		minimum.x = std::min(minimum.x, data.centerSize.x - radius.x);
		minimum.y = std::min(minimum.y, data.centerSize.y - radius.y);
		maximum.x = std::max(maximum.x, data.centerSize.x + radius.x);
		maximum.y = std::max(maximum.y, data.centerSize.y + radius.y);
		minAltitude = std::min(minAltitude, data.heightFeather.x);
		maxAltitude = std::max(maxAltitude, data.heightFeather.x + data.heightFeather.y);
		layers.push_back({ data, { maps.height, maps.modeling, mask }, instance.priority, instance.asset });
	}
	std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) { return a.priority < b.priority; });
	std::string nextKey = worldspace;
	nextKey.push_back('\0');
	for (const auto& layer : layers) {
		nextKey.append(reinterpret_cast<const char*>(&layer.data), sizeof(layer.data));
		nextKey.append(layer.asset).push_back('\0');
	}
	const float requestedTexelSize = ClampFinite(settings.localTexelSize, 1.f, 512.f, 32.f);
	nextKey.append(reinterpret_cast<const char*>(&requestedTexelSize), sizeof(requestedTexelSize));
	if (key == nextKey && textureRevision == textures.revision)
		return false;
	activeAssets.clear();
	rect = altitude = {};
	uint32_t dimension = 1;
	effectiveTexelSize = 0.f;
	if (!layers.empty()) {
		const float extent = std::max(maximum.x - minimum.x, maximum.y - minimum.y);
		effectiveTexelSize = std::max(requestedTexelSize, extent / 2044.f);
		minimum.x = std::floor(minimum.x / effectiveTexelSize) * effectiveTexelSize - effectiveTexelSize;
		minimum.y = std::floor(minimum.y / effectiveTexelSize) * effectiveTexelSize - effectiveTexelSize;
		dimension = std::clamp(static_cast<uint32_t>(std::ceil(std::max(maximum.x - minimum.x, maximum.y - minimum.y) / effectiveTexelSize)) + 1u, 1u, 2048u);
		const float side = dimension * effectiveTexelSize;
		rect = { minimum.x, minimum.y, 1.f / side, 1.f / side };
		altitude = { minAltitude, maxAltitude - minAltitude, 1.f, 0.f };
	}
	if (!height || !scratchHeight || height->desc.Width != dimension) {
		height = CreateLocalTexture(dimension, "PhysicalSky::LocalNdfHeight");
		modeling = CreateLocalTexture(dimension, "PhysicalSky::LocalNdfModeling");
		scratchHeight = CreateLocalTexture(dimension, "PhysicalSky::LocalNdfScratchHeight");
		scratchModeling = CreateLocalTexture(dimension, "PhysicalSky::LocalNdfScratchModeling");
	}
	auto* context = globals::d3d::context;
	winrt::com_ptr<ID3D11ComputeShader> previousShader;
	winrt::com_ptr<ID3D11Buffer> previousCb;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 5> previousSources;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> previousLocalSources;
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 2> previousTargets;
	context->CSGetShader(previousShader.put(), nullptr, nullptr);
	context->CSGetConstantBuffers(1, 1, previousCb.put());
	for (UINT i = 0; i < previousSources.size(); ++i)
		context->CSGetShaderResources(i, 1, previousSources[i].put());
	for (UINT i = 0; i < previousTargets.size(); ++i) {
		context->CSGetUnorderedAccessViews(i, 1, previousTargets[i].put());
		context->CSGetShaderResources(32 + i, 1, previousLocalSources[i].put());
	}
	ID3D11ShaderResourceView* empty[2] = {};
	context->CSSetShaderResources(32, 2, empty);
	const float clear[4] = {};
	context->ClearUnorderedAccessViewFloat(height->uav.get(), clear);
	context->ClearUnorderedAccessViewFloat(modeling->uav.get(), clear);
	context->ClearUnorderedAccessViewFloat(scratchHeight->uav.get(), clear);
	context->ClearUnorderedAccessViewFloat(scratchModeling->uav.get(), clear);
	winrt::com_ptr<ID3D11SamplerState> savedSampler;
	context->CSGetSamplers(0, 1, savedSampler.put());
	auto* samp = sampler.get();
	auto* buffer = cb->CB();
	context->CSSetSamplers(0, 1, &samp);
	context->CSSetConstantBuffers(1, 1, &buffer);
	context->CSSetShader(program.get(), nullptr, 0);
	globals::profiler->BeginPass("PhysicalSky::LocalNdf");
	for (auto& layer : layers) {
		layer.data.rect = rect;
		layer.data.altitude = altitude;
		layer.data.dimension = dimension;
		cb->Update(layer.data);
		ID3D11ShaderResourceView* sources[] = { layer.sources[0], layer.sources[1], layer.sources[2], height->srv.get(), modeling->srv.get() };
		ID3D11UnorderedAccessView* targets[] = { scratchHeight->uav.get(), scratchModeling->uav.get() };
		context->CSSetShaderResources(0, 5, sources);
		context->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
		context->Dispatch((dimension + 7u) / 8u, (dimension + 7u) / 8u, 1);
		ID3D11ShaderResourceView* emptySources[5] = {};
		ID3D11UnorderedAccessView* emptyTargets[2] = {};
		context->CSSetShaderResources(0, 5, emptySources);
		context->CSSetUnorderedAccessViews(0, 2, emptyTargets, nullptr);
		height.swap(scratchHeight);
		modeling.swap(scratchModeling);
		activeAssets.push_back(layer.asset);
	}
	globals::profiler->EndPass();
	ID3D11ShaderResourceView* nullSources[3] = {};
	ID3D11UnorderedAccessView* nullUavs[2] = {};
	context->CSSetShaderResources(0, 3, nullSources);
	context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);
	samp = savedSampler.get();
	context->CSSetSamplers(0, 1, &samp);
	context->CSSetShader(previousShader.get(), nullptr, 0);
	buffer = previousCb.get();
	context->CSSetConstantBuffers(1, 1, &buffer);
	for (UINT i = 0; i < previousSources.size(); ++i) {
		auto* source = previousSources[i].get();
		context->CSSetShaderResources(i, 1, &source);
	}
	for (UINT i = 0; i < previousTargets.size(); ++i) {
		auto* target = previousTargets[i].get();
		auto* source = previousLocalSources[i].get();
		context->CSSetUnorderedAccessViews(i, 1, &target, nullptr);
		context->CSSetShaderResources(32 + i, 1, &source);
	}
	textureRevision = textures.revision;
	key = std::move(nextKey);
	return true;
}

void LocalNdfManager::DrawSettings(NdfSettings& settings, const std::string& worldspace, float2 cameraMeters)
{
	if (!ImGui::TreeNode(T(TKEY("local_ndf"), "Local cloud instances")))
		return;
	if (!scanned)
		RefreshAssets();
	ImGui::TextUnformatted(kAssetRoot);
	if (!program)
		ImGui::TextWrapped("%s", T(TKEY("local_ndf_shader_missing"), "Local NDF shader unavailable; global clouds remain active."));
	if (ImGui::Button(T(TKEY("local_ndf_scan"), "Scan asset folders")))
		RefreshAssets();
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("local_ndf_add"), "Add cloud instance"))) {
		LocalNdfInstance instance;
		instance.name = T(TKEY("local_ndf_default_name"), "Cloud");
		uint32_t suffix = 1;
		do {
			instance.id = "cloud-" + std::to_string(suffix++);
		} while (std::ranges::any_of(settings.instances, [&](const auto& existing) { return existing.id == instance.id; }));
		instance.worldspace = worldspace;
		instance.center = cameraMeters;
		if (!assets.empty())
			instance.asset = assets.front();
		settings.instances.push_back(std::move(instance));
	}
	ImGui::DragFloat(T(TKEY("local_ndf_texel"), "Requested texel size (m)"), &settings.localTexelSize, 1.f, 1.f, 512.f);
	ImGui::TextWrapped("%s", T(TKEY("local_ndf_saved"), "Instances are saved with Physical Sky settings. Positions use world metres; altitude is relative to the world's atmosphere ground."));
	for (size_t i = 0; i < settings.instances.size();) {
		auto& instance = settings.instances[i];
		ImGui::PushID(static_cast<int>(i));
		bool remove = false;
		if (ImGui::TreeNode("instance", "%s", instance.name.c_str())) {
			ImGui::Text("ID: %s", instance.id.c_str());
			ImGui::InputText(T(TKEY("local_ndf_name"), "Instance name"), &instance.name);
			ImGui::Checkbox(T(TKEY("local_ndf_enabled"), "Enable instance"), &instance.enabled);
			ImGui::InputText(T(TKEY("local_ndf_asset"), "Asset folder ID"), &instance.asset);
			if (ImGui::BeginCombo(T(TKEY("local_ndf_assets"), "Available assets"), instance.asset.c_str())) {
				for (const auto& asset : assets)
					if (ImGui::Selectable(asset.c_str(), asset == instance.asset))
						instance.asset = asset;
				ImGui::EndCombo();
			}
			ImGui::InputText(T(TKEY("local_ndf_worldspace"), "Worldspace Editor ID"), &instance.worldspace);
			if (instance.worldspace.empty() || instance.worldspace != worldspace)
				ImGui::TextWrapped("%s", T(TKEY("local_ndf_other_world"), "Inactive here: assign the current worldspace to preview this instance."));
			if (ImGui::Button(T(TKEY("local_ndf_camera"), "Move to camera / current worldspace"))) {
				instance.center = cameraMeters;
				instance.worldspace = worldspace;
			}
			ImGui::DragFloat2(T(TKEY("local_ndf_center"), "World X / Y (m)"), &instance.center.x, 10.f);
			ImGui::DragFloat2(T(TKEY("local_ndf_size"), "Width / length (m)"), &instance.size.x, 10.f, 1.f, 128000.f);
			ImGui::DragFloat(T(TKEY("local_ndf_rotation"), "Rotation (degrees)"), &instance.rotation, 1.f);
			ImGui::DragFloat(T(TKEY("local_ndf_altitude"), "Height zero (m)"), &instance.altitude, 10.f, -24000.f, 48000.f);
			ImGui::DragFloat(T(TKEY("local_ndf_height_span"), "Height span (m)"), &instance.heightSpan, 10.f, 1.f, 24000.f);
			ImGui::SliderFloat(T(TKEY("local_ndf_strength"), "Instance strength"), &instance.strength, 0.f, 1.f);
			ImGui::SliderFloat(T(TKEY("local_ndf_model_weight"), "Modeling influence"), &instance.modelingWeight, 0.f, 1.f);
			ImGui::SliderFloat(T(TKEY("local_ndf_height_weight"), "Height influence"), &instance.heightWeight, 0.f, 1.f);
			ImGui::DragFloat(T(TKEY("local_ndf_feather"), "Edge feather (m)"), &instance.feather, 1.f, 0.f, 64000.f);
			ImGui::Checkbox(T(TKEY("local_ndf_alpha"), "Use modeling alpha as mask"), &instance.modelingAlpha);
			ImGui::InputInt(T(TKEY("local_ndf_priority"), "Priority (higher draws last)"), &instance.priority);
			remove = ImGui::Button(T(TKEY("local_ndf_remove"), "Remove instance"));
			ImGui::TreePop();
		}
		ImGui::PopID();
		if (remove)
			settings.instances.erase(settings.instances.begin() + i);
		else
			++i;
	}
	for (const auto& asset : rejectedAssets)
		ImGui::TextWrapped("%s: %s", T(TKEY("local_ndf_rejected"), "Rejected asset: requires matching linear 2D Height RG and Modeling RGB; optional Mask must be linear R"), asset.c_str());
	ImGui::TreePop();
}

void LocalNdfManager::DrawDebug(TextureManager& textures, float scale)
{
	if (!ImGui::TreeNode(T(TKEY("local_ndf_debug"), "Local NDF resources")))
		return;
	ImGui::Text("%s: %zu", T(TKEY("local_ndf_active"), "Active instances"), activeAssets.size());
	ImGui::Text("%s: %.2f", T(TKEY("local_ndf_effective_texel"), "Effective texel size (m), atlas limited to 2048"), effectiveTexelSize);
	ImGui::Text("XY: %.2f, %.2f; Z: %.2f .. %.2f", rect.x, rect.y, altitude.x, altitude.x + altitude.y);
	auto* sky = PhysicalSky::GetSingleton();
	const auto maps = GetTextures();
	sky->DrawDebugCloudTexture(maps.height, "localHeight", T(TKEY("local_ndf_debug_height"), "Composed height RG / height weight B / shaping A"), scale);
	sky->DrawDebugCloudTexture(maps.modeling, "localModeling", T(TKEY("local_ndf_debug_model"), "Composed modeling RGB / influence A"), scale);
	sky->DrawDebugCloudTexture(scratchHeight ? scratchHeight->srv.get() : nullptr, "localScratchHeight", T(TKEY("local_ndf_scratch_height"), "Composition scratch height"), scale);
	sky->DrawDebugCloudTexture(scratchModeling ? scratchModeling->srv.get() : nullptr, "localScratchModel", T(TKEY("local_ndf_scratch_model"), "Composition scratch modeling"), scale);
	auto sources = activeAssets;
	sources.insert(sources.end(), rejectedAssets.begin(), rejectedAssets.end());
	std::ranges::sort(sources);
	sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
	for (size_t i = 0; i < sources.size(); ++i) {
		const auto path = AssetPath(sources[i]);
		if (path.empty())
			continue;
		ImGui::PushID(static_cast<int>(i));
		ImGui::TextUnformatted(sources[i].c_str());
		sky->DrawDebugCloudTexture(textures.Query((path / "Height.dds").string()), "heightInput", T(TKEY("local_ndf_height_input"), "Height input"), scale);
		sky->DrawDebugCloudTexture(textures.Query((path / "Modeling.dds").string()), "modelInput", T(TKEY("local_ndf_model_input"), "Modeling input / alpha mask"), scale);
		sky->DrawDebugCloudTexture(textures.Query((path / "Mask.dds").string()), "maskInput", T(TKEY("local_ndf_mask_input"), "Optional mask input"), scale, true);
		ImGui::PopID();
	}
	ImGui::TreePop();
}

#undef I18N_KEY_PREFIX
