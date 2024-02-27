#include "SubsurfaceScattering.h"
#include <Util.h>

#include "State.h"

void SubsurfaceScattering::DrawSettings()
{
	if (ImGui::TreeNodeEx("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
	
		ImGui::Spacing();
		ImGui::Spacing();

		ImGui::TreePop();
	}
}

void SubsurfaceScattering::Draw(const RE::BSShader* shader, const uint32_t)
{
	if (shader->shaderType.get() != RE::BSShader::Type::Lighting)
		return;

	if (normalsMode == 0) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto context = renderer->GetRuntimeData().context;

		ID3D11RenderTargetView* views[3];
		ID3D11DepthStencilView* dsv;
		context->OMGetRenderTargets(3, views, &dsv);

		auto normals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK];
		auto normalsSwap = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK_SWAP];

		if (views[2] == normals.RTV)
		{
			normalsMode = 1;
		} else if (views[2] == normalsSwap.RTV)
		{
			normalsMode = 2;
		}

		for (int i = 0; i < 3; i++) {
			if (views[i])
				views[i]->Release();
		}

		if (dsv)
			dsv->Release();
	}
}

void SubsurfaceScattering::SetupResources()
{
	{
		blurCB = new ConstantBuffer(ConstantBufferDesc<BlurCB>());
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(PerPass);
		sbDesc.ByteWidth = sizeof(PerPass);
		perPass = std::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;
		perPass->CreateSRV(srvDesc);
	}

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	{
		auto snowSwapTexture = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP];

		D3D11_TEXTURE2D_DESC texDesc{};
		snowSwapTexture.texture->GetDesc(&texDesc);

		blurHorizontalTemp = new Texture2D(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		snowSwapTexture.SRV->GetDesc(&srvDesc);
		blurHorizontalTemp->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		snowSwapTexture.UAV->GetDesc(&uavDesc);
		blurHorizontalTemp->CreateUAV(uavDesc);
	}

	{
		auto device = renderer->GetRuntimeData().forwarder;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &linearSampler));

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointSampler));
	}
	
}

void SubsurfaceScattering::Reset()
{
	normalsMode = 0;
}

void SubsurfaceScattering::RestoreDefaultSettings()
{
}

void SubsurfaceScattering::Load(json& o_json)
{
	Feature::Load(o_json);
}

void SubsurfaceScattering::Save(json&)
{
}

void SubsurfaceScattering::DrawDeferred()
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

	auto reflections = (!REL::Module::IsVR() ?
							   RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().cubeMapRenderTarget :
							   RE::BSGraphics::RendererShadowState::GetSingleton()->GetVRRuntimeData().cubeMapRenderTarget) == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS;

	if (!(reflections || accumulator->GetRuntimeData().activeShadowSceneNode != RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0])) {
		auto viewport = RE::BSGraphics::State::GetSingleton();

		float resolutionX = blurHorizontalTemp->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		float resolutionY = blurHorizontalTemp->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

		{
			BlurCB data{};

			data.BufferDim.x = (float)blurHorizontalTemp->desc.Width;
			data.BufferDim.y = (float)blurHorizontalTemp->desc.Height;

			data.RcpBufferDim.x = 1.0f / (float)blurHorizontalTemp->desc.Width;
			data.RcpBufferDim.y = 1.0f / (float)blurHorizontalTemp->desc.Height;
			const auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
			auto bTAA = !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled :
			                                   imageSpaceManager->GetVRRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled;
			data.FrameCount = viewport->uiFrameCount * (bTAA || State::GetSingleton()->upscalerLoaded);

			data.DynamicRes.x = viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
			data.DynamicRes.y = viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

			data.DynamicRes.z = 1.0f / data.DynamicRes.x;
			data.DynamicRes.w = 1.0f / data.DynamicRes.y;

			auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			auto cameraData = !REL::Module::IsVR() ? shadowState->GetRuntimeData().cameraData.getEye() :
			                                         shadowState->GetVRRuntimeData().cameraData.getEye();

			data.SSSS_FOVY = atan(1.0f / cameraData.projMat.m[0][0]) * 2.0f * (180.0f / 3.14159265359f);

			data.CameraData = Util::GetCameraData();

			blurCB->Update(data);
		}

		auto renderer = RE::BSGraphics::Renderer::GetSingleton();

		{
			ID3D11Buffer* buffer[1] = { blurCB->CB() };
			context->CSSetConstantBuffers(0, 1, buffer);

			auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
			auto snowSwap = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP];
			auto normals = normalsMode == 1 ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK] : renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK_SWAP];

			ID3D11ShaderResourceView* views[3];
			views[0] = snowSwap.SRV;
			views[1] = depth.depthSRV;
			views[2] = normals.SRV;

			context->CSSetShaderResources(0, 3, views);

			ID3D11UnorderedAccessView* uav = blurHorizontalTemp->uav.get();

			// Horizontal pass to temporary texture
			{
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

				auto shader = GetComputeShaderHorizontalBlur();
				context->CSSetShader(shader, nullptr, 0);

				context->Dispatch((uint32_t)std::ceil(resolutionX / 32.0f), (uint32_t)std::ceil(resolutionY / 32.0f), 1);
			}

			ID3D11ShaderResourceView* view = nullptr;
			context->CSSetShaderResources(2, 1, &view);

			uav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

			// Vertical pass to main texture
			{
				views[0] = blurHorizontalTemp->srv.get();
				context->CSSetShaderResources(0, 1, views);

				ID3D11UnorderedAccessView* uavs[2] = { snowSwap.UAV, normals.UAV };
 				context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

				auto shader = GetComputeShaderVerticalBlur();
				context->CSSetShader(shader, nullptr, 0);

				context->Dispatch((uint32_t)std::ceil(resolutionX / 32.0f), (uint32_t)std::ceil(resolutionY / 32.0f), 1);
			}
		}

		ID3D11Buffer* buffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &buffer);

		ID3D11ShaderResourceView* views[3]{ nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, 3, views);

		ID3D11UnorderedAccessView* uavs[2]{ nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);
	}
}

void SubsurfaceScattering::ClearShaderCache()
{
	if (horizontalSSBlur) {
		horizontalSSBlur->Release();
		horizontalSSBlur = nullptr;
	}
	if (verticalSSBlur) {
		verticalSSBlur->Release();
		verticalSSBlur = nullptr;
	}
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderHorizontalBlur()
{
	if (!horizontalSSBlur) {
		logger::debug("Compiling horizontalSSBlur");
		horizontalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", { { "HORIZONTAL", "" } }, "cs_5_0");
	}
	return horizontalSSBlur;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderVerticalBlur()
{
	if (!verticalSSBlur) {
		logger::debug("Compiling verticalSSBlur");
		verticalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", {}, "cs_5_0");
	}
	return verticalSSBlur;
}

void SubsurfaceScattering::BSLightingShader_SetupGeometry_Before(RE::BSRenderPass*)
{
	//skinMode = 0;

	//auto geometry = Pass->geometry;
	//if (auto userData = geometry->GetUserData()) {
	//	if (auto actor = userData->As<RE::Actor>()) {
	//		if (auto race = actor->GetRace()) {
	//			if (Pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kFace, RE::BSShaderProperty::EShaderPropertyFlag::kFaceGenRGBTint)) {
	//				static auto isBeastRace = RE::TESForm::LookupByEditorID("IsBeastRace")->As<RE::BGSKeyword>();
	//				skinMode = race->HasKeyword(isBeastRace) ? 1 : 2;
	//			} else if (Pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kCharacterLighting, RE::BSShaderProperty::EShaderPropertyFlag::kSoftLighting)) {
	//				auto lightingMaterial = (RE::BSLightingShaderMaterialBase*)(Pass->shaderProperty->material);
	//				if (auto diffuse = lightingMaterial->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
	//					if (diffuse != nullptr) {
	//						std::string diffuseStr = diffuse;
	//						if (diffuseStr.contains("MaleChild") || diffuseStr.contains("FemaleChild")) {
	//							static auto isBeastRace = RE::TESForm::LookupByEditorID("IsBeastRace")->As<RE::BGSKeyword>();
	//							skinMode = race->HasKeyword(isBeastRace) ? 1 : 2;
	//						}
	//					}
	//				}
	//			}
	//		}
	//	}
	//}
}