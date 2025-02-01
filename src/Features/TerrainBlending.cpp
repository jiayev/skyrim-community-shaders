#include "TerrainBlending.h"

#include "State.h"
#include "Util.h"

#include "VariableCache.h"

ID3D11VertexShader* TerrainBlending::GetTerrainVertexShader()
{
	if (!terrainVertexShader) {
		logger::debug("Compiling Utility.hlsl");
		terrainVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", { { "RENDER_DEPTH", "" } }, "vs_5_0");
	}
	return terrainVertexShader;
}

ID3D11VertexShader* TerrainBlending::GetTerrainOffsetVertexShader()
{
	if (!terrainOffsetVertexShader) {
		logger::debug("Compiling Utility.hlsl");
		terrainOffsetVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", { { "RENDER_DEPTH", "" }, { "OFFSET_DEPTH", "" } }, "vs_5_0");
	}
	return terrainOffsetVertexShader;
}

ID3D11ComputeShader* TerrainBlending::GetDepthBlendShader()
{
	if (!depthBlendShader) {
		logger::debug("Compiling DepthBlend.hlsl");
		depthBlendShader = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\TerrainBlending\\DepthBlend.hlsl", {}, "cs_5_0");
	}
	return depthBlendShader;
}

void TerrainBlending::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& device = State::GetSingleton()->device;

	{
		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc;
		mainDepth.texture->GetDesc(&texDesc);
		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, NULL, &terrainDepth.texture));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		mainDepth.depthSRV->GetDesc(&srvDesc);
		DX::ThrowIfFailed(device->CreateShaderResourceView(terrainDepth.texture, &srvDesc, &terrainDepth.depthSRV));

		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		mainDepth.views[0]->GetDesc(&dsvDesc);
		DX::ThrowIfFailed(device->CreateDepthStencilView(terrainDepth.texture, &dsvDesc, &terrainDepth.views[0]));
	}

	{
		auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		blendedDepthTexture = new Texture2D(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		main.SRV->GetDesc(&srvDesc);
		srvDesc.Format = texDesc.Format;
		blendedDepthTexture->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		main.UAV->GetDesc(&uavDesc);
		uavDesc.Format = texDesc.Format;
		blendedDepthTexture->CreateUAV(uavDesc);

		texDesc.Format = DXGI_FORMAT_R16_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		blendedDepthTexture16 = new Texture2D(texDesc);
		blendedDepthTexture16->CreateSRV(srvDesc);
		blendedDepthTexture16->CreateUAV(uavDesc);

		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		depthSRVBackup = mainDepth.depthSRV;

		auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		prepassSRVBackup = zPrepassCopy.depthSRV;
	}
}

void TerrainBlending::PostPostLoad()
{
	Hooks::Install();
}

struct DepthStates
{
	ID3D11DepthStencilState* a[6][40];
};

struct BlendStates
{
	ID3D11BlendState* a[7][2][13][2];
};

void TerrainBlending::TerrainShaderHacks()
{
	if (renderTerrainDepth) {
		auto renderer = VariableCache::GetSingleton()->renderer;
		auto context = VariableCache::GetSingleton()->context;
		if (renderAltTerrain) {
			auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];
			context->OMSetRenderTargets(0, nullptr, dsv);
			context->VSSetShader(GetTerrainOffsetVertexShader(), NULL, NULL);
		} else {
			auto dsv = terrainDepth.views[0];
			context->OMSetRenderTargets(0, nullptr, dsv);
			auto shadowState = VariableCache::GetSingleton()->shadowState;
			GET_INSTANCE_MEMBER(currentVertexShader, shadowState)
			context->VSSetShader((ID3D11VertexShader*)currentVertexShader->shader, NULL, NULL);
		}
		renderAltTerrain = !renderAltTerrain;
	}
}

void TerrainBlending::ResetDepth()
{
	auto context = VariableCache::GetSingleton()->context;

	auto dsv = terrainDepth.views[0];
	context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0u);
}

void TerrainBlending::ResetTerrainDepth()
{
	auto context = VariableCache::GetSingleton()->context;

	auto stateUpdateFlags = VariableCache::GetSingleton()->stateUpdateFlags;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	auto currentVertexShader = *VariableCache::GetSingleton()->currentVertexShader;
	context->VSSetShader((ID3D11VertexShader*)currentVertexShader->shader, NULL, NULL);
}

void TerrainBlending::BlendPrepassDepths()
{
	auto& context = VariableCache::GetSingleton()->context;
	context->OMSetRenderTargets(0, nullptr, nullptr);

	auto dispatchCount = Util::GetScreenDispatchCount();

	{
		ID3D11ShaderResourceView* views[2] = { depthSRVBackup, terrainDepth.depthSRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[2] = { blendedDepthTexture->uav.get(), blendedDepthTexture16->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(GetDepthBlendShader(), nullptr, 0);

		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
	}

	ID3D11ShaderResourceView* views[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);

	auto stateUpdateFlags = VariableCache::GetSingleton()->stateUpdateFlags;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	auto renderer = VariableCache::GetSingleton()->renderer;
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	context->CopyResource(terrainDepth.texture, mainDepth.texture);
}

void TerrainBlending::ResetTerrainWorld()
{
	auto stateUpdateFlags = VariableCache::GetSingleton()->stateUpdateFlags;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
}

void TerrainBlending::ClearShaderCache()
{
	if (terrainVertexShader) {
		terrainVertexShader->Release();
		terrainVertexShader = nullptr;
	}
	if (terrainOffsetVertexShader) {
		terrainOffsetVertexShader->Release();
		terrainOffsetVertexShader = nullptr;
	}
	if (depthBlendShader) {
		depthBlendShader->Release();
		depthBlendShader = nullptr;
	}
}

void TerrainBlending::Hooks::Main_RenderDepth::thunk(bool a1, bool a2)
{
	auto singleton = VariableCache::GetSingleton()->terrainBlending;
	auto shaderCache = VariableCache::GetSingleton()->shaderCache;
	auto renderer = VariableCache::GetSingleton()->renderer;

	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	singleton->averageEyePosition = Util::GetAverageEyePosition();

	if (shaderCache->IsEnabled()) {
		mainDepth.depthSRV = singleton->blendedDepthTexture->srv.get();
		zPrepassCopy.depthSRV = singleton->blendedDepthTexture->srv.get();

		singleton->renderDepth = true;
		singleton->ResetDepth();

		func(a1, a2);

		singleton->renderDepth = false;

		if (singleton->renderTerrainDepth) {
			singleton->renderTerrainDepth = false;
			singleton->ResetTerrainDepth();
		}

		singleton->BlendPrepassDepths();
	} else {
		mainDepth.depthSRV = singleton->depthSRVBackup;
		zPrepassCopy.depthSRV = singleton->prepassSRVBackup;

		func(a1, a2);
	}
}

void TerrainBlending::Hooks::BSBatchRenderer__RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	auto singleton = VariableCache::GetSingleton()->terrainBlending;
	auto shaderCache = VariableCache::GetSingleton()->shaderCache;
	auto deferred = VariableCache::GetSingleton()->deferred;

	if (shaderCache->IsEnabled()) {
		if (singleton->renderDepth) {
			// Entering or exiting terrain depth section
			bool inTerrain = a_pass->shaderProperty && a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);

			if (inTerrain) {
				if ((a_pass->geometry->worldBound.center.GetDistance(singleton->averageEyePosition) - a_pass->geometry->worldBound.radius) > 1024.0f) {
					inTerrain = false;
				}
			}

			if (singleton->renderTerrainDepth != inTerrain) {
				if (!inTerrain)
					singleton->ResetTerrainDepth();
				singleton->renderTerrainDepth = inTerrain;
			}

			if (inTerrain)
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);  // Run terrain twice
		} else if (deferred->inWorld) {
			// Entering or exiting terrain section
			bool inTerrain = a_pass->shaderProperty && a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);

			if (inTerrain) {
				if ((a_pass->geometry->worldBound.center.GetDistance(singleton->averageEyePosition) - a_pass->geometry->worldBound.radius) > 1024.0f) {
					inTerrain = false;
				}
			}

			if (inTerrain) {
				RenderPass call{ a_pass, a_technique, a_alphaTest, a_renderFlags };
				singleton->renderPasses.push_back(call);
				return;
			}
		}
	}
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void TerrainBlending::RenderTerrain()
{
	renderTerrainWorld = true;

	auto renderer = VariableCache::GetSingleton()->renderer;
	auto context = VariableCache::GetSingleton()->context;
	auto shadowState = VariableCache::GetSingleton()->shadowState;
	auto stateUpdateFlags = VariableCache::GetSingleton()->stateUpdateFlags;

	static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

	// Enable alpha blending
	GET_INSTANCE_MEMBER(alphaBlendMode, shadowState)
	alphaBlendMode = 1;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

	// Enable rendering for depth below the surface
	GET_INSTANCE_MEMBER(depthStencilDepthMode, shadowState)
	depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTest;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);

	// Used to get the distance of the surface to the lowest depth
	auto view = terrainDepth.depthSRV;
	context->PSSetShaderResources(55, 1, &view);

	for (auto& renderPass : renderPasses)
		TerrainBlending::Hooks::BSBatchRenderer__RenderPassImmediately::func(renderPass.a_pass, renderPass.a_technique, renderPass.a_alphaTest, renderPass.a_renderFlags);

	renderPasses.clear();

	renderTerrainWorld = false;

	ResetTerrainWorld();

	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	mainDepth.depthSRV = depthSRVBackup;
}