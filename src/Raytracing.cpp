#include "Raytracing.h"

#include "Deferred.h"

void Raytracing::InitD3D12()
{
	auto manager = RE::BSGraphics::Renderer::GetSingleton();
	auto device = reinterpret_cast<ID3D11Device*>(manager->GetRuntimeData().forwarder);

	winrt::com_ptr<IDXGIDevice> dxgiDevice;
	DX::ThrowIfFailed(device->QueryInterface(dxgiDevice.put()));

	winrt::com_ptr<IDXGIAdapter> dxgiAdapter;
	DX::ThrowIfFailed(dxgiDevice->GetAdapter(dxgiAdapter.put()));

	dxgiAdapter->QueryInterface(dxgiAdapter3.put());

	DX::ThrowIfFailed(D3D12CreateDevice(dxgiAdapter3.get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&d3d12Device)));

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	DX::ThrowIfFailed(d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)));

	DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)));

	DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.get(), nullptr, IID_PPV_ARGS(&commandList)));

	InitBrixelizer();
}

void Raytracing::InitBrixelizer()
{	
	auto ffxDevice = ffxGetDeviceDX12(d3d12Device.get());

	size_t scratchBufferSize = ffxGetScratchMemorySizeDX12(2);
	void* scratchBuffer = calloc(scratchBufferSize, 1);
	memset(scratchBuffer, 0, scratchBufferSize);

	if (ffxGetInterfaceDX12(&initializationParameters.backendInterface, ffxDevice, scratchBuffer, scratchBufferSize, FFX_FSR3UPSCALER_CONTEXT_COUNT) != FFX_OK)
		logger::critical("[Raytracing] Failed to initialize Brixelizer backend interface!");

	initializationParameters.sdfCenter[0] = 0.0f;
	initializationParameters.sdfCenter[1] = 0.0f;
	initializationParameters.sdfCenter[2] = 0.0f;
	initializationParameters.flags = FFX_BRIXELIZER_CONTEXT_FLAG_ALL_DEBUG;
	initializationParameters.numCascades = 1;

	float voxelSize = 1.0f;
	for (uint32_t i = 0; i < initializationParameters.numCascades; ++i) {
		FfxBrixelizerCascadeDescription* cascadeDesc = &initializationParameters.cascadeDescs[i];
		cascadeDesc->flags = (FfxBrixelizerCascadeFlag)(FFX_BRIXELIZER_CASCADE_STATIC | FFX_BRIXELIZER_CASCADE_DYNAMIC);
		cascadeDesc->voxelSize = voxelSize;
		voxelSize *= 2.0f;
	}

	FfxErrorCode error = ffxBrixelizerContextCreate(&initializationParameters, &brixelizerContext);
	if (error != FFX_OK) {
		logger::info("error");
	}
}

// Function to check for shared NT handle support and convert to D3D12 resource
Raytracing::RenderTargetDataD3D12 Raytracing::ConvertD3D11TextureToD3D12(RE::BSGraphics::RenderTargetData* rtData)
{
	RenderTargetDataD3D12 renderTargetData{};

	if (!rtData->texture)
		return renderTargetData;

	D3D11_TEXTURE2D_DESC texDesc{};
	rtData->texture->GetDesc(&texDesc);

	if (!(texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
		return renderTargetData;

	// Query the DXGIResource1 interface to access shared NT handle
	winrt::com_ptr<IDXGIResource1> dxgiResource1;
	DX::ThrowIfFailed(rtData->texture->QueryInterface(IID_PPV_ARGS(&dxgiResource1)));

	// Create the shared NT handle
	HANDLE sharedNtHandle = nullptr;
	DX::ThrowIfFailed(dxgiResource1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedNtHandle));

	// Open the shared handle in D3D12
	winrt::com_ptr<ID3D12Resource> d3d12Resource;
	DX::ThrowIfFailed(d3d12Device->OpenSharedHandle(sharedNtHandle, IID_PPV_ARGS(&d3d12Resource)));
	CloseHandle(sharedNtHandle);  // Close the handle after opening it in D3D12

	return renderTargetData;
}

void Raytracing::OpenSharedHandles()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	for (int i = 0; i < RE::RENDER_TARGET::kTOTAL; i++) {
		renderTargetsD3D12[i] = ConvertD3D11TextureToD3D12(&renderer->GetRuntimeData().renderTargets[i]);
	}
}

uint Raytracing::GetBufferIndex(BufferData& a_bufferData)
{
	if (a_bufferData.registered)
		return a_bufferData.index;

	FfxResourceDescription resourceDescription = {};
	resourceDescription.flags = FFX_RESOURCE_FLAGS_NONE;
	resourceDescription.usage = FFX_RESOURCE_USAGE_READ_ONLY;
	resourceDescription.width = a_bufferData.width;
	resourceDescription.height = 1;
	resourceDescription.format = FFX_SURFACE_FORMAT_UNKNOWN;
	resourceDescription.depth = 0;
	resourceDescription.mipCount = 0;
	resourceDescription.type = FFX_RESOURCE_TYPE_BUFFER;

	FfxResource ffxResource = ffxGetResourceDX12(a_bufferData.buffer.get(), resourceDescription, nullptr, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);

	FfxBrixelizerBufferDescription brixelizerBufferDesc = {};
	brixelizerBufferDesc.buffer = ffxResource;
	brixelizerBufferDesc.outIndex = &a_bufferData.index;
	ffxBrixelizerRegisterBuffers(&brixelizerContext, &brixelizerBufferDesc, 1);

	a_bufferData.registered = true;

	return a_bufferData.index;
}

DirectX::XMMATRIX GetXMFromNiTransform(const RE::NiTransform& Transform)
{
	DirectX::XMMATRIX temp;

	const RE::NiMatrix3& m = Transform.rotate;
	const float scale = Transform.scale;

	temp.r[0] = DirectX::XMVectorScale(DirectX::XMVectorSet(
										   m.entry[0][0],
										   m.entry[1][0],
										   m.entry[2][0],
										   0.0f),
		scale);

	temp.r[1] = DirectX::XMVectorScale(DirectX::XMVectorSet(
										   m.entry[0][1],
										   m.entry[1][1],
										   m.entry[2][1],
										   0.0f),
		scale);

	temp.r[2] = DirectX::XMVectorScale(DirectX::XMVectorSet(
										   m.entry[0][2],
										   m.entry[1][2],
										   m.entry[2][2],
										   0.0f),
		scale);

	temp.r[3] = DirectX::XMVectorSet(
		Transform.translate.x,
		Transform.translate.y,
		Transform.translate.z,
		1.0f);

	return temp;
}

void Raytracing::UpdateGeometry(RE::BSGeometry* a_geometry)
{
	if (Deferred::GetSingleton()->inWorld) {
		auto it = geometries.find(a_geometry);
		if (it == geometries.end()) {
			geometries.insert(a_geometry);

			const auto& transform = a_geometry->world;
			const RE::NiPoint3 c = { a_geometry->worldBound.center.x, a_geometry->worldBound.center.y, a_geometry->worldBound.center.z };
			const RE::NiPoint3 r = { a_geometry->worldBound.radius, a_geometry->worldBound.radius, a_geometry->worldBound.radius };
			const RE::NiPoint3 aabbMinVec = c - r;
			const RE::NiPoint3 aabbMaxVec = c + r;
			const RE::NiPoint3 extents = aabbMaxVec - aabbMinVec;

			const RE::NiPoint3 aabbCorners[8] = {
				aabbMinVec + RE::NiPoint3(0.0f, 0.0f, 0.0f),
				aabbMinVec + RE::NiPoint3(extents.x, 0.0f, 0.0f),
				aabbMinVec + RE::NiPoint3(0.0f, 0.0f, extents.z),
				aabbMinVec + RE::NiPoint3(extents.x, 0.0f, extents.z),
				aabbMinVec + RE::NiPoint3(0.0f, extents.y, 0.0f),
				aabbMinVec + RE::NiPoint3(extents.x, extents.y, 0.0f),
				aabbMinVec + RE::NiPoint3(0.0f, extents.y, extents.z),
				aabbMinVec + RE::NiPoint3(extents.x, extents.y, extents.z),
			};

			float4 minExtents = float4(INFINITY, INFINITY, INFINITY, INFINITY);
			float4 maxExtents = float4(-INFINITY, -INFINITY, -INFINITY, -INFINITY);

			for (uint i = 0; i < 8; i++) {
				auto transformed = transform * aabbCorners[i];
				float4 transformedF = { transformed.x, transformed.y, transformed.z, 0 };

				minExtents = (float4)_mm_min_ps(minExtents, transformedF);
				maxExtents = (float4)_mm_max_ps(maxExtents, transformedF);
			}

			auto rendererData = a_geometry->GetGeometryRuntimeData().rendererData;

			if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
				return;

			BufferData* vertexBuffer;
			BufferData* indexBuffer;

			{
				auto it2 = vertexBuffers.find((ID3D11Buffer*)rendererData->vertexBuffer);
				if (it2 == vertexBuffers.end())
					return;
				vertexBuffer = &it2->second;
			}

			{
				auto it2 = indexBuffers.find((ID3D11Buffer*)rendererData->indexBuffer);
				if (it2 == indexBuffers.end())
					return;
				indexBuffer = &it2->second;
			}

			uint vertexCount = vertexBuffer->width / rendererData->vertexDesc.GetSize();
			uint triangleCount = indexBuffer->width / (sizeof(uint16_t) * 3);

			uint vertexBufferIndex = GetBufferIndex(*vertexBuffer);
			uint indexBufferIndex = GetBufferIndex(*indexBuffer);

			FfxBrixelizerInstanceDescription instanceDesc = {};

			{
				instanceDesc.aabb.min[0] = minExtents.x;
				instanceDesc.aabb.max[0] = maxExtents.x;

				instanceDesc.aabb.min[1] = minExtents.y;
				instanceDesc.aabb.max[1] = maxExtents.y;

				instanceDesc.aabb.min[2] = minExtents.z;
				instanceDesc.aabb.max[2] = maxExtents.z;
			}

			float4x4 xmmTransform = GetXMFromNiTransform(transform);

			for (uint row = 0; row < 3; ++row) {
				for (uint col = 0; col < 4; ++col) {
					instanceDesc.transform[row * 4 + col] = xmmTransform.m[col][row];
				}
			}

			instanceDesc.indexFormat = FFX_INDEX_TYPE_UINT16;
			instanceDesc.indexBuffer = indexBufferIndex;
			instanceDesc.indexBufferOffset = 0;
			instanceDesc.triangleCount = triangleCount;

			instanceDesc.vertexBuffer = vertexBufferIndex;
			instanceDesc.vertexStride = rendererData->vertexDesc.GetSize();
			instanceDesc.vertexBufferOffset = 0;
			instanceDesc.vertexCount = vertexCount;
			instanceDesc.vertexFormat = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;

			uint outInstanceID;
			instanceDesc.outInstanceID = &outInstanceID;
			instanceDesc.flags = FFX_BRIXELIZER_INSTANCE_FLAG_NONE;

			// Create instances for a given context with the ffxBrixelizerCreateInstance function.
			// Static instances are persistent across frames. Dynamic instances are discarded after a single frame.
			FfxErrorCode error = ffxBrixelizerCreateInstances(&brixelizerContext, &instanceDesc, 1);
			if (error != FFX_OK)
				logger::error("error");
		}
	}
}

void Raytracing::RemoveGeometry(RE::BSGeometry* a_geometry)
{
	if (geometries.erase(a_geometry)) {
		// Remove Event
	}
}

Raytracing::BufferData Raytracing::AllocateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData)
{
	BufferData data;

	// Allocate the memory on the CPU and GPU
	{
		D3D12_HEAP_PROPERTIES heapProperties = {};
		heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC bufferDesc = {};
		bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufferDesc.Width = pDesc->ByteWidth;
		bufferDesc.Height = 1;
		bufferDesc.DepthOrArraySize = 1;
		bufferDesc.MipLevels = 1;
		bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufferDesc.SampleDesc.Count = 1;
		bufferDesc.SampleDesc.Quality = 0;
		bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		DX::ThrowIfFailed(d3d12Device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&bufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&data.buffer)));

		data.width = pDesc->ByteWidth;
	}

	// Map the buffer to CPU memory and copy the vertex data
	{
		void* pVertexData = nullptr;
		data.buffer->Map(0, nullptr, &pVertexData);
		memcpy(pVertexData, pInitialData->pSysMem, pDesc->ByteWidth);
		data.buffer->Unmap(0, nullptr);
	}

	return data;
}

void Raytracing::RegisterVertexBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer)
{
	BufferData data = AllocateBuffer(pDesc, pInitialData);
	vertexBuffers.insert({ *ppBuffer, data });
}

void Raytracing::RegisterIndexBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer)
{
	BufferData data = AllocateBuffer(pDesc, pInitialData);
	indexBuffers.insert({ *ppBuffer, data });
}
