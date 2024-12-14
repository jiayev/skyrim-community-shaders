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

inline const float4 MinPerElem(const float4& vec0, const float4& vec1)
{
	return float4(_mm_min_ps(vec0, vec1));
}

inline const float4 minPerElem(const float4& vec0, const float4& vec1)
{
	return float4(_mm_max_ps(vec0, vec1));
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

			for (uint32_t i = 0; i < 8; i++) {
				auto transformed = transform * aabbCorners[i];
				float3 transformedF = { transformed.x, transformed.y, transformed.z };

				minExtents = (float4)_mm_min_ps(minExtents, transformedF);
				maxExtents = (float4)_mm_min_ps(maxExtents, transformedF);
			}

			auto& rendererData = a_geometry->GetGeometryRuntimeData().rendererData;

			auto rawIndexData = rendererData->rawIndexData;
			auto rawVertexData = rendererData->rawVertexData;

			// TODO : Might be 16bit not 32bit

			eastl::vector<float3> vertexData;
		
			uint vertexCount = 0;
			uint triangleCount = 0;

			if (auto triShape = a_geometry->AsTriShape()) {
				vertexCount = triShape->GetTrishapeRuntimeData().vertexCount;
				triangleCount = triShape->GetTrishapeRuntimeData().triangleCount;
			}

			for (int v = 0; v < vertexCount; v++) {
				uint32_t vertexSize = rendererData->vertexDesc.GetSize();
				uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
				if (float3* vertex = reinterpret_cast<float3*>(&rendererData->rawVertexData[vertexSize * v + offset])) {
					vertexData.push_back(*vertex);
				}
			}
			
			BufferData bufferData{};

			{
				D3D12_HEAP_PROPERTIES heapProps = {};
				heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
				heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
				heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

				D3D12_RESOURCE_DESC bufferDesc = {};
				bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				bufferDesc.Width = sizeof(float3) * vertexData.size();
				bufferDesc.Height = 1;
				bufferDesc.DepthOrArraySize = 1;
				bufferDesc.MipLevels = 1;
				bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
				bufferDesc.SampleDesc.Count = 1;
				bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

				d3d12Device->CreateCommittedResource(
					&heapProps,
					D3D12_HEAP_FLAG_NONE,
					&bufferDesc,
					D3D12_RESOURCE_STATE_COPY_DEST,
					nullptr,
					IID_PPV_ARGS(&bufferData.vertexBuffer));
			}

			{
				ID3D12Resource* indexBuffer = nullptr;

				D3D12_HEAP_PROPERTIES heapProps = {};
				heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
				heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
				heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

				D3D12_RESOURCE_DESC bufferDesc = {};
				bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				bufferDesc.Width = triangleCount * sizeof(uint16_t) * 3;
				bufferDesc.Height = 1;
				bufferDesc.DepthOrArraySize = 1;
				bufferDesc.MipLevels = 1;
				bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
				bufferDesc.SampleDesc.Count = 1;
				bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

				d3d12Device->CreateCommittedResource(
					&heapProps,
					D3D12_HEAP_FLAG_NONE,
					&bufferDesc,
					D3D12_RESOURCE_STATE_COPY_DEST,
					nullptr,
					IID_PPV_ARGS(&indexBuffer));
			}

			Buffer* vertexBuffer = vertexBufferInfo->pBuffer;
			Buffer* indexBuffer = indexBufferInfo.pBuffer;

			uint32_t vertexBufferIndex = GetBufferIndex(vertexBuffer);
			uint32_t indexBufferIndex = GetBufferIndex(indexBuffer);

			FfxBrixelizerInstanceDescription instanceDesc = {};

			for (uint32_t i = 0; i < 3; ++i) {
				instanceDesc.aabb.min[i] = minExtents[i];
				instanceDesc.aabb.max[i] = maxExtents[i];
			}

			for (uint32_t row = 0; row < 3; ++row) {
				for (uint32_t col = 0; col < 4; ++col) {
					instanceDesc.transform[row * 4 + col] = transform.getCol(col)[row];
				}
			}

			instanceDesc.indexFormat = FFX_INDEX_TYPE_UINT16;
			instanceDesc.indexBuffer = indexBufferIndex;
			instanceDesc.indexBufferOffset = 0;
			instanceDesc.triangleCount = triangleCount;

			instanceDesc.vertexBuffer = vertexBufferIndex;
			instanceDesc.vertexStride = sizeof(float3);
			instanceDesc.vertexBufferOffset = 0;
			instanceDesc.vertexCount = vertexCount;
			instanceDesc.vertexFormat = FFX_SURFACE_FORMAT_R16G16B16_FLOAT;

			instanceDesc.outInstanceID = &info->instanceID;
			instanceDesc.flags = info->isDynamic ? FFX_BRIXELIZER_INSTANCE_FLAG_DYNAMIC : FFX_BRIXELIZER_INSTANCE_FLAG_NONE;
		}
	}
}

void Raytracing::RemoveGeometry(RE::BSGeometry* a_geometry)
{
	if (geometries.erase(a_geometry))
	{
		// Remove Event
	}
}
