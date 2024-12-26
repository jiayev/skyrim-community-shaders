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

	Brixelizer::CreatedWrappedResource(texDesc, currLitOutput);
	Brixelizer::CreatedWrappedResource(texDesc, prevLitOutput);
	Brixelizer::CreatedWrappedResource(texDesc, diffuseGi);
	Brixelizer::CreatedWrappedResource(texDesc, specularGi);
	Brixelizer::CreatedWrappedResource(texDesc, roughness);
	Brixelizer::CreatedWrappedResource(texDesc, motionVectors);

	{
        D3D12_RESOURCE_DESC textureDesc = {};
        textureDesc.Dimension           = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Alignment           = 0;
        textureDesc.Width               = 512;
        textureDesc.Height              = 512;
        textureDesc.DepthOrArraySize    = 6;
        textureDesc.MipLevels           = 1;
        textureDesc.Format              = DXGI_FORMAT_R16G16B16A16_FLOAT;
        textureDesc.SampleDesc.Count    = 1;
        textureDesc.SampleDesc.Quality  = 0;
        textureDesc.Flags               = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        textureDesc.Layout              = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		// Create the texture resource (cubemap)
		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
		Brixelizer::GetSingleton()->d3d12Device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(&environmentMap));
	}
}

HRESULT UploadDDSTexture(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	ID3D12CommandQueue* commandQueue,
	const std::string& filePath,
	winrt::com_ptr<ID3D12Resource>& textureResource)
{
	// Convert std::string to std::wstring
	std::wstring wFilePath(filePath.begin(), filePath.end());

	// Load the DDS texture
	std::unique_ptr<uint8_t[]> ddsData;
	std::vector<D3D12_SUBRESOURCE_DATA> subresources;
	HRESULT hr = DirectX::LoadDDSTextureFromFile(
		device, wFilePath.c_str(), textureResource.put(), ddsData, subresources);

	if (FAILED(hr)) {
		return hr;
	}

	D3D12_SUBRESOURCE_DATA subresourceData = subresources[0];

	// Create an upload heap for texture data
	UINT64 uploadBufferSize = GetRequiredIntermediateSize(textureResource.get(), 0, 1);
	winrt::com_ptr<ID3D12Resource> uploadHeap;

	CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);

	hr = device->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(uploadHeap.put()));

	if (FAILED(hr)) {
		return hr;
	}

	// Copy subresource data to the upload heap
	UpdateSubresources(
		commandList,
		textureResource.get(),
		uploadHeap.get(),
		0, 0, 1,
		&subresourceData);

	// Transition the texture to PIXEL_SHADER_RESOURCE state
	CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		textureResource.get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	commandList->ResourceBarrier(1, &barrier);

	// Execute the command list
	hr = commandList->Close();
	if (FAILED(hr)) {
		return hr;
	}

	ID3D12CommandList* ppCommandLists[] = { commandList };
	commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	// Synchronize with the GPU
	winrt::com_ptr<ID3D12Fence> fence;
	HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!fenceEvent) {
		return HRESULT_FROM_WIN32(GetLastError());
	}

	hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()));
	if (FAILED(hr)) {
		return hr;
	}

	UINT64 fenceValue = 1;
	commandQueue->Signal(fence.get(), fenceValue);
	if (fence->GetCompletedValue() < fenceValue) {
		fence->SetEventOnCompletion(fenceValue, fenceEvent);
		WaitForSingleObject(fenceEvent, INFINITE);
	}
	CloseHandle(fenceEvent);

	return S_OK;
}

void BrixelizerGIContext::CreateNoiseTextures()
{
	for (int i = 0; i < 16; i++) {
		auto filePath = std::format("Data\\Shaders\\Brixelizer\\Noise\\LDR_RG01_{}.dds", i);
		DX::ThrowIfFailed(Brixelizer::GetSingleton()->commandAllocator->Reset());
		DX::ThrowIfFailed(Brixelizer::GetSingleton()->commandList->Reset(Brixelizer::GetSingleton()->commandAllocator.get(), nullptr));
		UploadDDSTexture(
			Brixelizer::GetSingleton()->d3d12Device.get(),
			Brixelizer::GetSingleton()->commandList.get(),
			Brixelizer::GetSingleton()->commandQueue.get(),
			filePath,
			noiseTextures[i]);
	}
}

void BrixelizerGIContext::InitBrixelizerGIContext()
{
	const auto displaySize = State::GetSingleton()->screenSize;

	// Context Creation
	FfxBrixelizerGIContextDescription desc = {
		.flags = {},
		.internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT,
		.displaySize = { static_cast<uint>(displaySize.x), static_cast<uint>(displaySize.y) },
		.backendInterface = BrixelizerContext::GetSingleton()->initializationParameters.backendInterface,
	};
	if (ffxBrixelizerGIContextCreate(&brixelizerGIContext, &desc) != FFX_OK)
		logger::critical("Failed to create Brixelizer GI context.");
	if (ffxBrixelizerGIGetEffectVersion() == FFX_SDK_MAKE_VERSION(1, 0, 0))
		logger::critical("FidelityFX Brixelizer GI sample requires linking with a 1.0 version Brixelizer GI library.");

	CreateMiscTextures();
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
	auto& main = renderer->GetRuntimeData().renderTargets[Deferred::GetSingleton()->forwardRenderTargets[0]];

	{
		auto dispatchCount = Util::GetScreenDispatchCount(true);

		ID3D11ShaderResourceView* views[3] = { depth11.depthSRV, normalRoughness.SRV, main.SRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[5] = { depth.uav, normal.uav, currLitOutput.uav, roughness.uav, motionVectors.uav };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(GetCopyToSharedBufferCS(), nullptr, 0);

		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
	}

	ID3D11ShaderResourceView* views[3] = { nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[5] = { nullptr, nullptr, nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);
}

void BrixelizerGIContext::CopyHistoryResources(ID3D12GraphicsCommandList* cmdList)
{
	//auto& context = State::GetSingleton()->context;
	//context->CopyResource(historyDepth.resource11, depth.resource11);
	//context->CopyResource(historyNormal.resource11, normal.resource11);
	//context->CopyResource(prevLitOutput.resource11, currLitOutput.resource11);

	{
		CD3DX12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(historyDepth.resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
			CD3DX12_RESOURCE_BARRIER::Transition(historyNormal.resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
			CD3DX12_RESOURCE_BARRIER::Transition(prevLitOutput.resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
			CD3DX12_RESOURCE_BARRIER::Transition(depth.resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(normal.resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(currLitOutput.resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
		};
		cmdList->ResourceBarrier(_countof(barriers), barriers);
	}
	cmdList->CopyResource(historyDepth.resource.get(), depth.resource.get());
	cmdList->CopyResource(historyNormal.resource.get(), normal.resource.get());
	cmdList->CopyResource(prevLitOutput.resource.get(), currLitOutput.resource.get());
	{
		CD3DX12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(historyDepth.resource.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(historyNormal.resource.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(prevLitOutput.resource.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(depth.resource.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(normal.resource.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(currLitOutput.resource.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		cmdList->ResourceBarrier(_countof(barriers), barriers);
	}
}

void BrixelizerGIContext::UpdateBrixelizerGIContext(ID3D12GraphicsCommandList* cmdList)
{
	auto brixelizer = Brixelizer::GetSingleton();
	auto brixelizerContext = BrixelizerContext::GetSingleton();

	//auto& normalsRoughness = brixelizer->renderTargetsD3D12[NORMALROUGHNESS];
	//auto& motionVectors = brixelizer->renderTargetsD3D12[RE::RENDER_TARGET::kMOTION_VECTOR];

	//auto& main = brixelizer->renderTargetsD3D12[Deferred::GetSingleton()->forwardRenderTargets[0]];

	//auto& environmentMap = brixelizer->renderTargetsCubemapD3D12[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

	auto frameBufferCached = brixelizer->frameBufferCached;

	auto cameraViewInverseAdjusted = frameBufferCached.CameraViewInverse.Transpose();
	cameraViewInverseAdjusted._41 += frameBufferCached.CameraPosAdjust.x;
	cameraViewInverseAdjusted._42 += frameBufferCached.CameraPosAdjust.y;
	cameraViewInverseAdjusted._43 += frameBufferCached.CameraPosAdjust.z;
	auto cameraProjInverse = frameBufferCached.CameraProjInverse.Transpose();

	auto view = cameraViewInverseAdjusted.Invert();
	auto projection = cameraProjInverse.Invert();

	static auto prevView = view;
	static auto prevProjection = projection;

	uint noiseIndex = RE::BSGraphics::State::GetSingleton()->frameCount % 16u;

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
	giDispatchDesc.environmentMapIntensity = 0.1f;
	giDispatchDesc.motionVectorScale = { 1.0f, 1.0f };

	giDispatchDesc.depth = ffxGetResourceDX12(depth.resource.get(), ffxGetResourceDescriptionDX12(depth.resource.get()), L"Depth");

	giDispatchDesc.normal = ffxGetResourceDX12(normal.resource.get(), ffxGetResourceDescriptionDX12(normal.resource.get()), L"Normal");
	giDispatchDesc.roughness = ffxGetResourceDX12(roughness.resource.get(), ffxGetResourceDescriptionDX12(roughness.resource.get()), L"Roughness");
	giDispatchDesc.motionVectors = ffxGetResourceDX12(motionVectors.resource.get(), ffxGetResourceDescriptionDX12(motionVectors.resource.get()), L"MotionVectors");

	giDispatchDesc.historyDepth = ffxGetResourceDX12(historyDepth.resource.get(), ffxGetResourceDescriptionDX12(historyDepth.resource.get()), L"HistoryDepth");
	giDispatchDesc.historyNormal = ffxGetResourceDX12(historyNormal.resource.get(), ffxGetResourceDescriptionDX12(historyNormal.resource.get()), L"HistoryNormal");
	giDispatchDesc.prevLitOutput = ffxGetResourceDX12(prevLitOutput.resource.get(), ffxGetResourceDescriptionDX12(prevLitOutput.resource.get()), L"PrevLitOutput");

	giDispatchDesc.noiseTexture = ffxGetResourceDX12(noiseTextures[noiseIndex].get(), ffxGetResourceDescriptionDX12(noiseTextures[noiseIndex].get()), L"Noise");
	giDispatchDesc.environmentMap = ffxGetResourceDX12(environmentMap.get(), ffxGetResourceDescriptionDX12(environmentMap.get()), L"EnvironmentMap");

	giDispatchDesc.sdfAtlas = ffxGetResourceDX12(brixelizerContext->sdfAtlas.get(), ffxGetResourceDescriptionDX12(brixelizerContext->sdfAtlas.get()), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
	giDispatchDesc.bricksAABBs = ffxGetResourceDX12(brixelizerContext->brickAABBs.get(), ffxGetResourceDescriptionDX12(brixelizerContext->brickAABBs.get()), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
	giDispatchDesc.bricksAABBs.description.stride = 4;

	for (uint32_t i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; ++i) {
		giDispatchDesc.cascadeAABBTrees[i] = ffxGetResourceDX12(brixelizerContext->cascadeAABBTrees[i].get(), ffxGetResourceDescriptionDX12(brixelizerContext->cascadeAABBTrees[i].get()), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
		giDispatchDesc.cascadeAABBTrees[i].description.stride = 4;
		giDispatchDesc.cascadeBrickMaps[i] = ffxGetResourceDX12(brixelizerContext->cascadeBrickMaps[i].get(), ffxGetResourceDescriptionDX12(brixelizerContext->cascadeBrickMaps[i].get()), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
		giDispatchDesc.cascadeBrickMaps[i].description.stride = 4;
	}

	giDispatchDesc.outputDiffuseGI = ffxGetResourceDX12(diffuseGi.resource.get(), ffxGetResourceDescriptionDX12(diffuseGi.resource.get()), L"OutputDiffuseGI");
	giDispatchDesc.outputSpecularGI = ffxGetResourceDX12(specularGi.resource.get(), ffxGetResourceDescriptionDX12(specularGi.resource.get()), L"OutputSpecularGI");

	if (ffxBrixelizerGetRawContext(&brixelizerContext->brixelizerContext, &giDispatchDesc.brixelizerContext) != FFX_OK)
		logger::error("Failed to get Brixelizer context pointer.");

	if (ffxBrixelizerGIContextDispatch(&brixelizerGIContext, &giDispatchDesc, ffxGetCommandListDX12(cmdList)) != FFX_OK)
		logger::error("Failed to dispatch Brixelizer GI.");

	{
		auto state = State::GetSingleton();

		FfxBrixelizerGIDebugDescription debug_desc = {};

		memcpy(&debug_desc.view, &view, sizeof(debug_desc.view));
		memcpy(&debug_desc.projection, &projection, sizeof(debug_desc.projection));

		debug_desc.outputSize[0] = (uint)state->screenSize.x;
		debug_desc.outputSize[1] = (uint)state->screenSize.y;
		debug_desc.normalsUnpackMul = 2.0f;
		debug_desc.normalsUnpackAdd = -1.0f;

		debug_desc.debugMode = FFX_BRIXELIZER_GI_DEBUG_MODE_RADIANCE_CACHE;

		debug_desc.startCascade = brixelizerContext->m_StartCascadeIdx + (2 * NUM_BRIXELIZER_CASCADES);
		debug_desc.endCascade = brixelizerContext->m_EndCascadeIdx + (2 * NUM_BRIXELIZER_CASCADES);
		debug_desc.depth = ffxGetResourceDX12(depth.resource.get(), ffxGetResourceDescriptionDX12(depth.resource.get()), L"Depth");
		debug_desc.normal = ffxGetResourceDX12(normal.resource.get(), ffxGetResourceDescriptionDX12(normal.resource.get()), L"Normal");

		debug_desc.sdfAtlas = ffxGetResourceDX12(brixelizerContext->sdfAtlas.get(), ffxGetResourceDescriptionDX12(brixelizerContext->sdfAtlas.get()), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
		debug_desc.bricksAABBs = ffxGetResourceDX12(brixelizerContext->brickAABBs.get(), ffxGetResourceDescriptionDX12(brixelizerContext->brickAABBs.get()), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
		debug_desc.bricksAABBs.description.stride = 4;
		for (uint32_t i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; ++i) {
			debug_desc.cascadeAABBTrees[i] = ffxGetResourceDX12(brixelizerContext->cascadeAABBTrees[i].get(), ffxGetResourceDescriptionDX12(brixelizerContext->cascadeAABBTrees[i].get()), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
			debug_desc.cascadeAABBTrees[i].description.stride = 4;
			debug_desc.cascadeBrickMaps[i] = ffxGetResourceDX12(brixelizerContext->cascadeBrickMaps[i].get(), ffxGetResourceDescriptionDX12(brixelizerContext->cascadeBrickMaps[i].get()), nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
			debug_desc.cascadeBrickMaps[i].description.stride = 4;
		}

		debug_desc.outputDebug = ffxGetResourceDX12(brixelizerContext->debugRenderTarget.resource.get(), ffxGetResourceDescriptionDX12(brixelizerContext->debugRenderTarget.resource.get(), FFX_RESOURCE_USAGE_UAV), nullptr, FFX_RESOURCE_STATE_UNORDERED_ACCESS);

		if (ffxBrixelizerGetRawContext(&brixelizerContext->brixelizerContext, &debug_desc.brixelizerContext) != FFX_OK)
			logger::error("Failed to dispatch Brixelizer GI.");

		if (ffxBrixelizerGIContextDebugVisualization(&brixelizerGIContext, &debug_desc, ffxGetCommandListDX12(cmdList)) != FFX_OK)
			logger::error("Failed to dispatch Brixelizer GI.");
	}
}
