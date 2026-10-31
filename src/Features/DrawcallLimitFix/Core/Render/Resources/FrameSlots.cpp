// FrameSlots.h's functions: the main pass's bindings captured (render thread), the frame textures it binds at t16+, and FrameData's
// names of the frame-constants buffer's parts.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

namespace DCLF::Draws
{
	std::shared_ptr<const FrameBlockViews> FrameBlockViews::Create(rhi::Device a_device, const std::shared_ptr<org::Buffer>& a_frameConstants)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto service = host && host->Graph() ? host->Graph()->RetainDescriptorService() : nullptr;
		if (!service || !a_frameConstants) {
			logger::error("[DCLF] frame block views: no descriptor service");
			return nullptr;
		}
		auto views = std::make_shared<FrameBlockViews>();
		views->service = std::move(service);
		views->buffer = a_frameConstants;
		const auto resource = a_frameConstants->GetAPIResource().GetHandle();
		bool ok = true;
		ForEachFrameBlock([&](bool a_pixel, std::uint32_t a_register, std::uint32_t a_block) {
			if (!ok)
				return;
			const rhi::DescriptorSlot slot = views->service->AllocateDescriptorSlot(rhi::DescriptorHeapType::CbvSrvUav, true);
			views->slots[a_block] = slot;
			views->made = a_block + 1;
			const rhi::CbvDesc desc{ .byteOffset = FrameBlockOffset(a_pixel, a_register), .byteSize = static_cast<std::uint32_t>(kFrameSlotBytes) };
			if (!slot.heap.valid() || a_device.CreateConstantBufferView(slot, resource, desc) != rhi::Result::Ok) {
				logger::error("[DCLF] frame block views: the view of {} b{} could not be made", a_pixel ? "PS" : "VS", a_register);
				ok = false;
				return;
			}
			views->indices[a_block] = slot.index;
		});
		return ok ? std::shared_ptr<const FrameBlockViews>(std::move(views)) : nullptr;
	}

	FrameBlockViews::~FrameBlockViews()
	{
		for (std::uint32_t b = 0; b < made; ++b)
			if (slots[b].heap.valid())
				service->RetireDescriptorSlotWithOwner(slots[b], buffer);
	}

	std::string FrameConstantsPart(std::uint64_t a_offset)
	{
		const auto slot = static_cast<std::uint32_t>(a_offset / kFrameSlotBytes);
		const std::uint64_t within = a_offset % kFrameSlotBytes;
		if (slot == kFrameFogRegister)
			return within < sizeof(FrameFog) ? "frame constants VS b13 (fog)" : "frame constants VS b13 (extras frame)";
		if (slot < kConstantBufferRegisters)
			return fmt::format("frame constants VS b{}", slot);
		if (slot < 2 * kConstantBufferRegisters)
			return fmt::format("frame constants PS b{}", slot - kConstantBufferRegisters);
		if (slot == kFrameSlotSharedLight)
			return "frame constants: zeroed light block";
		if (slot == kFrameSlotLighting)
			return within < sizeof(FrameLighting)                                   ? "frame constants: frame lighting" :
			       within < sizeof(FrameLighting) + sizeof(LodFadeFrame)           ? "frame constants: LOD fade frame" :
			       within < kExtrasPixelFrameOffset % kFrameSlotBytes               ? "frame constants: foliage parity" :
			                                                                         "frame constants PS b13 (extras frame)";
		if (slot == kFrameSlotRecord)
			return "frame constants: frame record";
		return "frame constants: ?";
	}

	Capture CaptureBindings()
	{
		RenderThreadBudget::Part budget(RenderThreadBudget::Bucket::Capture);
		auto* context = globals::d3d::context;
		Capture capture;
		context->VSGetConstantBuffers(0, kConstantBufferRegisters, capture.vsBuffers.data());
		context->PSGetConstantBuffers(0, kConstantBufferRegisters, capture.psBuffers.data());
		context->PSGetShaderResources(0, kTextureRegisters, capture.psViews.data());
		ID3D11RenderTargetView* views[kColorTargets] = {};
		ID3D11DepthStencilView* depth = nullptr;
		context->OMGetRenderTargets(kColorTargets, views, &depth);
		for (std::uint32_t i = 0; i < kColorTargets; ++i) {
			if (!views[i])
				continue;
			winrt::com_ptr<ID3D11Resource> resource;
			views[i]->GetResource(resource.put());
			capture.targets[i] = resource.try_as<ID3D11Texture2D>();
			capture.targetCount = i + 1;
			views[i]->Release();
		}
		if (depth) {
			winrt::com_ptr<ID3D11Resource> resource;
			depth->GetResource(resource.put());
			capture.depth = resource.try_as<ID3D11Texture2D>();
			depth->Release();
		}
		// Community Shaders binds its own per-frame pixel buffers - b5 SharedData and b6 FeatureData - from Renderer_ResetState, so
		// whether they are bound when this capture is taken depends on where the engine last reset its state (which the withheld
		// native draws change). They are CS's own buffers with known identities: taken from CS when the capture lacks them.
		if (auto* state = globals::state) {
			auto adopt = [&](std::uint32_t a_slot, ConstantBuffer* a_buffer) {
				if (capture.psBuffers[a_slot] || !a_buffer || !a_buffer->CB())
					return;
				capture.psBuffers[a_slot] = a_buffer->CB();
				capture.psBuffers[a_slot]->AddRef();  // Capture::Release owns what it holds
			};
			adopt(kSharedDataRegister, state->sharedDataCB.get());
			adopt(kFeatureDataRegister, state->featureDataCB.get());
		}
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		capture.eye = shadowState.posAdjust.getEye();
		capture.previousEye = shadowState.previousPosAdjust.getEye();
		D3D11_VIEWPORT viewport{};
		UINT count = 1;
		context->RSGetViewports(&count, &viewport);
		capture.viewportWidth = static_cast<std::uint32_t>(viewport.Width);
		capture.viewportHeight = static_cast<std::uint32_t>(viewport.Height);
		capture.minDepth = viewport.MinDepth;
		capture.maxDepth = viewport.MaxDepth;
		return capture;
	}

	namespace
	{
		/** @brief Whether a_register is one of the frame graph buffers' in a_graphBuffers (a bit per registration). */
		bool GraphBufferHeld(std::uint32_t a_graphBuffers, std::uint32_t a_register)
		{
			const auto registered = Extensions::RegisteredFrameGraphBuffers();
			for (std::size_t b = 0; b < registered.size(); ++b)
				if (((a_graphBuffers >> b) & 1) && a_register >= registered[b].firstRegister && a_register - registered[b].firstRegister < registered[b].count)
					return true;
			return false;
		}
	}

	std::vector<FrameBuffer> FrameBuffersOf(const Capture& a_capture, std::uint32_t a_graphBuffers)
	{
		std::vector<FrameBuffer> result;
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
			auto* view = a_capture.psViews[t];
			if (!view || (a_graphBuffers && GraphBufferHeld(a_graphBuffers, t)))
				continue;
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			view->GetDesc(&viewDesc);
			if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && viewDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX)
				continue;
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			auto buffer = resource.try_as<ID3D11Buffer>();
			D3D11_BUFFER_DESC bufferDesc{};
			buffer->GetDesc(&bufferDesc);
			if (!(bufferDesc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) || !bufferDesc.StructureByteStride)
				continue;  // only CPU-written structured buffers are copied
			FrameBuffer frameBuffer;
			frameBuffer.textureRegister = t;
			frameBuffer.stride = bufferDesc.StructureByteStride;
			frameBuffer.firstElement = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.FirstElement : viewDesc.BufferEx.FirstElement;
			frameBuffer.elements = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.NumElements : viewDesc.BufferEx.NumElements;
			result.push_back(frameBuffer);
		}
		return result;
	}
}
