#include "TerrainOcclusion.h"
#include "Menu.h"

#include "Deferred.h"
#include "State.h"
#include "Util.h"

#include <filesystem>

#include <DirectXTex.h>
#include <pystring/pystring.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainOcclusion::Settings::AOGenSettings,
	AoDistance,
	SliceCount,
	SampleCount)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainOcclusion::Settings::EffectSettings,
	EnableTerrainShadow,
	EnableTerrainAO,
	HeightBias,
	ShadowSofteningRadiusAngle,
	AOPower,
	AOFadeOutHeight)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainOcclusion::Settings,
	AoGen,
	Effect)

void TerrainOcclusion::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void TerrainOcclusion::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void TerrainOcclusion::DrawSettings()
{
	ImGui::Checkbox("Enable Terrain Shadow", (bool*)&settings.Effect.EnableTerrainShadow);
	ImGui::Checkbox("Enable Terrain AO", (bool*)&settings.Effect.EnableTerrainAO);

	ImGui::SliderFloat("Height Map Bias", &settings.Effect.HeightBias, -2000.f, 0.f, "%.0f units");

	ImGui::SeparatorText("Shadow");
	{
		// ImGui::SliderAngle("Softening", &settings.Effect.ShadowSofteningRadiusAngle, .1f, 10.f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
		// if (auto _tt = Util::HoverTooltipWrapper())
		// 	ImGui::Text("Controls the solid angle of sunlight, making terrain shadows softer.");

		ImGui::SliderFloat2("Fade Distance", &settings.Effect.ShadowFadeDistance.x, 0, 10000.f, "%.0f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Shadows around you are and should be handled by vanilla shadow maps.");
			if (auto settingCollection = RE::INIPrefSettingCollection::GetSingleton()) {
				auto gameShadowDist = settingCollection->GetSetting("fShadowDistance:Display")->GetFloat();
				ImGui::Text("Your fShadowDistance setting: %f", gameShadowDist);
			}
		}
	}

	ImGui::SeparatorText("AO");
	{
		ImGui::SliderFloat("Mix", &settings.Effect.AOMix, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat("Power", &settings.Effect.AOPower, 0.2f, 5, "%.2f");
		ImGui::SliderFloat("Fadeout Height", &settings.Effect.AOFadeOutHeight, 500, 5000, "%.0f units");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("On the ground AO is the most prominent. Up to a certain height it will gradually fade out.");

		if (ImGui::TreeNodeEx("Precomputation", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat("Distance", &settings.AoGen.AoDistance, 1.f / 32, 32, "%.2f cells");
			ImGui::InputScalar("Slices", ImGuiDataType_U32, &settings.AoGen.SliceCount);
			ImGui::InputScalar("Samples", ImGuiDataType_U32, &settings.AoGen.SampleCount);
			if (ImGui::Button("Force Regenerate AO", { -1, 0 }))
				needPrecompute = true;

			ImGui::TreePop();
		}
	}

	if (ImGui::CollapsingHeader("Debug")) {
		std::string curr_worldspace = "N/A";
		std::string curr_worldspace_name = "N/A";
		auto tes = RE::TES::GetSingleton();
		if (tes) {
			auto worldspace = tes->GetRuntimeData2().worldSpace;
			if (worldspace) {
				curr_worldspace = worldspace->GetFormEditorID();
				curr_worldspace_name = worldspace->GetName();
			}
		}
		ImGui::Text(fmt::format("Current worldspace: {} ({})", curr_worldspace, curr_worldspace_name).c_str());
		ImGui::Text(fmt::format("Has height map: {}", heightmaps.contains(curr_worldspace)).c_str());

		ImGui::Separator();

		if (texOcclusion) {
			ImGui::BulletText("shadowUpdateCBData");
			ImGui::Indent();
			{
				ImGui::Text(fmt::format("LightPxDir: ({}, {})", shadowUpdateCBData.LightPxDir.x, shadowUpdateCBData.LightPxDir.y).c_str());
				ImGui::Text(fmt::format("LightDeltaZ: ({}, {})", shadowUpdateCBData.LightDeltaZ.x, shadowUpdateCBData.LightDeltaZ.y).c_str());
				ImGui::Text(fmt::format("StartPxCoord: {}", shadowUpdateCBData.StartPxCoord).c_str());
				ImGui::Text(fmt::format("PxSize: ({}, {})", shadowUpdateCBData.PxSize.x, shadowUpdateCBData.PxSize.y).c_str());
			}
			ImGui::Unindent();

			if (ImGui::TreeNode("Buffer Viewer")) {
				static float debugRescale = .1f;
				ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

				BUFFER_VIEWER_NODE_BULLET(texOcclusion, debugRescale)
				BUFFER_VIEWER_NODE_BULLET(texNormalisedHeight, debugRescale)
				BUFFER_VIEWER_NODE_BULLET(texShadowHeight, debugRescale)
				ImGui::TreePop();
			}
		}
	}
}

void TerrainOcclusion::ClearShaderCache()
{
	if (occlusionProgram) {
		occlusionProgram->Release();
		occlusionProgram = nullptr;
	}
	if (shadowUpdateProgram) {
		shadowUpdateProgram->Release();
		shadowUpdateProgram = nullptr;
	}
	if (outputProgram) {
		outputProgram->Release();
		outputProgram = nullptr;
	}

	CompileComputeShaders();
}

void TerrainOcclusion::SetupResources()
{
	logger::debug("Listing height maps...");
	{
		std::filesystem::path texture_dir{ L"Data\\textures\\heightmaps\\" };
		for (auto const& dir_entry : std::filesystem::directory_iterator{ texture_dir }) {
			auto filename = dir_entry.path().filename();
			if (filename.extension() != ".dds")
				continue;

			logger::debug("Found dds: {}", filename.string());

			auto splitstr = pystring::split(filename.stem().string(), ".");

			if (splitstr.size() != 10)
				logger::warn("{} has incorrect number ({} instead of 10) of fields", filename.string(), splitstr.size());

			if (splitstr[1] == "HeightMap") {
				HeightMapMetadata metadata;
				try {
					metadata.worldspace = splitstr[0];
					metadata.pos0.x = std::stoi(splitstr[2]) * 4096.f;
					metadata.pos1.y = std::stoi(splitstr[3]) * 4096.f;
					metadata.pos1.x = (std::stoi(splitstr[4]) + 1) * 4096.f;
					metadata.pos0.y = (std::stoi(splitstr[5]) + 1) * 4096.f;
					metadata.pos0.z = std::stoi(splitstr[6]) * 8.f;
					metadata.pos1.z = std::stoi(splitstr[7]) * 8.f;
					metadata.zRange.x = std::stoi(splitstr[8]) * 8.f;
					metadata.zRange.y = std::stoi(splitstr[9]) * 8.f;
				} catch (std::exception& e) {
					logger::warn("Failed to parse {}. Error: {}", filename.string(), e.what());
					continue;
				}

				metadata.dir = dir_entry.path().parent_path().wstring();
				metadata.filename = filename.string();

				if (heightmaps.contains(metadata.worldspace)) {
					logger::warn("{} has more than one height maps!", metadata.worldspace);
				} else {
					heightmaps[metadata.worldspace] = metadata;
				}
			} else if (splitstr[1] != "AO" && splitstr[1] != "Cone")
				logger::warn("{} has unknown type ({})", filename.string(), splitstr[1]);
		}
	}

	logger::debug("Creating structured buffers...");
	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(AOGenBuffer);
		sbDesc.ByteWidth = sizeof(AOGenBuffer);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;

		aoGenBuffer = std::make_unique<Buffer>(sbDesc);
		aoGenBuffer->CreateSRV(srvDesc);

		sbDesc.StructureByteStride = sizeof(PerPass);
		sbDesc.ByteWidth = sizeof(PerPass);

		perPass = std::make_unique<Buffer>(sbDesc);
		perPass->CreateSRV(srvDesc);
	}

	logger::debug("Creating constant buffers...");
	{
		shadowUpdateCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ShadowUpdateCB>());
	}

	CompileComputeShaders();
}

void TerrainOcclusion::CompileComputeShaders()
{
	logger::debug("Compiling shaders...");
	{
		auto program_ptr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainOcclusion\\AOGen.cs.hlsl", {}, "cs_5_0"));
		if (program_ptr)
			occlusionProgram.attach(program_ptr);

		program_ptr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainOcclusion\\ShadowUpdate.cs.hlsl", {}, "cs_5_0"));
		if (program_ptr)
			shadowUpdateProgram.attach(program_ptr);

		program_ptr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainOcclusion\\Output.cs.hlsl", {}, "cs_5_0"));
		if (program_ptr)
			outputProgram.attach(program_ptr);
	}
}

bool TerrainOcclusion::IsHeightMapReady()
{
	if (auto tes = RE::TES::GetSingleton())
		if (auto worldspace = tes->GetRuntimeData2().worldSpace)
			return cachedHeightmap && cachedHeightmap->worldspace == worldspace->GetFormEditorID();
	return false;
}

void TerrainOcclusion::Draw(const RE::BSShader*, const uint32_t)
{
}

void TerrainOcclusion::UpdateBuffer()
{
	auto& context = State::GetSingleton()->context;

	bool isHeightmapReady = IsHeightMapReady();

	PerPass data = {
		.effect = settings.Effect,
	};
	data.effect.EnableTerrainAO = data.effect.EnableTerrainAO && isHeightmapReady;
	data.effect.EnableTerrainShadow = data.effect.EnableTerrainShadow && isHeightmapReady;

	if (isHeightmapReady) {
		data.effect.AOFadeOutHeight = 1.f / data.effect.AOFadeOutHeight;

		data.invScale = cachedHeightmap->pos1 - cachedHeightmap->pos0;
		data.scale = float3(1.f, 1.f, 1.f) / data.invScale;
		data.offset = -cachedHeightmap->pos0 * data.scale;
		data.zRange = cachedHeightmap->zRange;
	}

	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	size_t bytes = sizeof(PerPass);
	memcpy_s(mapped.pData, bytes, &data, bytes);
	context->Unmap(perPass->resource.get(), 0);
}

void TerrainOcclusion::LoadHeightmap()
{
	auto tes = RE::TES::GetSingleton();
	if (!tes)
		return;
	auto worldspace = tes->GetRuntimeData2().worldSpace;
	if (!worldspace)
		return;
	std::string worldspace_name = worldspace->GetFormEditorID();
	if (!heightmaps.contains(worldspace_name))  // no height map for that, but we don't remove cache
		return;
	if (cachedHeightmap && cachedHeightmap->worldspace == worldspace_name)  // already cached
		return;

	auto& device = State::GetSingleton()->device;

	logger::debug("Loading height map...");
	{
		auto& target_heightmap = heightmaps[worldspace_name];

		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ target_heightmap.dir };
			path /= target_heightmap.filename;

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texHeightMap.release();
		texHeightMap = std::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texHeightMap->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texHeightMap->CreateSRV(srvDesc);

		cachedHeightmap = &heightmaps[worldspace_name];
	}

	shadowUpdateIdx = 0;
	needPrecompute = true;
}

void TerrainOcclusion::Precompute()
{
	if (!cachedHeightmap)
		return;

	auto& context = State::GetSingleton()->context;

	logger::info("Creating occlusion texture...");
	{
		texOcclusion.release();
		texNormalisedHeight.release();
		texShadowHeight.release();

		D3D11_TEXTURE2D_DESC texDesc = {
			.Width = texHeightMap->desc.Width,
			.Height = texHeightMap->desc.Height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R8_UNORM,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texOcclusion = std::make_unique<Texture2D>(texDesc);
		texOcclusion->CreateSRV(srvDesc);
		texOcclusion->CreateUAV(uavDesc);

		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16_FLOAT;
		texNormalisedHeight = std::make_unique<Texture2D>(texDesc);
		texNormalisedHeight->CreateSRV(srvDesc);
		texNormalisedHeight->CreateUAV(uavDesc);

		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		texShadowHeight = std::make_unique<Texture2D>(texDesc);
		texShadowHeight->CreateSRV(srvDesc);
		texShadowHeight->CreateUAV(uavDesc);
	}

	{
		AOGenBuffer data = {
			.settings = settings.AoGen,
			.pos0 = cachedHeightmap->pos0,
			.pos1 = cachedHeightmap->pos1,
			.zRange = cachedHeightmap->zRange
		};

		data.settings.AoDistance *= 4096.f;

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(aoGenBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(AOGenBuffer);
		memcpy_s(mapped.pData, bytes, &data, bytes);
		context->Unmap(aoGenBuffer->resource.get(), 0);
	}

	/* ---- BACKUP ---- */
	struct ShaderState
	{
		ID3D11ShaderResourceView* srvs[2] = { nullptr };
		ID3D11ComputeShader* shader = nullptr;
		ID3D11UnorderedAccessView* uavs[2] = { nullptr };
		ID3D11ClassInstance* instance = nullptr;
		ID3D11SamplerState* samplers[1] = { nullptr };
		UINT numInstances;
	} old, newer;
	context->CSGetShaderResources(0, ARRAYSIZE(old.srvs), old.srvs);
	context->CSGetShader(&old.shader, &old.instance, &old.numInstances);
	context->CSGetUnorderedAccessViews(0, ARRAYSIZE(old.uavs), old.uavs);
	context->CSGetSamplers(0, ARRAYSIZE(old.samplers), old.samplers);

	/* ---- DISPATCH ---- */
	logger::info("Precomputation...");
	newer.srvs[0] = aoGenBuffer->srv.get();
	newer.srvs[1] = texHeightMap->srv.get();
	newer.uavs[0] = texOcclusion->uav.get();
	newer.uavs[1] = texNormalisedHeight->uav.get();

	context->CSSetSamplers(0, ARRAYSIZE(newer.samplers), newer.samplers);
	context->CSSetShaderResources(0, ARRAYSIZE(newer.srvs), newer.srvs);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(newer.uavs), newer.uavs, nullptr);
	context->CSSetShader(occlusionProgram.get(), nullptr, 0);
	context->Dispatch(((texOcclusion->desc.Width - 1) >> 5) + 1, ((texOcclusion->desc.Height - 1) >> 5) + 1, 1);

	/* ---- RESTORE ---- */
	context->CSSetShaderResources(0, ARRAYSIZE(old.srvs), old.srvs);
	context->CSSetShader(old.shader, &old.instance, old.numInstances);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(old.uavs), old.uavs, nullptr);
	context->CSSetSamplers(0, ARRAYSIZE(old.samplers), old.samplers);

	needPrecompute = false;
}

void TerrainOcclusion::UpdateShadow()
{
	if (!IsHeightMapReady())
		return;

	auto& context = State::GetSingleton()->context;
	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
	auto sunLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());
	if (!sunLight)
		return;

	/* ---- UPDATE CB ---- */
	uint width = texNormalisedHeight->desc.Width;
	uint height = texNormalisedHeight->desc.Height;

	// only update direction at the start of each cycle
	static float2 cachedDirLightPxDir;
	static float2 cachedDirLightDZRange;
	static uint edgePxCoord;
	static int signDir;
	static uint maxUpdates;
	if (shadowUpdateIdx == 0) {
		auto direction = sunLight->GetWorldDirection();
		float3 dirLightDir = { direction.x, direction.y, direction.z };
		if (dirLightDir.z > 0)
			dirLightDir = -dirLightDir;

		// in UV
		float3 invScale = cachedHeightmap->pos1 - cachedHeightmap->pos0;
		invScale.z = cachedHeightmap->zRange.y - cachedHeightmap->zRange.x;
		float3 dirLightPxDir = dirLightDir / invScale;
		dirLightPxDir.x *= width;
		dirLightPxDir.y *= height;

		float stepMult;
		if (abs(dirLightPxDir.x) >= abs(dirLightPxDir.y)) {
			stepMult = 1.f / abs(dirLightPxDir.x);
			edgePxCoord = dirLightPxDir.x > 0 ? 0 : (width - 1);
			signDir = dirLightPxDir.x > 0 ? 1 : -1;
			maxUpdates = ((width - 1) >> 10) + 1;
		} else {
			stepMult = 1.f / abs(dirLightPxDir.y);
			edgePxCoord = dirLightPxDir.y > 0 ? 0 : height - 1;
			signDir = dirLightPxDir.y > 0 ? 1 : -1;
			maxUpdates = ((height - 1) >> 10) + 1;
		}
		dirLightPxDir *= stepMult;

		cachedDirLightPxDir = { dirLightPxDir.x, dirLightPxDir.y };

		// soft shadow angles
		float lenUV = float2{ dirLightDir.x, dirLightDir.y }.Length();
		float dirLightAngle = atan2(-dirLightDir.z, lenUV);
		float upperAngle = std::max(0.f, dirLightAngle - settings.Effect.ShadowSofteningRadiusAngle);
		float lowerAngle = std::min(RE::NI_HALF_PI - 1e-2f, dirLightAngle + settings.Effect.ShadowSofteningRadiusAngle);

		cachedDirLightDZRange = -(lenUV / invScale.z * stepMult) * float2{ std::tan(upperAngle), std::tan(lowerAngle) };
	}

	shadowUpdateCBData = {
		.LightPxDir = cachedDirLightPxDir,
		.LightDeltaZ = cachedDirLightDZRange,
		.StartPxCoord = edgePxCoord + signDir * shadowUpdateIdx * 1024u,
		.PxSize = { 1.f / texNormalisedHeight->desc.Width, 1.f / texNormalisedHeight->desc.Height }
	};
	shadowUpdateCB->Update(shadowUpdateCBData);

	shadowUpdateIdx = (shadowUpdateIdx + 1) % maxUpdates;

	/* ---- BACKUP ---- */
	struct ShaderState
	{
		ID3D11ShaderResourceView* srvs[1] = { nullptr };
		ID3D11ComputeShader* shader = nullptr;
		ID3D11UnorderedAccessView* uavs[1] = { nullptr };
		ID3D11Buffer* buffer = nullptr;
	} old, newer;

	/* ---- DISPATCH ---- */
	newer.srvs[0] = texNormalisedHeight->srv.get();
	newer.uavs[0] = texShadowHeight->uav.get();
	newer.buffer = shadowUpdateCB->CB();

	context->CSSetShaderResources(0, ARRAYSIZE(newer.srvs), newer.srvs);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(newer.uavs), newer.uavs, nullptr);
	context->CSSetConstantBuffers(1, 1, &newer.buffer);
	context->CSSetShader(shadowUpdateProgram.get(), nullptr, 0);
	context->Dispatch(abs(cachedDirLightPxDir.x) >= abs(cachedDirLightPxDir.y) ? height : width, 1, 1);

	/* ---- RESTORE ---- */
	context->CSSetShaderResources(0, ARRAYSIZE(old.srvs), old.srvs);
	context->CSSetShader(old.shader, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(old.uavs), old.uavs, nullptr);
	context->CSSetConstantBuffers(1, 1, &old.buffer);
}

void TerrainOcclusion::DrawTerrainOcclusion()
{
	LoadHeightmap();
	UpdateBuffer();

	if (!settings.Effect.EnableTerrainShadow && !settings.Effect.EnableTerrainAO)
		return;

	if (needPrecompute)
		Precompute();
	if (settings.Effect.EnableTerrainShadow)
		UpdateShadow();

	////////////////////////////////////////////////////////////////////////////////

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& context = State::GetSingleton()->context;
	auto deferred = Deferred::GetSingleton();

	std::array<ID3D11ShaderResourceView*, 5> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 2> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 2> samplers = { nullptr };

	{
		srvs.at(0) = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
		srvs.at(1) = perPass->srv.get();
		if (texOcclusion)
			srvs.at(2) = texOcclusion->srv.get();
		if (texNormalisedHeight)
			srvs.at(3) = texNormalisedHeight->srv.get();
		if (texShadowHeight)
			srvs.at(4) = texShadowHeight->srv.get();

		uavs.at(0) = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK].UAV;
		uavs.at(1) = deferred->giTexture->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(outputProgram.get(), nullptr, 0);
		context->Dispatch((deferred->giTexture->desc.Width + 31u) >> 5, (deferred->giTexture->desc.Height + 31u) >> 5, 1);
	}

	// clean up

	srvs.fill(nullptr);
	uavs.fill(nullptr);
	samplers.fill(nullptr);

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
}
