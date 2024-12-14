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
