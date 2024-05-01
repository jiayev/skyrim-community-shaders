#include "Deferred.h"
#include "State.h"
#include "Util.h"
#include <Features/CloudShadows.h>
#include <Features/ScreenSpaceGI.h>
#include <Features/ScreenSpaceShadows.h>
#include <Features/SubsurfaceScattering.h>
#include <Features/TerrainOcclusion.h>
#include <ShaderCache.h>
#include <VariableRateShading.h>

void Deferred::DepthStencilStateSetDepthMode(RE::BSGraphics::DepthStencilDepthMode a_mode)
{
	auto& state = State::GetSingleton()->shadowState;
	GET_INSTANCE_MEMBER(depthStencilDepthMode, state)
	GET_INSTANCE_MEMBER(depthStencilDepthModePrevious, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)

	if (depthStencilDepthMode != a_mode) {
		depthStencilDepthMode = a_mode;
		if (depthStencilDepthModePrevious != a_mode)
			stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
		else
			stateUpdateFlags.reset(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
	}
}

void Deferred::AlphaBlendStateSetMode(uint32_t a_mode)
{
	auto& state = State::GetSingleton()->shadowState;
	GET_INSTANCE_MEMBER(alphaBlendMode, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)

	if (alphaBlendMode != a_mode) {
		alphaBlendMode = a_mode;
		stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	}
}

void Deferred::AlphaBlendStateSetAlphaToCoverage(uint32_t a_value)
{
	auto& state = State::GetSingleton()->shadowState;
	GET_INSTANCE_MEMBER(alphaBlendAlphaToCoverage, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)

	if (alphaBlendAlphaToCoverage != a_value) {
		alphaBlendAlphaToCoverage = a_value;
		stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	}
}

void Deferred::AlphaBlendStateSetWriteMode(uint32_t a_value)
{
	auto& state = State::GetSingleton()->shadowState;
	GET_INSTANCE_MEMBER(alphaBlendWriteMode, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)

	if (alphaBlendWriteMode != a_value) {
		alphaBlendWriteMode = a_value;
		stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	}
}

struct DepthStates
{
	ID3D11DepthStencilState* a[6][40];
};

struct BlendStates
{
	ID3D11BlendState* a[7][2][13][2];
};

void SetupRenderTarget(RE::RENDER_TARGET target, D3D11_TEXTURE2D_DESC texDesc, D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc, D3D11_RENDER_TARGET_VIEW_DESC rtvDesc, D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc, DXGI_FORMAT format)
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& device = State::GetSingleton()->device;

	texDesc.Format = format;
	srvDesc.Format = format;
	rtvDesc.Format = format;
	uavDesc.Format = format;

	auto& data = renderer->GetRuntimeData().renderTargets[target];
	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, &data.texture));
	DX::ThrowIfFailed(device->CreateShaderResourceView(data.texture, &srvDesc, &data.SRV));
	DX::ThrowIfFailed(device->CreateRenderTargetView(data.texture, &rtvDesc, &data.RTV));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(data.texture, &uavDesc, &data.UAV));
}

void Deferred::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	{
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.RTV->GetDesc(&rtvDesc);
		main.UAV->GetDesc(&uavDesc);

		// Available targets:
		// MAIN ONLY ALPHA
		// WATER REFLECTIONS
		// BLURFULL_BUFFER
		// LENSFLAREVIS
		// SAO DOWNSCALED
		// SAO CAMERAZ+MIP_LEVEL_0_ESRAM
		// SAO_RAWAO_DOWNSCALED
		// SAO_RAWAO_PREVIOUS_DOWNSCALDE
		// SAO_TEMP_BLUR_DOWNSCALED
		// INDIRECT
		// INDIRECT_DOWNSCALED
		// RAWINDIRECT
		// RAWINDIRECT_DOWNSCALED
		// RAWINDIRECT_PREVIOUS
		// RAWINDIRECT_PREVIOUS_DOWNSCALED
		// RAWINDIRECT_SWAP
		// VOLUMETRIC_LIGHTING_HALF_RES
		// VOLUMETRIC_LIGHTING_BLUR_HALF_RES
		// VOLUMETRIC_LIGHTING_QUARTER_RES
		// VOLUMETRIC_LIGHTING_BLUR_QUARTER_RES
		// TEMPORAL_AA_WATER_1
		// TEMPORAL_AA_WATER_2

		// Albedo
		SetupRenderTarget(ALBEDO, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
		// Specular
		SetupRenderTarget(SPECULAR, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R11G11B10_FLOAT);
		// Reflectance
		SetupRenderTarget(REFLECTANCE, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
		// Normal + Roughness
		SetupRenderTarget(NORMALROUGHNESS, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
		// Masks
		SetupRenderTarget(MASKS, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
	}

	{
		deferredCB = new ConstantBuffer(ConstantBufferDesc<DeferredCB>());
	}

	{
		auto& device = State::GetSingleton()->device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &linearSampler));
	}

	{
		D3D11_TEXTURE2D_DESC texDesc;
		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);

		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		{
			giTexture = new Texture2D(texDesc);
			giTexture->CreateSRV(srvDesc);
			giTexture->CreateRTV(rtvDesc);
			giTexture->CreateUAV(uavDesc);
		}
	}
}

void Deferred::Reset()
{
	//for (auto& str : perms)
	//{
	////	logger::info("{}", str);
	//}
}

void Deferred::UpdateConstantBuffer()
{
	auto state = State::GetSingleton();
	auto viewport = RE::BSGraphics::State::GetSingleton();

	DeferredCB data{};

	auto& shadowState = State::GetSingleton()->shadowState;

	if (REL::Module::IsVR()) {
		auto posAdjust = shadowState->GetVRRuntimeData().posAdjust.getEye(0);
		data.CamPosAdjust[0] = { posAdjust.x, posAdjust.y, posAdjust.z, 0 };
		posAdjust = shadowState->GetVRRuntimeData().posAdjust.getEye(1);
		data.CamPosAdjust[1] = { posAdjust.x, posAdjust.y, posAdjust.z, 0 };

		data.ViewMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(0).viewMat;
		data.ViewMatrix[1] = shadowState->GetVRRuntimeData().cameraData.getEye(1).viewMat;
		data.ProjMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(0).projMat;
		data.ProjMatrix[1] = shadowState->GetVRRuntimeData().cameraData.getEye(1).projMat;
		data.ViewProjMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(0).viewProjMat;
		data.ViewProjMatrix[1] = shadowState->GetVRRuntimeData().cameraData.getEye(1).viewProjMat;
		data.InvViewMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(0).viewMat.Invert();
		data.InvViewMatrix[1] = shadowState->GetVRRuntimeData().cameraData.getEye(1).viewMat.Invert();
		data.InvProjMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(0).projMat.Invert();
		data.InvProjMatrix[1] = shadowState->GetVRRuntimeData().cameraData.getEye(1).projMat.Invert();
		data.InvViewProjMatrix[0] = data.InvViewMatrix[0] * data.InvProjMatrix[0];
		data.InvViewProjMatrix[1] = data.InvViewMatrix[1] * data.InvProjMatrix[1];
	} else {
		auto posAdjust = shadowState->GetRuntimeData().posAdjust.getEye(0);
		data.CamPosAdjust[0] = { posAdjust.x, posAdjust.y, posAdjust.z, 0 };
		data.ViewMatrix[0] = shadowState->GetRuntimeData().cameraData.getEye(0).viewMat;
		data.ProjMatrix[0] = shadowState->GetRuntimeData().cameraData.getEye(0).projMat;
		data.ViewProjMatrix[0] = shadowState->GetRuntimeData().cameraData.getEye(0).viewProjMat;
		data.InvViewMatrix[0] = shadowState->GetRuntimeData().cameraData.getEye(0).viewMat.Invert();
		data.InvProjMatrix[0] = shadowState->GetRuntimeData().cameraData.getEye(0).projMat.Invert();
		data.InvViewProjMatrix[0] = data.InvViewMatrix[0] * data.InvProjMatrix[0];
	}

	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	data.DirLightColor = { dirLight->GetLightRuntimeData().diffuse.red, dirLight->GetLightRuntimeData().diffuse.green, dirLight->GetLightRuntimeData().diffuse.blue, 1.0f };

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	data.DirLightColor *= !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().data.baseData.hdr.sunlightScale : imageSpaceManager->GetVRRuntimeData().data.baseData.hdr.sunlightScale;

	auto& direction = dirLight->GetWorldDirection();
	float4 position{ -direction.x, -direction.y, -direction.z, 0.0f };

	data.DirLightDirectionVS[0] = float4::Transform(position, data.ViewMatrix[0]);
	data.DirLightDirectionVS[0].Normalize();

	data.DirLightDirectionVS[1] = float4::Transform(position, data.ViewMatrix[1]);
	data.DirLightDirectionVS[1].Normalize();

	auto& shaderManager = RE::BSShaderManager::State::GetSingleton();
	RE::NiTransform& dalcTransform = shaderManager.directionalAmbientTransform;
	Util::StoreTransform3x4NoScale(data.DirectionalAmbient, dalcTransform);

	data.BufferDim.x = state->screenWidth;
	data.BufferDim.y = state->screenHeight;

	data.RcpBufferDim.x = 1.0f / State::GetSingleton()->screenWidth;
	data.RcpBufferDim.y = 1.0f / State::GetSingleton()->screenHeight;

	auto useTAA = !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled : imageSpaceManager->GetVRRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled;
	data.FrameCount = useTAA ? viewport->uiFrameCount : 0;

	data.CameraData = Util::GetCameraData();

	deferredCB->Update(data);
}

void Deferred::StartDeferred()
{
	if (!inWorld)
		return;

	auto& shaderCache = SIE::ShaderCache::Instance();

	if (!shaderCache.IsEnabled())
		return;

	static bool setup = false;
	if (!setup) {
		auto& device = State::GetSingleton()->device;

		static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

		{
			forwardBlendStates[0] = blendStates->a[0][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[0]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[0]));
		}

		{
			forwardBlendStates[1] = blendStates->a[0][0][10][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[1]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[1]));
		}

		{
			forwardBlendStates[2] = blendStates->a[1][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[2]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[2]));
		}

		{
			forwardBlendStates[3] = blendStates->a[1][0][11][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[3]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[3]));
		}

		{
			forwardBlendStates[4] = blendStates->a[2][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[4]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[4]));
		}

		{
			forwardBlendStates[5] = blendStates->a[2][0][11][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[5]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[5]));
		}

		{
			forwardBlendStates[6] = blendStates->a[3][0][11][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[6]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[6]));
		}
		setup = true;
	}

	auto& state = State::GetSingleton()->shadowState;
	GET_INSTANCE_MEMBER(renderTargets, state)
	GET_INSTANCE_MEMBER(setRenderTargetMode, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)

	// Backup original render targets
	for (uint i = 0; i < 4; i++) {
		forwardRenderTargets[i] = renderTargets[i];
	}

	RE::RENDER_TARGET targets[8]{
		RE::RENDER_TARGET::kMAIN,
		RE::RENDER_TARGET::kMOTION_VECTOR,
		NORMALROUGHNESS,
		ALBEDO,
		SPECULAR,
		REFLECTANCE,
		MASKS,
		forwardRenderTargets[3]  // Improved snow shader
	};

	for (uint i = 2; i < 8; i++) {
		renderTargets[i] = targets[i];                                             // We must use unused targets to be indexable
		setRenderTargetMode[i] = RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR;  // Dirty from last frame, this calls ClearRenderTargetView once
	}

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

	// Set modified blend states
	blendStates->a[0][0][1][0] = deferredBlendStates[0];
	blendStates->a[0][0][10][0] = deferredBlendStates[1];
	blendStates->a[1][0][1][0] = deferredBlendStates[2];
	blendStates->a[1][0][11][0] = deferredBlendStates[3];
	blendStates->a[2][0][1][0] = deferredBlendStates[4];
	blendStates->a[2][0][11][0] = deferredBlendStates[5];
	blendStates->a[3][0][11][0] = deferredBlendStates[6];

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

	deferredPass = true;
}

void Deferred::DeferredPasses()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& context = State::GetSingleton()->context;
	auto state = State::GetSingleton();
	auto viewport = RE::BSGraphics::State::GetSingleton();

	UpdateConstantBuffer();

	{
		auto buffer = deferredCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);
	}

	{
		FLOAT clr[4] = { 0., 0., 0., 1. };
		context->ClearUnorderedAccessViewFloat(giTexture->uav.get(), clr);
	}

	auto specular = renderer->GetRuntimeData().renderTargets[SPECULAR];
	auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
	auto reflectance = renderer->GetRuntimeData().renderTargets[REFLECTANCE];
	auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	auto masks = renderer->GetRuntimeData().renderTargets[MASKS];

	auto main = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[0]];
	auto normals = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[2]];

	// Only render directional shadows if the game has a directional shadow caster
	auto shadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
	auto shadowDirLight = (RE::BSShadowLight*)shadowSceneNode->GetRuntimeData().shadowDirLight;
	bool dirShadow = shadowDirLight && shadowDirLight->shadowLightIndex == 0;

	if (dirShadow) {
		if (ScreenSpaceShadows::GetSingleton()->loaded) {
			ScreenSpaceShadows::GetSingleton()->DrawShadows();
		}

		if (TerrainOcclusion::GetSingleton()->loaded) {
			TerrainOcclusion::GetSingleton()->DrawTerrainOcclusion();
		}

		if (CloudShadows::GetSingleton()->loaded) {
			CloudShadows::GetSingleton()->DrawShadows();
		}
	}

	{
		ID3D11ShaderResourceView* srvs[7]{
			specular.SRV,
			albedo.SRV,
			reflectance.SRV,
			normalRoughness.SRV,
			shadowMask.SRV,
			depth.depthSRV,
			masks.SRV
		};

		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[2]{ main.UAV, normals.UAV };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetSamplers(0, 1, &linearSampler);

		auto shader = dirShadow ? GetComputeDirectionalShadow() : GetComputeDirectional();
		context->CSSetShader(shader, nullptr, 0);

		float resolutionX = state->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		float resolutionY = state->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

		uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 32.0f);
		uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 32.0f);

		context->Dispatch(dispatchX, dispatchY, 1);
	}

	// Features that require full diffuse lighting should be put here

	if (ScreenSpaceGI::GetSingleton()->loaded) {
		ScreenSpaceGI::GetSingleton()->DrawSSGI(giTexture);
	}

	{
		{
			ID3D11ShaderResourceView* srvs[8]{
				specular.SRV,
				albedo.SRV,
				reflectance.SRV,
				normalRoughness.SRV,
				shadowMask.SRV,
				depth.depthSRV,
				masks.SRV,
				giTexture->srv.get(),
			};

			context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

			ID3D11UnorderedAccessView* uavs[2]{ main.UAV, normals.UAV };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetSamplers(0, 1, &linearSampler);

			auto shader = GetComputeAmbientComposite();
			context->CSSetShader(shader, nullptr, 0);

			float resolutionX = state->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
			float resolutionY = state->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

			uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 32.0f);
			uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 32.0f);

			context->Dispatch(dispatchX, dispatchY, 1);
		}
	}

	if (SubsurfaceScattering::GetSingleton()->loaded) {
		SubsurfaceScattering::GetSingleton()->DrawSSSWrapper(false);
	}

	{
		{
			ID3D11ShaderResourceView* srvs[8]{
				specular.SRV,
				albedo.SRV,
				reflectance.SRV,
				normalRoughness.SRV,
				shadowMask.SRV,
				depth.depthSRV,
				masks.SRV,
				giTexture->srv.get(),
			};

			context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

			ID3D11UnorderedAccessView* uavs[2]{ main.UAV, normals.UAV };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetSamplers(0, 1, &linearSampler);

			auto shader = GetComputeMainComposite();
			context->CSSetShader(shader, nullptr, 0);

			float resolutionX = state->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
			float resolutionY = state->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

			uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 32.0f);
			uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 32.0f);

			context->Dispatch(dispatchX, dispatchY, 1);
		}
	}

	ID3D11ShaderResourceView* views[8]{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[2]{ nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	ID3D11Buffer* buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->CSSetShader(nullptr, nullptr, 0);
}

void Deferred::EndDeferred()
{
	if (!inWorld)
		return;

	inWorld = false;

	auto& shaderCache = SIE::ShaderCache::Instance();

	if (!shaderCache.IsEnabled())
		return;

	auto& state = State::GetSingleton()->shadowState;
	GET_INSTANCE_MEMBER(renderTargets, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)

	// Do not render to our targets past this point
	for (uint i = 0; i < 4; i++) {
		renderTargets[i] = forwardRenderTargets[i];
	}

	for (uint i = 4; i < 8; i++) {
		state->GetRuntimeData().renderTargets[i] = RE::RENDER_TARGET::kNONE;
	}

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	DeferredPasses();  // Perform deferred passes and composite forward buffers

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

	// Restore modified blend states
	blendStates->a[0][0][1][0] = forwardBlendStates[0];
	blendStates->a[0][0][10][0] = forwardBlendStates[1];
	blendStates->a[1][0][1][0] = forwardBlendStates[2];
	blendStates->a[1][0][11][0] = forwardBlendStates[3];
	blendStates->a[2][0][1][0] = forwardBlendStates[4];
	blendStates->a[2][0][11][0] = forwardBlendStates[5];
	blendStates->a[3][0][11][0] = forwardBlendStates[6];

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

	deferredPass = false;
}

void Deferred::UpdatePerms()
{
	if (deferredPass) {
		auto& state = State::GetSingleton()->shadowState;
		GET_INSTANCE_MEMBER(alphaBlendMode, state)
		GET_INSTANCE_MEMBER(alphaBlendAlphaToCoverage, state)
		GET_INSTANCE_MEMBER(alphaBlendWriteMode, state)
		GET_INSTANCE_MEMBER(alphaBlendModeExtra, state)

		std::string comboStr = std::format("{} {} {} {}", alphaBlendMode, alphaBlendAlphaToCoverage, alphaBlendWriteMode, alphaBlendModeExtra);

		perms.insert(comboStr);
	}
}

void Deferred::ClearShaderCache()
{
	if (directionalShadowCS) {
		directionalShadowCS->Release();
		directionalShadowCS = nullptr;
	}
	if (directionalCS) {
		directionalCS->Release();
		directionalCS = nullptr;
	}
	if (ambientCompositeCS) {
		ambientCompositeCS->Release();
		ambientCompositeCS = nullptr;
	}
	if (mainCompositeCS) {
		mainCompositeCS->Release();
		mainCompositeCS = nullptr;
	}
}

ID3D11ComputeShader* Deferred::GetComputeDirectionalShadow()
{
	if (!directionalShadowCS) {
		logger::debug("Compiling DeferredCompositeCS DirectionalShadowPass");
		directionalShadowCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DeferredCompositeCS.hlsl", {}, "cs_5_0", "DirectionalShadowPass");
	}
	return directionalShadowCS;
}

ID3D11ComputeShader* Deferred::GetComputeDirectional()
{
	if (!directionalCS) {
		logger::debug("Compiling DeferredCompositeCS DirectionalPass");
		directionalCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DeferredCompositeCS.hlsl", {}, "cs_5_0", "DirectionalPass");
	}
	return directionalCS;
}

ID3D11ComputeShader* Deferred::GetComputeAmbientComposite()
{
	if (!ambientCompositeCS) {
		logger::debug("Compiling DeferredCompositeCS AmbientCompositePass");
		ambientCompositeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DeferredCompositeCS.hlsl", {}, "cs_5_0", "AmbientCompositePass");
	}
	return ambientCompositeCS;
}

ID3D11ComputeShader* Deferred::GetComputeMainComposite()
{
	if (!mainCompositeCS) {
		logger::debug("Compiling DeferredCompositeCS MainCompositePass");
		mainCompositeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DeferredCompositeCS.hlsl", {}, "cs_5_0", "MainCompositePass");
	}
	return mainCompositeCS;
}