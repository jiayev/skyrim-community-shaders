#include "ScreenSpaceShadows.h"

#include "State.h"
#include "Util.h"

using RE::RENDER_TARGETS;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::Settings,
	MaxSamples,
	FarDistanceScale,
	FarThicknessScale,
	FarHardness,
	NearDistance,
	NearThickness,
	NearHardness,
	BlurRadius,
	BlurDropoff)

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Screen-Space Shadows", &enabled);

		ImGui::TextWrapped("Controls the accuracy of traced shadows.");
		ImGui::SliderInt("Max Samples", (int*)&settings.MaxSamples, 1, 512);

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Blur Filter", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Blur Radius", &settings.BlurRadius, 0, 1);
		ImGui::SliderFloat("Blur Depth Dropoff", &settings.BlurDropoff, 0.001f, 0.1f);

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Near Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Near Distance", &settings.NearDistance, 0, 128);
		ImGui::SliderFloat("Near Thickness", &settings.NearThickness, 0, 128);
		ImGui::SliderFloat("Near Hardness", &settings.NearHardness, 0, 64);

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Far Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Far Distance Scale", &settings.FarDistanceScale, 0, 1);
		ImGui::SliderFloat("Far Thickness Scale", &settings.FarThicknessScale, 0, 1);
		ImGui::SliderFloat("Far Hardness", &settings.FarHardness, 0, 64);

		ImGui::TreePop();
	}
}

enum class GrassShaderTechniques
{
	RenderDepth = 8,
};

void ScreenSpaceShadows::ModifyGrass(const RE::BSShader*, const uint32_t descriptor)
{
	const auto technique = descriptor & 0b1111;
	if (technique != static_cast<uint32_t>(GrassShaderTechniques::RenderDepth)) {
		ModifyLighting(nullptr, 0);
	}
}

enum class DistantTreeShaderTechniques
{
	DistantTreeBlock = 0,
	Depth = 1,
};

void ScreenSpaceShadows::ModifyDistantTree(const RE::BSShader*, const uint32_t descriptor)
{
	const auto technique = descriptor & 1;
	if (technique != static_cast<uint32_t>(DistantTreeShaderTechniques::Depth)) {
		ModifyLighting(nullptr, 0);
	}
}

enum class LightingShaderTechniques
{
	None = 0,
	Envmap = 1,
	Glowmap = 2,
	Parallax = 3,
	Facegen = 4,
	FacegenRGBTint = 5,
	Hair = 6,
	ParallaxOcc = 7,
	MTLand = 8,
	LODLand = 9,
	Snow = 10,  // unused
	MultilayerParallax = 11,
	TreeAnim = 12,
	LODObjects = 13,
	MultiIndexSparkle = 14,
	LODObjectHD = 15,
	Eye = 16,
	Cloud = 17,  // unused
	LODLandNoise = 18,
	MTLandLODBlend = 19,
	Outline = 20,
};

uint32_t GetTechnique(uint32_t descriptor)
{
	return 0x3F & (descriptor >> 24);
}

void ScreenSpaceShadows::ClearComputeShader()
{
	if (raymarchProgram) {
		raymarchProgram->Release();
		raymarchProgram = nullptr;
	}
	if (horizontalBlurProgram) {
		horizontalBlurProgram->Release();
		horizontalBlurProgram = nullptr;
	}
	if (verticalBlurProgram) {
		verticalBlurProgram->Release();
		verticalBlurProgram = nullptr;
	}
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeShader()
{
	if (!raymarchProgram) {
		logger::debug("Compiling raymarchProgram");
		raymarchProgram = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", {}, "cs_5_0");
	}
	return raymarchProgram;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeShaderHorizontalBlur()
{
	if (!horizontalBlurProgram) {
		logger::debug("Compiling horizontalBlurProgram");
		horizontalBlurProgram = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\FilterCS.hlsl", { { "HORIZONTAL", "" } }, "cs_5_0");
	}
	return horizontalBlurProgram;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeShaderVerticalBlur()
{
	if (!verticalBlurProgram) {
		verticalBlurProgram = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\FilterCS.hlsl", { { "VERTICAL", "" } }, "cs_5_0");
		logger::debug("Compiling verticalBlurProgram");
	}
	return verticalBlurProgram;
}

void ScreenSpaceShadows::ModifyLighting(const RE::BSShader*, const uint32_t)
{
	if (!loaded)
		return;

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	bool skyLight = true;
	if (auto sky = RE::Sky::GetSingleton())
		skyLight = sky->mode.get() == RE::Sky::Mode::kFull;

	if (dirLight && skyLight) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();

		if (!screenSpaceShadowsTexture) {
			{
				logger::debug("Creating screenSpaceShadowsTexture");

				auto device = renderer->GetRuntimeData().forwarder;

				D3D11_SAMPLER_DESC samplerDesc = {};
				samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
				samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
				samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
				samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
				samplerDesc.MaxAnisotropy = 1;
				samplerDesc.MinLOD = 0;
				samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
				DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &computeSampler));
			}

			{
				auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK];

				D3D11_TEXTURE2D_DESC texDesc{};
				shadowMask.texture->GetDesc(&texDesc);
				texDesc.Format = DXGI_FORMAT_R16_FLOAT;
				texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
				screenSpaceShadowsTexture = new Texture2D(texDesc);

				texDesc.Width /= 2;
				texDesc.Height /= 2;
				screenSpaceShadowsTextureTemp = new Texture2D(texDesc);

				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				shadowMask.SRV->GetDesc(&srvDesc);
				srvDesc.Format = texDesc.Format;
				screenSpaceShadowsTexture->CreateSRV(srvDesc);
				screenSpaceShadowsTextureTemp->CreateSRV(srvDesc);

				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
				uavDesc.Format = texDesc.Format;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				uavDesc.Texture2D.MipSlice = 0;
				screenSpaceShadowsTexture->CreateUAV(uavDesc);
				screenSpaceShadowsTextureTemp->CreateUAV(uavDesc);
			}
		}

		auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();

		bool enableSSS = true;

		if (shadowState->GetRuntimeData().cubeMapRenderTarget == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS) {
			enableSSS = false;

		} else if (!renderedScreenCamera && enabled) {
			renderedScreenCamera = true;

			// Backup the game state
			struct OldState
			{
				ID3D11ShaderResourceView* srvs[2];
				ID3D11SamplerState* sampler;
				ID3D11ComputeShader* shader;
				ID3D11Buffer* buffer;
				ID3D11UnorderedAccessView* uav;
				ID3D11ClassInstance* instance;
				UINT numInstances;
			};

			OldState old{};
			context->CSGetShaderResources(0, 2, old.srvs);
			context->CSGetSamplers(0, 1, &old.sampler);
			context->CSGetShader(&old.shader, &old.instance, &old.numInstances);
			context->CSGetConstantBuffers(0, 1, &old.buffer);
			context->CSGetUnorderedAccessViews(0, 1, &old.uav);

			{
				auto viewport = RE::BSGraphics::State::GetSingleton();

				float resolutionX = screenSpaceShadowsTexture->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
				float resolutionY = screenSpaceShadowsTexture->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

				{
					RaymarchCB data{};

					data.BufferDim.x = (float)screenSpaceShadowsTexture->desc.Width;
					data.BufferDim.y = (float)screenSpaceShadowsTexture->desc.Height;

					data.RcpBufferDim.x = 1.0f / data.BufferDim.x;
					data.RcpBufferDim.y = 1.0f / data.BufferDim.y;
					if (REL::Module::IsVR())
						data.ProjMatrix = shadowState->GetVRRuntimeData().cameraData.getEye().projMat;
					else
						data.ProjMatrix = shadowState->GetRuntimeData().cameraData.getEye().projMat;

					data.InvProjMatrix = XMMatrixInverse(nullptr, data.ProjMatrix);

					data.DynamicRes.x = viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
					data.DynamicRes.y = viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

					data.DynamicRes.z = 1.0f / data.DynamicRes.x;
					data.DynamicRes.w = 1.0f / data.DynamicRes.y;

					auto& direction = dirLight->GetWorldDirection();
					DirectX::XMFLOAT3 position{};
					position.x = -direction.x;
					position.y = -direction.y;
					position.z = -direction.z;
					auto viewMatrix = shadowState->GetRuntimeData().cameraData.getEye().viewMat;
					if (REL::Module::IsVR())
						viewMatrix = shadowState->GetVRRuntimeData().cameraData.getEye().viewMat;

					auto invDirLightDirectionWS = XMLoadFloat3(&position);
					data.InvDirLightDirectionVS = XMVector3TransformCoord(invDirLightDirectionWS, viewMatrix);

					data.ShadowDistance = 10000.0f;

					data.Settings = settings;

					raymarchCB->Update(data);
				}

				ID3D11Buffer* buffer[1] = { raymarchCB->CB() };
				context->CSSetConstantBuffers(0, 1, buffer);

				context->CSSetSamplers(0, 1, &computeSampler);

				auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

				ID3D11ShaderResourceView* view = depth.depthSRV;
				context->CSSetShaderResources(0, 1, &view);

				ID3D11UnorderedAccessView* uav = screenSpaceShadowsTexture->uav.get();
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

				auto shader = GetComputeShader();
				context->CSSetShader(shader, nullptr, 0);

				context->Dispatch((uint32_t)std::ceil(resolutionX / 32.0f), (uint32_t)std::ceil(resolutionY / 32.0f), 1);

				// Filter
				{
					uav = nullptr;
					context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
					view = nullptr;
					context->CSSetShaderResources(1, 1, &view);

					view = screenSpaceShadowsTexture->srv.get();

					context->CSSetShaderResources(1, 1, &view);

					uav = screenSpaceShadowsTextureTemp->uav.get();
					context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

					shader = GetComputeShaderHorizontalBlur();
					context->CSSetShader(shader, nullptr, 0);

					context->Dispatch((uint32_t)std::ceil(resolutionX / 64.0f), (uint32_t)std::ceil(resolutionY / 64.0f), 1);
				}

				{
					uav = nullptr;
					context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
					view = nullptr;
					context->CSSetShaderResources(1, 1, &view);

					view = screenSpaceShadowsTextureTemp->srv.get();

					context->CSSetShaderResources(1, 1, &view);

					uav = screenSpaceShadowsTexture->uav.get();
					context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

					shader = GetComputeShaderVerticalBlur();
					context->CSSetShader(shader, nullptr, 0);

					context->Dispatch((uint32_t)std::ceil(resolutionX / 64.0f), (uint32_t)std::ceil(resolutionY / 64.0f), 1);
				}
			}

			// Restore the game state
			context->CSSetShaderResources(0, 2, old.srvs);
			for (uint8_t i = 0; i < 2; i++)
				if (old.srvs[i])
					old.srvs[i]->Release();

			context->CSSetSamplers(0, 1, &old.sampler);
			if (old.sampler)
				old.sampler->Release();

			context->CSSetShader(old.shader, &old.instance, old.numInstances);
			if (old.shader)
				old.shader->Release();

			context->CSSetConstantBuffers(0, 1, &old.buffer);
			if (old.buffer)
				old.buffer->Release();

			context->CSSetUnorderedAccessViews(0, 1, &old.uav, nullptr);
			if (old.uav)
				old.uav->Release();
		}

		PerPass data{};
		data.EnableSSS = enableSSS && shadowState->GetRuntimeData().rasterStateCullMode <= 1 && enabled;
		perPass->Update(data);

		if (renderedScreenCamera) {
			auto shadowMask = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
			ID3D11ShaderResourceView* views[2]{};
			views[0] = shadowMask.depthSRV;
			views[1] = screenSpaceShadowsTexture->srv.get();
			context->PSSetShaderResources(20, ARRAYSIZE(views), views);
		}
	} else {
		PerPass data{};
		data.EnableSSS = false;
		perPass->Update(data);
	}

	ID3D11Buffer* buffers[1]{};
	buffers[0] = perPass->CB();
	context->PSSetConstantBuffers(5, ARRAYSIZE(buffers), buffers);

	context->PSSetSamplers(14, 1, &computeSampler);
}

void ScreenSpaceShadows::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Grass:
		ModifyGrass(shader, descriptor);
		break;
	case RE::BSShader::Type::DistantTree:
		ModifyDistantTree(shader, descriptor);
		break;
	case RE::BSShader::Type::Lighting:
		ModifyLighting(shader, descriptor);
		break;
	}
}

void ScreenSpaceShadows::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void ScreenSpaceShadows::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void ScreenSpaceShadows::SetupResources()
{
	perPass = new ConstantBuffer(ConstantBufferDesc<PerPass>());
	raymarchCB = new ConstantBuffer(ConstantBufferDesc<RaymarchCB>());
}

void ScreenSpaceShadows::Reset()
{
	renderedScreenCamera = false;
}