#include "PhysicalSky.h"
#include "SkySync.h"

#include "State.h"
#include "Util.h"

namespace
{
	void InfoBox(const char* str)
	{
		if (ImGui::BeginTable("Info", 1, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
			ImGui::TableNextColumn();
			ImGui::TextWrapped(str);
			ImGui::EndTable();
		}
	}

	float Lerp(float a, float b, float x) { return a + (b - a) * x; }

	RE::NiColor Float3ToNiColor(const float3& v) { return { v.x, v.y, v.z }; }
}

void PhysicalSky::DataLoaded()
{
	if (!globals::features::skySync.loaded) {
		failedLoadedMessage = "Sky Sync is required for Physical Sky to function.";
		loaded = false;
	}
}

void PhysicalSky::RestoreDefaultSettings() { settings = {}; }
void PhysicalSky::LoadSettings(json&) {}
void PhysicalSky::SaveSettings(json&) {}

void PhysicalSky::DrawSettings()
{
	if (ImGui::BeginTabBar("##PHYSSKY")) {
		if (ImGui::BeginTabItem("General")) {
			SettingsGeneral();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Celestials")) {
			SettingsCelestials();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Atmosphere")) {
			SettingsAtmosphere();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Debug")) {
			SettingsDebug();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

void PhysicalSky::SettingsGeneral()
{
	if (ImGui::BeginTable("Info", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
		ImGui::TableNextColumn();
		ImGui::Text("Shader Status: ");
		ImGui::TableNextColumn();
		if (ShadersOK())
			ImGui::TextColored({ 0, 1, 0, 1 }, "OK");
		else
			ImGui::TextColored({ 1, 0, 0, 1 }, "ERROR");

		ImGui::TableNextColumn();
		ImGui::Text("Worldspace: ");
		ImGui::TableNextColumn();
		if (globals::game::tes)
			if (auto worldspace = globals::game::tes->GetRuntimeData2().worldSpace; worldspace) {
				std::string worldspaceName = worldspace->GetFormEditorID();
				if (settings.worldspaceWhitelist.contains(worldspaceName))
					ImGui::Text("%s (Enabled)", worldspaceName.c_str());
				else
					ImGui::Text("%s (Disabled)", worldspaceName.c_str());
			}

		ImGui::EndTable();
	}

	ImGui::Checkbox("Enabled", &settings.enabled);

	ImGui::SliderFloat("Transmittance Mix", &settings.trMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Apply additional atmospheric tranmisttance on the directional light.\n"
			"Introduces natural yellowening at sunset with white sunlight.");

	ImGui::SeparatorText("Post Processing");
	{
		ImGui::InputFloat("Day Exposure", &settings.dayExposure);
		settings.dayExposure = std::max(1e-10f, settings.dayExposure);
		ImGui::InputFloat("Night Exposure", &settings.nightExposure);
		settings.nightExposure = std::max(1e-10f, settings.nightExposure);
		ImGui::SliderAngle("Adaptation Start", &settings.adaptationStart, -90.f, 0.f);
		ImGui::SliderAngle("Adaptation End", &settings.adaptationEnd, -90.f, 0.f);

		if (ImGui::BeginTable("tonemap", 4, ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
			ImGui::TableNextColumn();
			ImGui::Text("Tonemapper");
			ImGui::TableNextColumn();
			ImGui::RadioButton("Linear", &settings.tonemapper, 0);
			ImGui::TableNextColumn();
			ImGui::RadioButton("Gamma", &settings.tonemapper, 1);
			ImGui::TableNextColumn();
			ImGui::RadioButton("Reinherd", &settings.tonemapper, 2);
			ImGui::EndTable();
		}
		ImGui::SliderFloat("Vanilla Mix", &settings.vanillaMix, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Blend in vanilla sky color.");
	}
}

void PhysicalSky::SettingsCelestials()
{
	constexpr auto lightColorHint = "This sets the light color BEFORE it goes through the atmosphere i.e. extraterrestrial radiance.";

	InfoBox("The sun and moons, and their lights.");

	ImGui::Checkbox("Override Directional Light", &settings.overrideDirLight);

	ImGui::SeparatorText("Sun");
	{
		ImGui::PushID("Sun");
		ImGui::ColorEdit3("Light Color", &settings.sunlightColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(lightColorHint);
		ImGui::PopID();
	}

	ImGui::SeparatorText("Masser");
	{
		ImGui::PushID("Masser");
		ImGui::ColorEdit3("Light Color", &settings.masserColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(lightColorHint);
		ImGui::PopID();
	}

	ImGui::SeparatorText("Secunda");
	{
		ImGui::PushID("Secunda");
		ImGui::ColorEdit3("Light Color", &settings.secundaColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(lightColorHint);
		ImGui::PopID();
	}
}

void PhysicalSky::SettingsAtmosphere()
{
	InfoBox("The composition and physical properties of the atmosphere.");

	ImGui::SeparatorText("Air Molecules (Rayleigh)");
	{
		ImGui::PushID("Rayleigh");
		ImGui::TextWrapped(
			"Particles much smaller than the wavelength of light. They have almost complete symmetry in forward and backward scattering. "
			"On earth, they are what makes the sky blue and, at sunset, red. Usually needs no extra change.");

		ImGui::ColorEdit3("Scatter", &settings.rayleighScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::SliderFloat("Falloff", &settings.rayleighFalloff, 0.f, 2.f, "%.2f km^-1");
		ImGui::PopID();
	}

	ImGui::SeparatorText("Aerosol (Mie)");
	{
		ImGui::PushID("Mie");
		ImGui::TextWrapped(
			"Solid and liquid particles greater than 1/10 of the light wavelength but not too much, like dust. Strongly anisotropic (Mie Scattering). "
			"They contributes to the aureole around bright celestial bodies.");

		ImGui::SliderFloat("Anisotropy", &settings.aerosolPhaseG, -1, 1);
		ImGui::ColorEdit3("Scatter", &settings.aerosolScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit3("Absorption", &settings.aerosolAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Usually 1/9 of scatter coefficient. Dust/pollution is lower, fog is higher.");
		ImGui::SliderFloat("Falloff", &settings.aerosolFalloff, 0.f, 2.f, "%.2f km^-1");
		ImGui::PopID();
	}

	ImGui::SeparatorText("Ozone");
	{
		ImGui::PushID("Ozone");
		ImGui::TextWrapped(
			"The ozone layer high up in the sky that mainly absorbs light of certain wavelength. "
			"It keeps the zenith sky blue, especially at sunrise or sunset.");

		ImGui::ColorEdit3("Absorption", &settings.ozoneAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::DragFloat("Mean Altitude", &settings.ozoneAltitude, .1f, 0.f, 100.f, "%.3f km");
		ImGui::DragFloat("Layer Thickness", &settings.ozoneThickness, .1f, 0.f, 50.f, "%.3f km");
		ImGui::PopID();
	}
}

void PhysicalSky::SettingsDebug()
{
	InfoBox("Beep Boop.");

	if (ImGui::Button("Recompile Shaders"))
		ClearShaderCache();

	ImGui::SeparatorText("Values");
	{
		ImGui::InputFloat3("Sun Direction", &cbData.sunDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		ImGui::InputFloat3("Masser Direction", &cbData.masserDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		ImGui::InputFloat3("Secunda Direction", &cbData.secundaDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
	}

	ImGui::SeparatorText("Textures");
	{
		static float debugScale = 0.2f;
		ImGui::SliderFloat("View Scale", &debugScale, 0.1f, 1.f);

		BUFFER_VIEWER_NODE_BULLET(texTrLut, 1.f);
		BUFFER_VIEWER_NODE_BULLET(texMsLut, 1.f);
		BUFFER_VIEWER_NODE_BULLET(texSvLut, 1.f);
	}
}

void PhysicalSky::SetupResources()
{
	auto device = globals::d3d::device;

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampTr.put()));

		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampSv.put()));

		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampNoise.put()));
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC tex2dDesc{
			.Width = kTrLutW,
			.Height = kTrLutH,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = tex2dDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = tex2dDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texTrLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texTrLut->CreateSRV(srvDesc);
		texTrLut->CreateUAV(uavDesc);

		tex2dDesc.Width = kMsLutW;
		tex2dDesc.Height = kMsLutH;

		texMsLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texMsLut->CreateSRV(srvDesc);
		texMsLut->CreateUAV(uavDesc);

		tex2dDesc.Width = kSvLutW;
		tex2dDesc.Height = kSvLutH;

		texSvLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texSvLut->CreateSRV(srvDesc);
		texSvLut->CreateUAV(uavDesc);

		D3D11_TEXTURE3D_DESC tex3dDesc{
			.Width = kApLutW,
			.Height = kApLutH,
			.Depth = kApLutD,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 };
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
		uavDesc.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = kApLutD };

		texApLut = eastl::make_unique<Texture3D>(tex3dDesc);
		texApLut->CreateSRV(srvDesc);
		texApLut->CreateUAV(uavDesc);
	}

	CompileShaders();
}

void PhysicalSky::ClearShaderCache()
{
	CompileShaders();
}

void PhysicalSky::CompileShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* csPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines = {};
		std::string_view entry = "main";
	};

	std::vector<ShaderCompileInfo> shaderInfos = {
		{ &csTrLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "0" } } },
		{ &csMsLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "1" } } },
		{ &csSvLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "2" } } },
		{ &csApLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "3" } } }
	};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.data())))
			info.csPtr->attach(rawPtr);
	}
}

bool PhysicalSky::ShadersOK()
{
	return csTrLutGen && csMsLutGen && csSvLutGen && csApLutGen;
}

float PhysicalSky::CalcExposure()
{
	auto sunDir = globals::features::skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)];
	float sunAngle = DirectX::XMConvertToRadians(90.f) - acos(sunDir.z);
	float adaptAmount = (sunAngle - settings.adaptationStart) / (settings.adaptationEnd - settings.adaptationStart);
	adaptAmount = std::min(1.f, std::max(0.f, adaptAmount));
	float exposure = settings.dayExposure * exp(log(settings.nightExposure / settings.dayExposure) * adaptAmount);
	return exposure;
}

void PhysicalSky::Reset()
{
	auto& skySync = globals::features::skySync;

	bool allGood = settings.enabled && ShadersOK() && skySync.loaded && skySync.settings.Enabled;

	// check worldspace
	bool worldspace_enabled = false;
	WorldspaceInfo worldspaceInfo = {};
	if (globals::game::tes)
		if (auto worldspace = globals::game::tes->GetRuntimeData2().worldSpace; worldspace) {
			std::string worldspaceName = worldspace->GetFormEditorID();
			worldspace_enabled = settings.worldspaceWhitelist.contains(worldspaceName);
			if (worldspace_enabled)
				worldspaceInfo = settings.worldspaceWhitelist.at(worldspaceName);
		}
	allGood &= worldspace_enabled;

	if (!allGood) {
		cbData.enabled = allGood;
		return;
	}

	// resolution
	float2 res = globals::state->screenSize;
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = { floor(dynres.x), floor(dynres.y) };

	auto sunDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)];
	auto masserDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)];
	auto secundaDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)];

	float exposure = CalcExposure();

	cbData = {
		.texDim = res,
		.rcpTexDim = float2(1.0f) / res,
		.frameDim = dynres,
		.rcpFrameDim = float2(1.0f) / dynres,
		.sunDir = { sunDir.x, sunDir.y, sunDir.z },
		.sunlightColor = settings.sunlightColor * exposure,
		.trMix = settings.trMix,
		.masserDir = { masserDir.x, masserDir.y, masserDir.z },
		.masserColor = settings.masserColor * exposure,
		.secundaDir = { secundaDir.x, secundaDir.y, secundaDir.z },
		.secundaColor = settings.secundaColor * exposure,
		.enabled = allGood,
		.tonemapper = settings.tonemapper,
		.vanillaMix = settings.vanillaMix,
		.zBottom = worldspaceInfo.zBottom,
		.rPlanet = 6.36e3f / Util::Units::GAME_UNIT_TO_KM,
		.rAtmosphere = 6.42e3f / Util::Units::GAME_UNIT_TO_KM,
		.groundAlbedo = settings.groundAlbedo,
		.rayleighFalloff = settings.rayleighFalloff * Util::Units::GAME_UNIT_TO_KM,
		.rayleighScatter = settings.rayleighScatter * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.aerosolFalloff = settings.aerosolFalloff * Util::Units::GAME_UNIT_TO_KM,
		.aerosolPhaseG = settings.aerosolPhaseG,
		.aerosolScatter = settings.aerosolScatter * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.aerosolAbsorption = settings.aerosolAbsorption * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.ozoneAltitude = settings.ozoneAltitude / Util::Units::GAME_UNIT_TO_KM,
		.ozoneThickness = settings.ozoneThickness / Util::Units::GAME_UNIT_TO_KM,
		.ozoneAbsorption = settings.ozoneAbsorption * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
	};

	if (settings.overrideDirLight)
		skySync.lightColors = { Float3ToNiColor(cbData.sunlightColor), Float3ToNiColor(cbData.masserColor), Float3ToNiColor(cbData.secundaColor) };
	else
		skySync.lightColors = std::nullopt;

	RE::NiPoint3 posCam = { 0, 0, 0 };
	if (auto cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
		posCam = cam->cameraRoot->world.translate;
		cbData.zCameraPlanet = posCam.z - cbData.zBottom + cbData.rPlanet;
	}
}

void PhysicalSky::EarlyPrepass()
{
	auto context = globals::d3d::context;
	if (cbData.enabled) {
		GenerateLuts();

		std::array srvs = { texTrLut->srv.get(), texSvLut->srv.get() };
		context->PSSetShaderResources(61, (uint)srvs.size(), srvs.data());
	}
}

void PhysicalSky::ReflectionsPrepass()
{
	auto context = globals::d3d::context;
	if (cbData.enabled) {
		std::array srvs = { texTrLut->srv.get(), texSvLut->srv.get() };
		context->PSSetShaderResources(61, (uint)srvs.size(), srvs.data());
	}
}

void PhysicalSky::GenerateLuts()
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	constexpr auto debugStr = "Physical Sky: LUT Generation";
	state->BeginPerfEvent(debugStr);
	{
		TracyD3D11Zone(state->tracyCtx, debugStr);

		auto samplers = std::array{ sampTr.get(), sampSv.get(), sampNoise.get() };
		std::array<ID3D11ShaderResourceView*, 2> srvs = {};
		ID3D11UnorderedAccessView* uav = nullptr;

		/* ---- DISPATCH ---- */
		context->CSSetSamplers(0, (int)samplers.size(), samplers.data());

		// -> transmittance
		uav = texTrLut->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csTrLutGen.get(), nullptr, 0);
		context->Dispatch((kTrLutW + 7) >> 3, (kTrLutH + 7) >> 3, 1);

		// -> multiscatter
		uav = texMsLut->uav.get();
		srvs.at(0) = texTrLut->srv.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(csMsLutGen.get(), nullptr, 0);
		context->Dispatch((kMsLutW + 7) >> 3, (kMsLutH + 7) >> 3, 1);

		// -> sky-view
		uav = texSvLut->uav.get();
		srvs.at(1) = texMsLut->srv.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(csSvLutGen.get(), nullptr, 0);
		context->Dispatch((kSvLutW + 7) >> 3, (kSvLutH + 7) >> 3, 1);

		// -> aerial perspective
		uav = texApLut->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csApLutGen.get(), nullptr, 0);
		context->Dispatch((kApLutW + 7) >> 3, (kApLutH + 7) >> 3, 1);

		/* ---- RESTORE ---- */
		samplers.fill(nullptr);
		srvs.fill(nullptr);
		uav = nullptr;

		context->CSSetSamplers(0, (int)samplers.size(), samplers.data());
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(nullptr, nullptr, 0);
	}
	state->EndPerfEvent();
}