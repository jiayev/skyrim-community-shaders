#include "Raytracing.h"

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

	if (!(texDesc.MiscFlags& D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
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

	for (int i = 0; i < RE::RENDER_TARGET::kTOTAL; i++)
	{
		renderTargetsD3D12[i] = ConvertD3D11TextureToD3D12(&renderer->GetRuntimeData().renderTargets[i]);
	}
}