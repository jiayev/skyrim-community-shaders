#pragma once

#include <winrt/base.h>

#include <d3d11_4.h>
#include <directx/d3dx12.h>

#include "DX12Interop/WrappedResource.h"

#include "Feature.h"

#define NTDDI_VERSION NTDDI_WINBLUE

#include <DXProgrammableCapture.h>

#include "State.h"

struct DX12Interop : public Feature
{
	~DX12Interop();

	virtual inline std::string GetName() override { return "DirectX 12 Interoperability"; }
	virtual inline std::string GetShortName() override { return "DX12Interop"; }
	virtual bool IsCore() const override { return true; }

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	// Resources
	virtual void SetupResources() override;

	struct Settings
	{
		bool EnablePIXCapture = false;

		bool EnableDebugDevice = false;
		bool DebugBreakCorruption = true;
		bool DebugBreakError = true;
		bool DebugBreakWarning = false;
	} settings;

	winrt::com_ptr<ID3D12Device5> d3d12Device;

	winrt::com_ptr<ID3D12CommandQueue> commandQueue;
	winrt::com_ptr<ID3D12CommandQueue> computeCommandQueue;
	winrt::com_ptr<ID3D12CommandQueue> copyCommandQueue;

	struct FrameContext
	{
		winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> commandList;
		UINT64 fenceValueAtSubmission = 0;  // what value was signaled when this frame was submitted
		UINT lastUsedFrame = UINT_MAX;
	};

	static constexpr UINT kMaxFramesInFlight = 2;
	FrameContext frameContexts[kMaxFramesInFlight];
	UINT64 currentFenceValue = 0;
	HANDLE fenceEvent = nullptr;

	struct SharedResources
	{
		WrappedResource* main = nullptr;
		WrappedResource* depth = nullptr;
		WrappedResource* motionVector = nullptr;
		WrappedResource* reactiveMask = nullptr;
	} sharedResources;

	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;

	winrt::com_ptr<ID3D11Fence> d3d11Fence;
	winrt::com_ptr<ID3D12Fence> d3d12Fence;

	winrt::com_ptr<IDXGraphicsAnalysis> ga = nullptr;

	bool pixCapture = false;
	bool pixCaptureStarted = false;

	bool active = false;

	void Init(ID3D11Device* d3d11Device, ID3D11DeviceContext* a_immediateContext, IDXGIAdapter* a_adapter);

	// Fences the GPU, waits for D3D11 to be idle, executes the provided function, then waits for D3D12 to be idle before returning.
	template <typename Func>
	void Fence(Func func)
	{
		// Wait for D3D11 to finish
		DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), ++currentFenceValue));
		DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), currentFenceValue));

		// Execute
		func();

		// Wait for D3D12 to finish
		DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), ++currentFenceValue));
		DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), currentFenceValue));

		frameContexts[GetFrameContextIndex()].fenceValueAtSubmission = currentFenceValue;
	}

	// Executes D3D12 commands mid D3D11 execution, probably huge overhead from wait commands so use sparsely and wisely
	template <typename Func, typename Func2 = std::nullptr_t>
	void Execute(Func func, Func2 func2 = nullptr)
	{
		// Get to next frame context
		FrameContext& ctx = frameContexts[GetFrameContextIndex()];
		const UINT frameCount = globals::state->frameCount;

		// Reset/wait once per frame-context per frame to allow multiple Execute calls in the same frame.
		if (ctx.lastUsedFrame != frameCount) {
			ctx.lastUsedFrame = frameCount;

			// CPU-side wait: stall if this slot's previous submission isn't done yet
			// (i.e. we've lapped the GPU)
			if (ctx.fenceValueAtSubmission != 0) {
				if (d3d12Fence->GetCompletedValue() < ctx.fenceValueAtSubmission) {
					// GPU hasn't finished with this allocator yet - stall CPU
					DX::ThrowIfFailed(d3d12Fence->SetEventOnCompletion(ctx.fenceValueAtSubmission, fenceEvent));
					WaitForSingleObject(fenceEvent, INFINITE);
				}
			}

			// Safe to reset now - GPU is done with this allocator
			// Reset allocator only on first use this frame
			DX::ThrowIfFailed(ctx.commandAllocator->Reset());
		}

		DX::ThrowIfFailed(ctx.commandList->Reset(ctx.commandAllocator.get(), nullptr));

		// DX11 -> DX12 handoff
		DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), ++currentFenceValue));
		DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), currentFenceValue));

		func(ctx.commandList.get());
		DX::ThrowIfFailed(ctx.commandList->Close());

		ID3D12CommandList* lists[] = { ctx.commandList.get() };
		commandQueue->ExecuteCommandLists(1, lists);

		if constexpr (!std::is_same_v<Func2, std::nullptr_t>)
			func2();

		// DX12 -> DX11 handoff
		DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), ++currentFenceValue));
		DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), currentFenceValue));

		// Record what fence value this frame context was submitted at
		ctx.fenceValueAtSubmission = currentFenceValue;
	}

	bool Active() const;
	
	// Whether DirectX 12 is required or not
	// True when Raytracing is loaded or Upscaling is loaded in frame generation mode
	static bool D3D12Mode();

private:
	void CreateInterop();
	void SetD3D11Device(ID3D11Device* a_d3d11Device);
	void SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context);
	void InitializePIX();
	void CreateD3D12Device(IDXGIAdapter* a_adapter);
	UINT GetFrameContextIndex() const;
};
