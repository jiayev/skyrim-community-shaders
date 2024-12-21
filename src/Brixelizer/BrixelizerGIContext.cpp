#include "BrixelizerGIContext.h"

#include <DDSTextureLoader.h>
#include <DirectXMesh.h>

#include "BrixelizerContext.h"
#include "Deferred.h"
#include "Util.h"

void BrixelizerGIContext::CreateMiscTextures()
{
	const auto displaySize = State::GetSingleton()->screenSize;

	D3D11_TEXTURE2D_DESC texDesc;
	texDesc.Width = (UINT)displaySize.x;
	texDesc.Height = (UINT)displaySize.y;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	texDesc.CPUAccessFlags = 0;
	texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

	CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);

	Brixelizer::CreatedWrappedResource(texDesc, depth);
	Brixelizer::CreatedWrappedResource(texDesc, historyDepth);

	texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	Brixelizer::CreatedWrappedResource(texDesc, normal);
	Brixelizer::CreatedWrappedResource(texDesc, historyNormal);

	Brixelizer::CreatedWrappedResource(texDesc, prevLitOutput);
	Brixelizer::CreatedWrappedResource(texDesc, diffuseGi);
	Brixelizer::CreatedWrappedResource(texDesc, specularGi);
}

void BrixelizerGIContext::CreateNoiseTextures()
{
	auto& device = State::GetSingleton()->device;
	auto& context = State::GetSingleton()->context;

	for (int i = 0; i < 16; i++) {
		winrt::com_ptr<ID3D11Resource> noiseTexture11;

		wchar_t filePath[128];
		swprintf(filePath, 128, L"Data\\Shaders\\Brixelizer\\Noise\\LDR_RG01_%d.dds", i);

		DirectX::CreateDDSTextureFromFileEx(
			device,
			context,
			filePath,
			SIZE_T_MAX,
			D3D11_USAGE_DEFAULT,
			D3D11_BIND_SHADER_RESOURCE,
			0u,
			D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED,
			DirectX::DDS_LOADER_FLAGS::DDS_LOADER_DEFAULT,
			noiseTexture11.put(),
			nullptr);

		// Query the DXGIResource1 interface to access shared NT handle
		winrt::com_ptr<IDXGIResource1> dxgiResource1;
		DX::ThrowIfFailed(noiseTexture11->QueryInterface(IID_PPV_ARGS(&dxgiResource1)));

		// Create the shared NT handle
		HANDLE sharedNtHandle = nullptr;
		DX::ThrowIfFailed(dxgiResource1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &sharedNtHandle));

		// Open the shared handle in D3D12
		winrt::com_ptr<ID3D12Resource> d3d12Resource;
		DX::ThrowIfFailed(Brixelizer::GetSingleton()->d3d12Device->OpenSharedHandle(sharedNtHandle, IID_PPV_ARGS(&d3d12Resource)));
		CloseHandle(sharedNtHandle);  // Close the handle after opening it in D3D12

		winrt::com_ptr<ID3D11Texture2D> texture11;
		DX::ThrowIfFailed(noiseTexture11->QueryInterface(IID_PPV_ARGS(&texture11)));

		D3D11_TEXTURE2D_DESC texDesc11{};
		texture11->GetDesc(&texDesc11);

		D3D12_RESOURCE_DESC texDesc = {};
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texDesc.Width = texDesc11.Width;
		texDesc.Height = texDesc11.Height;
		texDesc.DepthOrArraySize = 1;
		texDesc.MipLevels = 1;
		texDesc.Format = texDesc11.Format;
		texDesc.SampleDesc.Count = 1;
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
		DX::ThrowIfFailed(Brixelizer::GetSingleton()->d3d12Device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&noiseTextures[i])));

		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers{
				CD3DX12_RESOURCE_BARRIER::Transition(d3d12Resource.get(),
					D3D12_RESOURCE_STATE_COMMON,
					D3D12_RESOURCE_STATE_COPY_SOURCE)
			};
			Brixelizer::GetSingleton()->commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}

		Brixelizer::GetSingleton()->commandList->CopyResource(noiseTextures[i].get(), d3d12Resource.get());

		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers{
				CD3DX12_RESOURCE_BARRIER::Transition(d3d12Resource.get(),
					D3D12_RESOURCE_STATE_COPY_SOURCE,
					D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(noiseTextures[i].get(),
					D3D12_RESOURCE_STATE_COPY_DEST,
					D3D12_RESOURCE_STATE_COMMON),
			};
			Brixelizer::GetSingleton()->commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}
	}
}

void BrixelizerGIContext::InitBrixelizerGIContext()
{
	const auto displaySize = State::GetSingleton()->screenSize;

	// Context Creation
	FfxBrixelizerGIContextDescription desc = {
		.flags = FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED,
		.internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT,
		.displaySize = { static_cast<uint>(displaySize.x), static_cast<uint>(displaySize.y) },
		.backendInterface = BrixelizerContext::GetSingleton()->initializationParameters.backendInterface,
	};
	if (ffxBrixelizerGIContextCreate(&brixelizerGIContext, &desc) != FFX_OK)
		logger::critical("Failed to create Brixelizer GI context.");
	if (ffxBrixelizerGIGetEffectVersion() == FFX_SDK_MAKE_VERSION(1, 0, 0))
		logger::critical("FidelityFX Brixelizer GI sample requires linking with a 1.0 version Brixelizer GI library.");

	// Resource Creation
	D3D12_RESOURCE_DESC texDesc = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		.Width = static_cast<UINT64>(displaySize.x),
		.Height = static_cast<UINT64>(displaySize.y),
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.SampleDesc = { .Count = 1 },
		.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
		.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
	};

	CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);

	CreateMiscTextures();
	CreateNoiseTextures();
}

ID3D11ComputeShader* BrixelizerGIContext::GetCopyToSharedBufferCS()
{
	if (!copyToSharedBufferCS)
		copyToSharedBufferCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Brixelizer\\CopyToSharedBufferCS.hlsl", {}, "cs_5_0");
	return copyToSharedBufferCS;
}

void BrixelizerGIContext::ClearShaderCache()
{
	if (copyToSharedBufferCS)
		copyToSharedBufferCS->Release();
	copyToSharedBufferCS = nullptr;
}

void BrixelizerGIContext::CopyResourcesToSharedBuffers()
{
	auto& context = State::GetSingleton()->context;
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto& depth11 = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto& normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];

	{
		auto dispatchCount = Util::GetScreenDispatchCount(true);

		ID3D11ShaderResourceView* views[2] = { depth11.depthSRV, normalRoughness.SRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[2] = { depth.uav, normal.uav };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(GetCopyToSharedBufferCS(), nullptr, 0);

		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
	}

	ID3D11ShaderResourceView* views[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);
}

void BrixelizerGIContext::UpdateBrixelizerGIContext()
{
	auto brixelizer = Brixelizer::GetSingleton();
	auto brixelizerContext = BrixelizerContext::GetSingleton();

	auto& normalsRoughness = brixelizer->renderTargetsD3D12[NORMALROUGHNESS];
	auto& motionVectors = brixelizer->renderTargetsD3D12[RE::RENDER_TARGET::kMOTION_VECTOR];

	auto& main = brixelizer->renderTargetsD3D12[Deferred::GetSingleton()->forwardRenderTargets[0]];

	auto& environmentMap = brixelizer->renderTargetsCubemapD3D12[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

	auto frameBufferCached = brixelizer->frameBufferCached;

	auto view = frameBufferCached.CameraView.Transpose();
	view._41 -= frameBufferCached.CameraPosAdjust.x;
	view._42 -= frameBufferCached.CameraPosAdjust.y;
	view._43 -= frameBufferCached.CameraPosAdjust.z;

	auto projection = frameBufferCached.CameraProj.Transpose();

	static auto prevView = view;
	static auto prevProjection = projection;

	uint noiseIndex = RE::BSGraphics::State::GetSingleton()->frameCount % 16u;

	{
		std::vector<D3D12_RESOURCE_BARRIER> barriers{
			CD3DX12_RESOURCE_BARRIER::Transition(depth.resource.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(normal.resource.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(normalsRoughness.d3d12Resource.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(motionVectors.d3d12Resource.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(historyDepth.resource.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(historyNormal.resource.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(main.d3d12Resource.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(noiseTextures[noiseIndex].get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(diffuseGi.resource.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(specularGi.resource.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
		};
		brixelizer->commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
	}

	{
		memcpy(&giDispatchDesc.view, &view, sizeof(giDispatchDesc.view));
		memcpy(&giDispatchDesc.projection, &projection, sizeof(giDispatchDesc.projection));
		memcpy(&giDispatchDesc.prevView, &prevView, sizeof(giDispatchDesc.prevView));
		memcpy(&giDispatchDesc.prevProjection, &prevProjection, sizeof(giDispatchDesc.prevProjection));

		prevView = view;
		prevProjection = projection;

		memcpy(&giDispatchDesc.cameraPosition, &frameBufferCached.CameraPosAdjust, sizeof(giDispatchDesc.cameraPosition));

		giDispatchDesc.startCascade = brixelizerContext->m_StartCascadeIdx + (2 * NUM_BRIXELIZER_CASCADES);
		giDispatchDesc.endCascade = brixelizerContext->m_EndCascadeIdx + (2 * NUM_BRIXELIZER_CASCADES);
		giDispatchDesc.rayPushoff = brixelizerContext->m_RayPushoff;
		giDispatchDesc.sdfSolveEps = brixelizerContext->m_SdfSolveEps;
		giDispatchDesc.specularRayPushoff = brixelizerContext->m_RayPushoff;
		giDispatchDesc.specularSDFSolveEps = brixelizerContext->m_SdfSolveEps;
		giDispatchDesc.tMin = brixelizerContext->m_TMin;
		giDispatchDesc.tMax = brixelizerContext->m_TMax;

		giDispatchDesc.normalsUnpackMul = 2.0f;
		giDispatchDesc.normalsUnpackAdd = -1.0f;
		giDispatchDesc.isRoughnessPerceptual = false;
		giDispatchDesc.roughnessChannel = 1;
		giDispatchDesc.roughnessThreshold = 0.9f;
		giDispatchDesc.environmentMapIntensity = 0.0f;
		giDispatchDesc.motionVectorScale = { 1.0f, 1.0f };

		giDispatchDesc.depth = ffxGetResourceDX12(depth.resource.get(), ffxGetResourceDescriptionDX12(depth.resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"Depth", FFX_RESOURCE_STATE_COMPUTE_READ);

		giDispatchDesc.normal = ffxGetResourceDX12(normal.resource.get(), ffxGetResourceDescriptionDX12(normal.resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"Normal", FFX_RESOURCE_STATE_COMPUTE_READ);
		giDispatchDesc.roughness = ffxGetResourceDX12(normalsRoughness.d3d12Resource.get(), ffxGetResourceDescriptionDX12(normalsRoughness.d3d12Resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"Roughness", FFX_RESOURCE_STATE_COMPUTE_READ);
		giDispatchDesc.motionVectors = ffxGetResourceDX12(motionVectors.d3d12Resource.get(), ffxGetResourceDescriptionDX12(motionVectors.d3d12Resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"MotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);

		giDispatchDesc.historyDepth = ffxGetResourceDX12(historyDepth.resource.get(), ffxGetResourceDescriptionDX12(historyDepth.resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"HistoryDepth", FFX_RESOURCE_STATE_COMPUTE_READ);
		giDispatchDesc.historyNormal = ffxGetResourceDX12(historyNormal.resource.get(), ffxGetResourceDescriptionDX12(historyNormal.resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"HistoryNormal", FFX_RESOURCE_STATE_COMPUTE_READ);
		giDispatchDesc.prevLitOutput = ffxGetResourceDX12(main.d3d12Resource.get(), ffxGetResourceDescriptionDX12(main.d3d12Resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"PrevLitOutput", FFX_RESOURCE_STATE_COMPUTE_READ);

		giDispatchDesc.noiseTexture = ffxGetResourceDX12(noiseTextures[noiseIndex].get(), ffxGetResourceDescriptionDX12(noiseTextures[noiseIndex].get(), FFX_RESOURCE_USAGE_READ_ONLY), L"Noise", FFX_RESOURCE_STATE_COMPUTE_READ);
		giDispatchDesc.environmentMap = ffxGetResourceDX12(environmentMap.d3d12Resource.get(), ffxGetResourceDescriptionDX12(environmentMap.d3d12Resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"EnvironmentMap", FFX_RESOURCE_STATE_COMPUTE_READ);

		giDispatchDesc.sdfAtlas = ffxGetResourceDX12(brixelizerContext->sdfAtlas.get(), ffxGetResourceDescriptionDX12(brixelizerContext->sdfAtlas.get(), FFX_RESOURCE_USAGE_READ_ONLY), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
		giDispatchDesc.bricksAABBs = ffxGetResourceDX12(brixelizerContext->brickAABBs.get(), ffxGetResourceDescriptionDX12(brixelizerContext->brickAABBs.get(), FFX_RESOURCE_USAGE_READ_ONLY), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);

		for (uint32_t i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; ++i) {
			giDispatchDesc.cascadeAABBTrees[i] = ffxGetResourceDX12(brixelizerContext->cascadeAABBTrees[i].get(), ffxGetResourceDescriptionDX12(brixelizerContext->cascadeAABBTrees[i].get(), FFX_RESOURCE_USAGE_READ_ONLY), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
			giDispatchDesc.cascadeBrickMaps[i] = ffxGetResourceDX12(brixelizerContext->cascadeBrickMaps[i].get(), ffxGetResourceDescriptionDX12(brixelizerContext->cascadeBrickMaps[i].get(), FFX_RESOURCE_USAGE_READ_ONLY), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
		}

		giDispatchDesc.outputDiffuseGI = ffxGetResourceDX12(diffuseGi.resource.get(), ffxGetResourceDescriptionDX12(diffuseGi.resource.get(), FFX_RESOURCE_USAGE_UAV), L"OutputDiffuseGI", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
		giDispatchDesc.outputSpecularGI = ffxGetResourceDX12(specularGi.resource.get(), ffxGetResourceDescriptionDX12(specularGi.resource.get(), FFX_RESOURCE_USAGE_UAV), L"OutputSpecularGI", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

		if (ffxBrixelizerGetRawContext(&brixelizerContext->brixelizerContext, &giDispatchDesc.brixelizerContext) != FFX_OK)
			logger::error("Failed to get Brixelizer context pointer.");

		if (ffxBrixelizerGIContextDispatch(&brixelizerGIContext, &giDispatchDesc, ffxGetCommandListDX12(brixelizer->commandList.get())) != FFX_OK)
			logger::error("Failed to dispatch Brixelizer GI.");
	}

	{
		std::vector<D3D12_RESOURCE_BARRIER> barriers{
			CD3DX12_RESOURCE_BARRIER::Transition(depth.resource.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(normal.resource.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(normalsRoughness.d3d12Resource.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(motionVectors.d3d12Resource.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(historyDepth.resource.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(historyNormal.resource.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(main.d3d12Resource.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(noiseTextures[noiseIndex].get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(diffuseGi.resource.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(specularGi.resource.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON)
		};
		brixelizer->commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
	}
}
