#pragma once

#include <d3d12.h>

#include <FidelityFX/host/backends/dx12/ffx_dx12.h>

#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/ffx_interface.h>

#include "Buffer.h"
#include "State.h"

class Raytracing
{
public:
	static Raytracing* GetSingleton()
	{
		static Raytracing singleton;
		return &singleton;
	}

	winrt::com_ptr<IDXGIAdapter3> dxgiAdapter3;
	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;
	winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList;

	void InitD3D12();

	void OpenSharedHandles();

	struct RenderTargetDataD3D12
	{
		// D3D12 Resource
		winrt::com_ptr<ID3D12Resource> d3d12Resource;
	};

	RenderTargetDataD3D12 renderTargetsD3D12[RE::RENDER_TARGET::kTOTAL];
	RenderTargetDataD3D12 ConvertD3D11TextureToD3D12(RE::BSGraphics::RenderTargetData* rtData);
};
