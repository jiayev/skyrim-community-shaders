#include "XeGTAO.h"

#include "Deferred.h"
#include "Menu.h"
#include "State.h"
#include "Util.h"

void XeGTAOFeature::DrawSettings()
{
	static float debugRescale = .3f;
	ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

	BUFFER_VIEWER_NODE(workingDepths, debugRescale)
	BUFFER_VIEWER_NODE(workingEdges, debugRescale)
	BUFFER_VIEWER_NODE(workingAOTerm, debugRescale)
	BUFFER_VIEWER_NODE(workingAOTermPong, debugRescale)
	BUFFER_VIEWER_NODE(outputAO, debugRescale)
	BUFFER_VIEWER_NODE(outputNormals, debugRescale)
}

void XeGTAOFeature::SetupResources()
{
	CompileComputeShaders();

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC texDesc{};
	main.texture->GetDesc(&texDesc);

	texDesc.Format = DXGI_FORMAT_R16_FLOAT;
	texDesc.MipLevels = XE_GTAO_DEPTH_MIP_LEVELS;

	workingDepths = new Texture2D(texDesc);

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = {
			.MostDetailedMip = 0,
			.MipLevels = texDesc.MipLevels }
	};

	workingDepths->CreateSRV(srvDesc);

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};

	for (int mip = 0; mip < XE_GTAO_DEPTH_MIP_LEVELS; mip++) {
		uavDesc.Texture2D.MipSlice = mip;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(workingDepths->resource.get(), &uavDesc, &workingDepthsMIPViews[mip]));
	}

	texDesc.MipLevels = 1;
	srvDesc.Texture2D.MipLevels = 1;
	uavDesc.Texture2D.MipSlice = 0;

	texDesc.Format = DXGI_FORMAT_R8_UNORM;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	workingEdges = new Texture2D(texDesc);
	workingEdges->CreateSRV(srvDesc);
	workingEdges->CreateUAV(uavDesc);

	texDesc.Format = DXGI_FORMAT_R8_UINT;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	workingAOTerm = new Texture2D(texDesc);
	workingAOTerm->CreateSRV(srvDesc);
	workingAOTerm->CreateUAV(uavDesc);

	workingAOTermPong = new Texture2D(texDesc);
	workingAOTermPong->CreateSRV(srvDesc);
	workingAOTermPong->CreateUAV(uavDesc);

	outputAO = new Texture2D(texDesc);
	outputAO->CreateSRV(srvDesc);
	outputAO->CreateUAV(uavDesc);

	texDesc.Format = DXGI_FORMAT_R32_UINT;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	outputNormals = new Texture2D(texDesc);
	outputNormals->CreateSRV(srvDesc);
	outputNormals->CreateUAV(uavDesc);

	constantBuffer = new ConstantBuffer(ConstantBufferDesc<XeGTAO::GTAOConstants>());

	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &samplerPointClamp));
	}
}

void XeGTAOFeature::ClearShaderCache()
{
	if (CSPrefilterDepths16x16) {
		CSPrefilterDepths16x16->Release();
		CSPrefilterDepths16x16 = nullptr;
	}
	if (CSGTAOUltra) {
		CSGTAOUltra->Release();
		CSGTAOUltra = nullptr;
	}
	if (CSDenoisePass) {
		CSDenoisePass->Release();
		CSDenoisePass = nullptr;
	}
	if (CSDenoiseLastPass) {
		CSDenoiseLastPass->Release();
		CSDenoiseLastPass = nullptr;
	}

	CompileComputeShaders();
}

void XeGTAOFeature::CompileComputeShaders()
{
	CSPrefilterDepths16x16 = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\XeGTAO\\vaGTAO.hlsl", {}, "cs_5_0", "CSPrefilterDepths16x16"));
	CSGTAOUltra = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\XeGTAO\\vaGTAO.hlsl", {}, "cs_5_0", "CSGTAOUltra"));
	CSDenoisePass = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\XeGTAO\\vaGTAO.hlsl", {}, "cs_5_0", "CSDenoisePass"));
	CSDenoiseLastPass = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\XeGTAO\\vaGTAO.hlsl", {}, "cs_5_0", "CSDenoiseLastPass"));
	CSGenerateNormals = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\XeGTAO\\vaGTAO.hlsl", {}, "cs_5_0", "CSGenerateNormals"));
}

void XeGTAOFeature::Prepass()
{
	auto state = globals::state;

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto inputDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;

	{
		XeGTAO::GTAOConstants consts;

		auto usingTAA = Util::GetTemporal();
		auto gameViewport = globals::game::graphicsState;

		auto projMatrix = Util::GetCameraData(0).projMat;

		XeGTAO::GTAOUpdateConstants(consts, (int)state->screenSize.x, (int)state->screenSize.y, settings, &projMatrix._11, true, (usingTAA) ? (gameViewport->frameCount % 256) : (0));

		constantBuffer->Update(consts);

		ID3D11Buffer* buffers[1] = { constantBuffer->CB() };

		context->CSSetConstantBuffers(0, 1, buffers);
	}

	context->CSSetSamplers(0, 1, &samplerPointClamp);

	{
		state->BeginPerfEvent("XeGTAO Prepass Generate Normals");

		context->CSSetShader(CSGenerateNormals, nullptr, 0);
		context->CSSetShaderResources(0, 1, &inputDepth);
		auto uav = outputNormals->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

		context->Dispatch(((uint)state->screenSize.x + XE_GTAO_NUMTHREADS_X - 1) / XE_GTAO_NUMTHREADS_X, ((uint)state->screenSize.y + XE_GTAO_NUMTHREADS_Y - 1) / XE_GTAO_NUMTHREADS_Y, 1);
		state->EndPerfEvent();
	}

	{
		ID3D11UnorderedAccessView* uavs[1] { nullptr };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		ID3D11ShaderResourceView* srvs[1]{ nullptr };
		context->CSSetShaderResources(0, 1, srvs);

		ID3D11Buffer* buffers[1]{ nullptr };
		context->CSSetConstantBuffers(0, 1, buffers);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}

void XeGTAOFeature::GTAO()
{
	auto state = globals::state;

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto inputDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
	auto inputNormals = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS].SRV;

	settings.DenoisePasses = 3;

	{
		XeGTAO::GTAOConstants consts;

		auto usingTAA = Util::GetTemporal();
		auto gameViewport = globals::game::graphicsState;

		auto projMatrix = Util::GetCameraData(0).projMat;

		XeGTAO::GTAOUpdateConstants(consts, (int)state->screenSize.x, (int)state->screenSize.y, settings, &projMatrix._11, true, (usingTAA) ? (gameViewport->frameCount % 256) : (0));

		constantBuffer->Update(consts);

		ID3D11Buffer* buffers[1] = { constantBuffer->CB() };

		context->CSSetConstantBuffers(0, 1, buffers);
	}

	context->CSSetSamplers(0, 1, &samplerPointClamp);

	{
		state->BeginPerfEvent("GTAO Prefilter Depths");

		context->CSSetShader(CSPrefilterDepths16x16, nullptr, 0);

		// input SRVs
		ID3D11ShaderResourceView* srvs[1]{
			inputDepth,
		};
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[XE_GTAO_DEPTH_MIP_LEVELS]{ workingDepthsMIPViews[0], workingDepthsMIPViews[1], workingDepthsMIPViews[2], workingDepthsMIPViews[3], workingDepthsMIPViews[4] };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->Dispatch(((uint)state->screenSize.x + 16 - 1) / 16, ((uint)state->screenSize.y + 16 - 1) / 16, 1);

		state->EndPerfEvent();
	}

	{
		ID3D11UnorderedAccessView* uavs[5]{ nullptr, nullptr, nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
	}

	{
		state->BeginPerfEvent("GTAO Main Pass");

		context->CSSetShader(CSGTAOUltra, nullptr, 0);

		// input SRVs
		ID3D11ShaderResourceView* srvs[2]{ workingDepths->srv.get(), inputNormals };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[2]{ workingAOTerm->uav.get(), workingEdges->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->Dispatch(((uint)state->screenSize.x + XE_GTAO_NUMTHREADS_X - 1) / XE_GTAO_NUMTHREADS_X, ((uint)state->screenSize.y + XE_GTAO_NUMTHREADS_Y - 1) / XE_GTAO_NUMTHREADS_Y, 1);

		state->EndPerfEvent();
	}

	{
		ID3D11UnorderedAccessView* uavs[2]{ nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
	}

	{
		state->BeginPerfEvent("GTAO Denoise");

		const int passCount = std::max(1, settings.DenoisePasses);  // even without denoising we have to run a single last pass to output correct term into the external output texture
		for (int i = 0; i < passCount; i++) {
			const bool lastPass = i == passCount - 1;

			context->CSSetShader((lastPass) ? (CSDenoiseLastPass) : (CSDenoisePass), nullptr, 0);

			ID3D11ShaderResourceView* srvs[2]{ workingAOTerm->srv.get(), workingEdges->srv.get() };
			context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

			{
				ID3D11UnorderedAccessView* uavs[1]{ (lastPass) ? (outputAO->uav.get()) : (workingAOTermPong->uav.get()) };
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
			}

			context->Dispatch(((uint)state->screenSize.x + (XE_GTAO_NUMTHREADS_X * 2) - 1) / (XE_GTAO_NUMTHREADS_X * 2), ((uint)state->screenSize.y + XE_GTAO_NUMTHREADS_Y - 1) / XE_GTAO_NUMTHREADS_Y, 1);

			// ping becomes pong, pong becomes ping.
			auto temp = workingAOTerm;
			workingAOTerm = workingAOTermPong;
			workingAOTermPong = temp;

			{
				ID3D11UnorderedAccessView* uavs[2]{ nullptr, nullptr };
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
			}
		}

		state->EndPerfEvent();
	}
}