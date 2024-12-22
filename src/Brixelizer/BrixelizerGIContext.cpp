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
		
		UploadDDSTexture(
			Brixelizer::GetSingleton()->d3d12Device.get(),
			Brixelizer::GetSingleton()->commandList.get(),
			Brixelizer::GetSingleton()->commandQueue.get(),
			filePath,
			noiseTextures[i]);

		DX::ThrowIfFailed(Brixelizer::GetSingleton()->commandAllocator->Reset());
		DX::ThrowIfFailed(Brixelizer::GetSingleton()->commandList->Reset(Brixelizer::GetSingleton()->commandAllocator.get(), nullptr));
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

		ID3D11UnorderedAccessView* uavs[3] = { depth.uav, normal.uav, prevLitOutput.uav };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(GetCopyToSharedBufferCS(), nullptr, 0);

		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
	}

	ID3D11ShaderResourceView* views[3] = { nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[3] = { nullptr, nullptr, nullptr };
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

	//auto& main = brixelizer->renderTargetsD3D12[Deferred::GetSingleton()->forwardRenderTargets[0]];

	//auto& environmentMap = brixelizer->renderTargetsCubemapD3D12[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

	auto frameBufferCached = brixelizer->frameBufferCached;

	auto view = frameBufferCached.CameraView.Transpose();
	view._41 -= frameBufferCached.CameraPosAdjust.x;
	view._42 -= frameBufferCached.CameraPosAdjust.y;
	view._43 -= frameBufferCached.CameraPosAdjust.z;

	auto projection = frameBufferCached.CameraProj.Transpose();

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
	giDispatchDesc.environmentMapIntensity = 0.0f;
	giDispatchDesc.motionVectorScale = { 1.0f, 1.0f };

	giDispatchDesc.depth = ffxGetResourceDX12(depth.resource.get(), ffxGetResourceDescriptionDX12(depth.resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"Depth", FFX_RESOURCE_STATE_COMPUTE_READ);

	giDispatchDesc.normal = ffxGetResourceDX12(normal.resource.get(), ffxGetResourceDescriptionDX12(normal.resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"Normal", FFX_RESOURCE_STATE_COMPUTE_READ);
	giDispatchDesc.roughness = ffxGetResourceDX12(normalsRoughness.d3d12Resource.get(), ffxGetResourceDescriptionDX12(normalsRoughness.d3d12Resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"Roughness", FFX_RESOURCE_STATE_COMPUTE_READ);
	giDispatchDesc.motionVectors = ffxGetResourceDX12(motionVectors.d3d12Resource.get(), ffxGetResourceDescriptionDX12(motionVectors.d3d12Resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"MotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);

	giDispatchDesc.historyDepth = ffxGetResourceDX12(depth.resource.get(), ffxGetResourceDescriptionDX12(depth.resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"HistoryDepth", FFX_RESOURCE_STATE_COMPUTE_READ);
	giDispatchDesc.historyNormal = ffxGetResourceDX12(normal.resource.get(), ffxGetResourceDescriptionDX12(normal.resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"HistoryNormal", FFX_RESOURCE_STATE_COMPUTE_READ);
	giDispatchDesc.prevLitOutput = ffxGetResourceDX12(prevLitOutput.resource.get(), ffxGetResourceDescriptionDX12(prevLitOutput.resource.get(), FFX_RESOURCE_USAGE_READ_ONLY), L"PrevLitOutput", FFX_RESOURCE_STATE_COMPUTE_READ);

	giDispatchDesc.noiseTexture = ffxGetResourceDX12(noiseTextures[noiseIndex].get(), ffxGetResourceDescriptionDX12(noiseTextures[noiseIndex].get(), FFX_RESOURCE_USAGE_READ_ONLY), L"Noise", FFX_RESOURCE_STATE_COMPUTE_READ);
	giDispatchDesc.environmentMap = ffxGetResourceDX12(nullptr, ffxGetResourceDescriptionDX12(nullptr, FFX_RESOURCE_USAGE_READ_ONLY), L"EnvironmentMap", FFX_RESOURCE_STATE_COMPUTE_READ);

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
