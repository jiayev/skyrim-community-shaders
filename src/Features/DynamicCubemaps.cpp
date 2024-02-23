#include "DynamicCubemaps.h"
#include <Util.h>

constexpr auto MIPLEVELS = 10;

void DynamicCubemaps::DrawSettings()
{
	if (ImGui::TreeNodeEx("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (REL::Module::IsVR()) {
			if (ImGui::BeginTable("##SettingsToggles", 3, ImGuiTableFlags_SizingStretchSame)) {
				for (const auto& settingName : iniVRCubeMapSettings) {
					if (auto setting = RE::INISettingCollection::GetSingleton()->GetSetting(settingName); setting) {
						ImGui::TableNextColumn();
						ImGui::Checkbox(settingName.c_str(), &setting->data.b);
						if (auto _tt = Util::HoverTooltipWrapper()) {
							//ImGui::Text(fmt::format(fmt::runtime("{} {0:x}"), settingName, &setting->data.b).c_str());
							ImGui::Text(settingName.c_str());
						}
					}
				}
				for (const auto& settingPair : hiddenVRCubeMapSettings) {
					const auto& settingName = settingPair.first;
					const auto address = REL::Offset{ settingPair.second }.address();
					bool* setting = reinterpret_cast<bool*>(address);
					ImGui::TableNextColumn();
					ImGui::Checkbox(settingName.c_str(), setting);
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text(settingName.c_str());
						//ImGui::Text(fmt::format(fmt::runtime("{} {0:x}"), settingName, address).c_str());
					}
				}
				ImGui::EndTable();
			}
		}
		ImGui::Spacing();
		ImGui::Spacing();

		ImGui::TreePop();
	}
}

void DynamicCubemaps::DataLoaded()
{
	if (REL::Module::IsVR()) {
		// enable cubemap settings in VR
		for (const auto& settingName : iniVRCubeMapSettings) {
			if (auto setting = RE::INISettingCollection::GetSingleton()->GetSetting(settingName); setting) {
				if (!setting->data.b) {
					logger::info("[DC] Changing {} from {} to {} to support Dynamic Cubemaps", settingName, setting->data.b, true);
					setting->data.b = true;
				}
			}
		}
		for (const auto& settingPair : hiddenVRCubeMapSettings) {
			const auto& settingName = settingPair.first;
			const auto address = REL::Offset{ settingPair.second }.address();
			bool* setting = reinterpret_cast<bool*>(address);
			if (!*setting) {
				logger::info("[DC] Changing {} from {} to {} to support Dynamic Cubemaps", settingName, *setting, true);
				*setting = true;
			}
		}
	}
	MenuOpenCloseEventHandler::Register();
}

RE::BSEventNotifyControl MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// When entering a new cell, reset the capture
	if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
		if (!a_event->opening)
			DynamicCubemaps::GetSingleton()->resetCapture = true;
	}
	return RE::BSEventNotifyControl::kContinue;
}

bool MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	auto ui = RE::UI::GetSingleton();

	if (!ui) {
		logger::error("UI event source not found");
		return false;
	}

	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);

	logger::info("Registered {}", typeid(singleton).name());

	return true;
}

void DynamicCubemaps::ClearShaderCache()
{
	if (updateCubemapCS) {
		updateCubemapCS->Release();
		updateCubemapCS = nullptr;
	}
	if (inferCubemapCS) {
		inferCubemapCS->Release();
		inferCubemapCS = nullptr;
	}
	if (inferCubemapReflectionsCS) {
		inferCubemapReflectionsCS->Release();
		inferCubemapReflectionsCS = nullptr;
	}
	if (specularIrradianceCS) {
		specularIrradianceCS->Release();
		specularIrradianceCS = nullptr;
	}
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderUpdate()
{
	if (!updateCubemapCS) {
		logger::debug("Compiling UpdateCubemapCS");
		updateCubemapCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\UpdateCubemapCS.hlsl", {}, "cs_5_0");
	}
	return updateCubemapCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderInferrence()
{
	if (!inferCubemapCS) {
		logger::debug("Compiling InferCubemapCS");
		inferCubemapCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\InferCubemapCS.hlsl", {}, "cs_5_0");
	}
	return inferCubemapCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderInferrenceReflections()
{
	if (!inferCubemapReflectionsCS) {
		logger::debug("Compiling InferCubemapCS REFLECTIONS");
		inferCubemapReflectionsCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\InferCubemapCS.hlsl", { { "REFLECTIONS", "" } }, "cs_5_0");
	}
	return inferCubemapReflectionsCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderSpecularIrradiance()
{
	if (!specularIrradianceCS) {
		logger::debug("Compiling SpecularIrradianceCS");
		specularIrradianceCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\SpecularIrradianceCS.hlsl", {}, "cs_5_0");
	}
	return specularIrradianceCS;
}

void DynamicCubemaps::UpdateCubemapCapture()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto context = renderer->GetRuntimeData().context;

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto& snowSwap = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP];

	ID3D11ShaderResourceView* srvs[2] = { depth.depthSRV, snowSwap.SRV };
	context->CSSetShaderResources(0, 2, srvs);

	ID3D11UnorderedAccessView* uavs[3] = { envCaptureTexture->uav.get(), envCaptureRawTexture->uav.get(), envCapturePositionTexture->uav.get() };
	context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

	ID3D11Buffer* buffers[2];
	context->PSGetConstantBuffers(12, 1, buffers);

	UpdateCubemapCB updateData{};
	updateData.CameraData = Util::GetCameraData();
	updateData.Reset = resetCapture;

	static float3 cameraPreviousPosAdjust = { 0, 0, 0 };
	updateData.CameraPreviousPosAdjust = cameraPreviousPosAdjust;

	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	auto eyePosition = !REL::Module::IsVR() ?
	                       state->GetRuntimeData().posAdjust.getEye(0) :
	                       state->GetVRRuntimeData().posAdjust.getEye(0);

	cameraPreviousPosAdjust = { eyePosition.x, eyePosition.y, eyePosition.z };

	updateCubemapCB->Update(updateData);
	buffers[1] = updateCubemapCB->CB();

	context->CSSetConstantBuffers(0, 2, buffers);

	resetCapture = false;

	context->CSSetSamplers(0, 1, &computeSampler);

	context->CSSetShader(GetComputeShaderUpdate(), nullptr, 0);
	context->Dispatch((uint32_t)std::ceil(envCaptureTexture->desc.Width / 32.0f), (uint32_t)std::ceil(envCaptureTexture->desc.Height / 32.0f), 6);

	uavs[0] = nullptr;
	uavs[1] = nullptr;
	uavs[2] = nullptr;
	context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

	srvs[0] = nullptr;
	srvs[1] = nullptr;
	context->CSSetShaderResources(0, 2, srvs);

	buffers[0] = nullptr;
	buffers[1] = nullptr;
	context->CSSetConstantBuffers(0, 2, buffers);

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11SamplerState* nullSampler = { nullptr };
	context->CSSetSamplers(0, 1, &nullSampler);
}

void DynamicCubemaps::DrawDeferred()
{
	auto shadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

	if (shadowSceneNode == accumulator->GetRuntimeData().activeShadowSceneNode) {
		if (nextTask == NextTask::kCapture) {
			UpdateCubemapCapture();
			nextTask = NextTask::kInferrence;
		}
	}
}

void DynamicCubemaps::UpdateCubemap()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	//if (!REL::Module::IsVR()) {
	//	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	//	imageSpaceManager->GetRuntimeData().BSImagespaceShaderApplyReflections->active = false;
	//}

	{
		ID3D11ShaderResourceView* view = nullptr;
		context->PSSetShaderResources(64, 1, &view);
	}

	if (nextTask == NextTask::kInferrence) {
		nextTask = NextTask::kIrradiance;

		// Infer local reflection information
		ID3D11UnorderedAccessView* uavs[2] = { envInferredTexture->uav.get(), cubemapUAV };

		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

		context->GenerateMips(envCaptureTexture->srv.get());

		auto srv = envCaptureTexture->srv.get();
		context->CSSetShaderResources(0, 1, &srv);

		context->CSSetSamplers(0, 1, &computeSampler);

		context->CSSetShader(activeReflections ? GetComputeShaderInferrenceReflections() : GetComputeShaderInferrence(), nullptr, 0);

		context->Dispatch((uint32_t)std::ceil(envCaptureTexture->desc.Width / 32.0f), (uint32_t)std::ceil(envCaptureTexture->desc.Height / 32.0f), 6);

		srv = nullptr;
		context->CSSetShaderResources(0, 1, &srv);

		uavs[0] = nullptr;
		uavs[1] = nullptr;

		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

		context->CSSetShader(nullptr, 0, 0);

		ID3D11SamplerState* sampler = nullptr;
		context->CSSetSamplers(0, 1, &sampler);
	} else if (nextTask == NextTask::kIrradiance) {
		nextTask = NextTask::kCapture;

		// Copy cubemap to other resources
		for (uint face = 0; face < 6; face++) {
			uint srcSubresourceIndex = D3D11CalcSubresource(0, face, MIPLEVELS);
			context->CopySubresourceRegion(envTexture->resource.get(), D3D11CalcSubresource(0, face, MIPLEVELS), 0, 0, 0, envInferredTexture->resource.get(), srcSubresourceIndex, nullptr);
		}

		// Compute pre-filtered specular environment map.
		{
			auto srv = envInferredTexture->srv.get();
			context->GenerateMips(srv);

			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetSamplers(0, 1, &computeSampler);
			context->CSSetShader(GetComputeShaderSpecularIrradiance(), nullptr, 0);

			ID3D11Buffer* buffer = spmapCB->CB();
			context->CSSetConstantBuffers(0, 1, &buffer);

			float const delta_roughness = 1.0f / std::max(float(MIPLEVELS - 1), 1.0f);

			std::uint32_t size = std::max(envTexture->desc.Width, envTexture->desc.Height);

			for (std::uint32_t level = 1; level < MIPLEVELS; level++, size /= 2) {
				const UINT numGroups = (UINT)std::max(1u, size / 32);

				const SpecularMapFilterSettingsCB spmapConstants = { level * delta_roughness };
				spmapCB->Update(spmapConstants);

				auto uav = uavArray[level - 1].get();

				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				context->Dispatch(numGroups, numGroups, 6);
			}
		}

		ID3D11ShaderResourceView* nullSRV = { nullptr };
		ID3D11SamplerState* nullSampler = { nullptr };
		ID3D11Buffer* nullBuffer = { nullptr };
		ID3D11UnorderedAccessView* nullUAV = { nullptr };

		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetSamplers(0, 1, &nullSampler);
		context->CSSetShader(nullptr, 0, 0);
		context->CSSetConstantBuffers(0, 1, &nullBuffer);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	}
}

void DynamicCubemaps::Draw(const RE::BSShader* shader, const uint32_t)
{
	if (shader->shaderType.get() == RE::BSShader::Type::Lighting || shader->shaderType.get() == RE::BSShader::Type::Water) {
		// During world cubemap generation we cannot use the cubemap
		auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
		auto cubeMapRenderTarget = !REL::Module::IsVR() ? shadowState->GetRuntimeData().cubeMapRenderTarget : shadowState->GetVRRuntimeData().cubeMapRenderTarget;
		if (cubeMapRenderTarget != RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS && !renderedScreenCamera) {
			UpdateCubemap();
			renderedScreenCamera = true;

			auto renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto context = renderer->GetRuntimeData().context;

			if (REL::Module::IsVR()) {
				ID3D11ShaderResourceView* views[2]{};
				views[0] = envTexture->srv.get();
				views[1] = spBRDFLUT->srv.get();
				context->PSSetShaderResources(64, 2, views);
			} else {
				auto ssrBlurred = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSSR];
				auto ssrRaw = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSSR_RAW];

				ID3D11ShaderResourceView* views[4]{};
				views[0] = envTexture->srv.get();
				views[1] = spBRDFLUT->srv.get();
				views[2] = ssrBlurred.SRV;
				views[3] = ssrRaw.SRV;
				context->PSSetShaderResources(64, 4, views);
			}
		}
	}
}

void DynamicCubemaps::SetupResources()
{
	GetComputeShaderUpdate();
	GetComputeShaderInferrence();
	GetComputeShaderInferrenceReflections();
	GetComputeShaderSpecularIrradiance();

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;
	auto device = renderer->GetRuntimeData().forwarder;

	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &computeSampler));
	}

	{
		spBRDFProgram = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\SpbrdfCS.hlsl", {}, "cs_5_0");

		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = 256;
		texDesc.Height = 256;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		spBRDFLUT = new Texture2D(texDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		spBRDFLUT->CreateUAV(uavDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		spBRDFLUT->CreateSRV(srvDesc);

		{
			// Compute Cook-Torrance BRDF 2D LUT for split-sum approximation.
			auto uav = spBRDFLUT->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(spBRDFProgram, nullptr, 0);
			context->Dispatch(spBRDFLUT->desc.Width / 32, spBRDFLUT->desc.Height / 32, 1);
			uav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		}
	}

	auto& cubemap = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];

	{
		// Backup and release the original texture and views

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		cubemap.SRV->GetDesc(&srvDesc);
		cubemap.SRV->Release();
		cubemap.SRV = nullptr;

		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc[6];
		for (int i = 0; i < 6; i++) {
			cubemap.cubeSideRTV[i]->GetDesc(&rtvDesc[i]);
			cubemap.cubeSideRTV[i]->Release();
			cubemap.cubeSideRTV[i] = nullptr;
		}

		D3D11_TEXTURE2D_DESC texDesc;
		cubemap.texture->GetDesc(&texDesc);
		cubemap.texture->Release();
		cubemap.texture = nullptr;

		// Create the new texture and views with UAV access

		texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, &cubemap.texture));
		DX::ThrowIfFailed(device->CreateShaderResourceView(cubemap.texture, &srvDesc, &cubemap.SRV));

		for (int i = 0; i < 6; i++) {
			DX::ThrowIfFailed(device->CreateRenderTargetView(cubemap.texture, &rtvDesc[i], &cubemap.cubeSideRTV[i]));
		}

		// Create additional resources

		texDesc.MipLevels = MIPLEVELS;
		texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
		srvDesc.TextureCube.MipLevels = MIPLEVELS;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.MipSlice = 0;
		uavDesc.Texture2DArray.FirstArraySlice = 0;
		uavDesc.Texture2DArray.ArraySize = texDesc.ArraySize;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(cubemap.texture, &uavDesc, &cubemapUAV));

		envTexture = new Texture2D(texDesc);
		envTexture->CreateSRV(srvDesc);
		envTexture->CreateUAV(uavDesc);

		envCaptureTexture = new Texture2D(texDesc);
		envCaptureTexture->CreateSRV(srvDesc);
		envCaptureTexture->CreateUAV(uavDesc);

		envCaptureRawTexture = new Texture2D(texDesc);
		envCaptureRawTexture->CreateSRV(srvDesc);
		envCaptureRawTexture->CreateUAV(uavDesc);

		envInferredTexture = new Texture2D(texDesc);
		envInferredTexture->CreateSRV(srvDesc);
		envInferredTexture->CreateUAV(uavDesc);

		envCapturePositionTexture = new Texture2D(texDesc);
		envCapturePositionTexture->CreateSRV(srvDesc);
		envCapturePositionTexture->CreateUAV(uavDesc);

		updateCubemapCB = new ConstantBuffer(ConstantBufferDesc<UpdateCubemapCB>());
	}

	{
		spmapCB = new ConstantBuffer(ConstantBufferDesc<SpecularMapFilterSettingsCB>());
	}

	{
		D3D11_TEXTURE2D_DESC texDesc{};
		cubemap.texture->GetDesc(&texDesc);
		texDesc.MipLevels = 1;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
		srvDesc.TextureCube.MostDetailedMip = 0;
		srvDesc.TextureCube.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = envTexture->desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;

		uavDesc.Texture2DArray.FirstArraySlice = 0;
		uavDesc.Texture2DArray.ArraySize = envTexture->desc.ArraySize;

		for (std::uint32_t level = 1; level < MIPLEVELS; ++level) {
			uavDesc.Texture2DArray.MipSlice = level;
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(envTexture->resource.get(), &uavDesc, uavArray[level - 1].put()));
		}
	}
}

void DynamicCubemaps::Reset()
{
	if (auto sky = RE::Sky::GetSingleton())
		activeReflections = sky->mode.get() == RE::Sky::Mode::kFull;
	else
		activeReflections = false;

	auto setting = RE::GetINISetting("fCubeMapRefreshRate:Water");
	setting->data.f = activeReflections ? 0 : FLT_MAX;

	renderedScreenCamera = false;
}

void DynamicCubemaps::Load(json& o_json)
{
	Feature::Load(o_json);
}

void DynamicCubemaps::Save(json&)
{
}

void DynamicCubemaps::RestoreDefaultSettings()
{
}