// End-to-end check of the render-graph-on-DXVK path without the game.
//
// Loads the DXVK fork (dxvk_dxgi.dll + dxvk_d3d11.dll), requests the interop device
// features, creates a D3D11 device, and then does what RenderGraphRuntime does in the
// plugin: BasicRHI adopts DXVK's VkDevice and an OpenRenderGraph PersistentGraphHost
// runs a compute pass on it. The pass writes a graph-owned buffer that D3D11 also writes
// (a sentinel, before the epoch) and reads (a copy, after it) through
// dxvkCreateBufferFromVkBuffer, so a correct readback proves the handoff in both directions.
//
// Two submission modes, as in the plugin:
//   lock   - each epoch flushes DXVK and waits for it, then submits under DXVK's queue lock;
//   stream - no flush: the graph's batches go into DXVK's command stream
//            (dxvkEnqueueInteropSubmission) between the D3D11 commands around the epoch.
// Finally the D3D11 device is released and DXVK's teardown callback must fire before
// its VkDevice is destroyed.
//
// Usage: OrgDxvkInteropTest <directory containing dxvk_d3d11.dll and dxvk_dxgi.dll> [lock|stream]

#include <rhi_interop_vulkan.h>

#include "Features/Upscaling/DXVKInteropInterfaces.h"
#include "RenderGraph/DxvkOrgInterop.h"

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef ORG_DXVK_TEST_SPV
#	error "ORG_DXVK_TEST_SPV must name the compiled SPIR-V of WriteValues.hlsl"
#endif

using Microsoft::WRL::ComPtr;

#define REQUIRE(cond, msg)                                                              \
	do {                                                                                \
		if (!(cond)) {                                                                  \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);         \
			return 1;                                                                   \
		}                                                                               \
	} while (0)

namespace
{
	constexpr uint32_t kCount = 256;
	constexpr uint64_t kBytes = kCount * sizeof(uint32_t);

	struct Program
	{
		rhi::PipelineLayoutPtr layout;
		rhi::PipelinePtr pipeline;
	};

	struct WriteBindings
	{
		org::ResourceBindingToken target;
	};

	struct WriteFrame
	{
		std::shared_ptr<const Program> program;
		uint32_t targetIndex = 0;
		uint32_t value = 0;
	};

	class WritePass final : public org::TypedRenderGraphPass<WritePass, WriteFrame, WriteBindings>
	{
	public:
		WritePass(std::shared_ptr<org::Buffer> target, std::shared_ptr<const Program> program, const uint32_t* value) :
			target(std::move(target)), program(std::move(program)), value(value) {}
		WriteBindings Declare(org::PassBuilder& builder)
		{
			builder.PreferQueue(org::QueueKind::Graphics);
			return { builder.BindUnorderedAccess(target) };
		}
		WriteFrame Prepare(const WriteBindings& bindings, const org::PassPrepareContext& preparation) const
		{
			return { program, preparation.ResolveView(bindings.target, { org::BindlessViewKind::UnorderedAccess }).index, *value };
		}
		static void Record(const WriteBindings&, const WriteFrame& frame, org::PassRecordContext& recording)
		{
			auto& commands = recording.Commands();
			commands.BindLayout(frame.program->layout->GetHandle());
			commands.BindPipeline(frame.program->pipeline->GetHandle());
			const uint32_t constants[3] = { frame.targetIndex, frame.value, kCount };
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, 3, constants);
			commands.Dispatch((kCount + 63) / 64, 1, 1);
		}

	private:
		std::shared_ptr<org::Buffer> target;
		std::shared_ptr<const Program> program;
		const uint32_t* value;
	};

	class WriteExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		WriteExtension(std::shared_ptr<org::Buffer> target, std::shared_ptr<const Program> program, const uint32_t* value) :
			target(std::move(target)), program(std::move(program)), value(value) {}
		void PrepareForBuild(org::RenderGraph& graph) override
		{
			graph.RegisterResource(org::ResourceIdentifier("test.values"), target);
		}
		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& out) override
		{
			out.push_back(org::RenderGraph::ExternalPassDesc::Compute("test.write-values",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<WritePass>(target, program, value)))
					.PreferQueue(org::QueueKind::Graphics));
		}

	private:
		std::shared_ptr<org::Buffer> target;
		std::shared_ptr<const Program> program;
		const uint32_t* value;
	};

	std::shared_ptr<const Program> LoadProgram(rhi::Device device)
	{
		std::ifstream file(ORG_DXVK_TEST_SPV, std::ios::binary);
		std::vector<char> spirv((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (spirv.empty())
			return {};
		auto program = std::make_shared<Program>();
		rhi::PushConstantRangeDesc constants{};
		constants.visibility = rhi::ShaderStage::Compute;
		constants.num32BitValues = 3;
		if (device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PipelineLayoutFlags::PF_None },
				program->layout) != rhi::Result::Ok)
			return {};
		rhi::SubobjLayout layout{ program->layout->GetHandle() };
		rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { spirv.data(), static_cast<uint32_t>(spirv.size()) }, "main" };
		const rhi::PipelineStreamItem items[] = { rhi::Make(layout), rhi::Make(shader) };
		if (device.CreatePipeline(items, 2, program->pipeline) != rhi::Result::Ok)
			return {};
		return program;
	}

	struct Locking
	{
		IDXGIVkInteropDevice* interop = nullptr;
		int locks = 0;
		int depth = 0;
		bool unbalanced = false;
	};

	void LockQueue(void* user, VkQueue)
	{
		auto* locking = static_cast<Locking*>(user);
		locking->interop->LockSubmissionQueue();
		++locking->locks;
		if (++locking->depth != 1)
			locking->unbalanced = true;
	}

	void UnlockQueue(void* user, VkQueue)
	{
		auto* locking = static_cast<Locking*>(user);
		if (--locking->depth != 0)
			locking->unbalanced = true;
		locking->interop->ReleaseSubmissionQueue();
	}

	struct Streaming
	{
		ID3D11Device* device = nullptr;
		PFN_dxvkEnqueueInteropSubmission enqueue = nullptr;
		std::atomic<int> enqueued = 0;
		std::atomic<int> submitted = 0;
		std::atomic<int> failed = 0;
	};

	void OnStreamSubmitted(void* user, VkResult result)
	{
		auto* streaming = static_cast<Streaming*>(user);
		if (result != VK_SUCCESS)
			++streaming->failed;
		++streaming->submitted;
	}

	// What RenderGraphRuntime's submit hook does on the D3D11 thread.
	VkResult SubmitToStream(void* user, VkQueue, const VkSubmitInfo2& submit)
	{
		auto* streaming = static_cast<Streaming*>(user);
		DxvkOrgInteropSubmission submission{};
		submission.version = DXVK_ORG_INTEROP_VERSION;
		submission.waitCount = submit.waitSemaphoreInfoCount;
		submission.waits = submit.pWaitSemaphoreInfos;
		submission.commandBufferCount = submit.commandBufferInfoCount;
		submission.commandBuffers = submit.pCommandBufferInfos;
		submission.signalCount = submit.signalSemaphoreInfoCount;
		submission.signals = submit.pSignalSemaphoreInfos;
		submission.onSubmitted = &OnStreamSubmitted;
		submission.user = streaming;
		++streaming->enqueued;
		return SUCCEEDED(streaming->enqueue(streaming->device, &submission)) ? VK_SUCCESS : VK_ERROR_UNKNOWN;
	}

	struct Teardown
	{
		std::unique_ptr<org::PersistentGraphHost>* host = nullptr;
		rhi::DevicePtr* device = nullptr;
		bool fired = false;
	};

	void OnTeardown(void* user, VkDevice)
	{
		// What RenderGraphRuntime::Shutdown does when DXVK destroys its device.
		auto* teardown = static_cast<Teardown*>(user);
		teardown->fired = true;
		teardown->host->reset();
		teardown->device->Reset();
	}

	template <class T>
	T Resolve(HMODULE module, const char* name)
	{
		return reinterpret_cast<T>(reinterpret_cast<void*>(::GetProcAddress(module, name)));
	}
}

int main(int argc, char** argv)
{
	REQUIRE(argc >= 2, "usage: OrgDxvkInteropTest <dxvk build dir> [lock|stream]");
	const std::filesystem::path dir = argv[1];
	const bool stream = argc >= 3 && std::strcmp(argv[2], "stream") == 0;

	HMODULE dxgi = ::LoadLibraryExW((dir / L"dxvk_dxgi.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	HMODULE d3d11 = ::LoadLibraryExW((dir / L"dxvk_d3d11.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	REQUIRE(dxgi && d3d11, "load the DXVK DLLs");

	auto requestFeatures = Resolve<PFN_dxvkRequestDeviceFeatures>(d3d11, "dxvkRequestDeviceFeatures");
	auto getInfo = Resolve<PFN_dxvkGetInteropDeviceInfo>(d3d11, "dxvkGetInteropDeviceInfo");
	auto wrapBuffer = Resolve<PFN_dxvkCreateBufferFromVkBuffer>(d3d11, "dxvkCreateBufferFromVkBuffer");
	auto setTeardown = Resolve<PFN_dxvkSetDeviceTeardownCallback>(d3d11, "dxvkSetDeviceTeardownCallback");
	auto enqueue = Resolve<PFN_dxvkEnqueueInteropSubmission>(d3d11, "dxvkEnqueueInteropSubmission");
	auto createDevice = Resolve<decltype(&D3D11CreateDevice)>(d3d11, "D3D11CreateDevice");
	REQUIRE(requestFeatures && getInfo && wrapBuffer && setTeardown && enqueue && createDevice, "DXVK interop exports");

	const DxvkOrgInteropFeature features[] = {
		{ VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, "descriptorHeap" },
		{ nullptr, "vulkanMemoryModelDeviceScope" },
	};
	const DxvkOrgInteropFeatureRequest request{ DXVK_ORG_INTEROP_VERSION, static_cast<uint32_t>(std::size(features)), features };
	REQUIRE(SUCCEEDED(requestFeatures(&request)), "dxvkRequestDeviceFeatures");

	ComPtr<ID3D11Device> d3dDevice;
	ComPtr<ID3D11DeviceContext> context;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
	REQUIRE(SUCCEEDED(createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &d3dDevice, nullptr, &context)),
		"D3D11CreateDevice through DXVK");

	ComPtr<IDXGIVkInteropDevice> interop;
	REQUIRE(SUCCEEDED(d3dDevice.As(&interop)), "IDXGIVkInteropDevice");
	DxvkOrgInteropDeviceInfo info{};
	info.version = DXVK_ORG_INTEROP_VERSION;
	REQUIRE(SUCCEEDED(getInfo(d3dDevice.Get(), &info)), "dxvkGetInteropDeviceInfo");
	bool hasHeap = false;
	for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
		hasHeap |= std::strcmp(info.enabledExtensions[i], VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME) == 0;
	if (!hasHeap) {
		std::puts("SKIP: the device does not support VK_EXT_descriptor_heap");
		return 0;
	}
	REQUIRE(info.grantedFeatureCount == std::size(features) && info.deniedFeatureCount == 0, "interop features granted");

	Locking locking{ interop.Get() };
	rhi::vulkan::AdoptedVulkanDeviceInfo adopt{};
	adopt.getInstanceProcAddr = info.getInstanceProcAddr;
	adopt.instance = info.instance;
	adopt.instanceApiVersion = info.instanceApiVersion;
	adopt.physicalDevice = info.physicalDevice;
	adopt.device = info.device;
	adopt.enabledDeviceExtensions = info.enabledExtensions;
	adopt.enabledDeviceExtensionCount = info.enabledExtensionCount;
	adopt.enabledFeatureChain = info.enabledFeatures;
	adopt.queues[0] = { info.graphicsQueue, info.graphicsQueueFamily, info.graphicsQueueIndex };
	Streaming streaming{ d3dDevice.Get(), enqueue };
	if (stream)
		adopt.submissionHooks = { &streaming, nullptr, nullptr, &SubmitToStream };
	else
		adopt.submissionHooks = { &locking, &LockQueue, &UnlockQueue };
	rhi::DevicePtr device;
	REQUIRE(rhi::vulkan::AdoptVulkanDevice(adopt, device) == rhi::Result::Ok, "AdoptVulkanDevice");

	std::unique_ptr<org::PersistentGraphHost> host = std::make_unique<org::PersistentGraphHost>(org::PersistentGraphHost::Desc{
		.device = device.Get(), .backend = rhi::Backend::Vulkan, .queueBoundary = { .entry = true, .exit = true } });
	Teardown teardown{ &host, &device };
	REQUIRE(SUCCEEDED(setTeardown(&OnTeardown, &teardown)), "dxvkSetDeviceTeardownCallback");

	uint32_t value = 0;
	{
		auto program = LoadProgram(device.Get());
		REQUIRE(program, "compute program");
		auto values = org::Buffer::CreateUnmaterializedStructuredBuffer(kCount, sizeof(uint32_t), true);
		values->SetName("test.values");
		values->Materialize();
		host->AddExtension("test.write", [&] { return std::make_unique<WriteExtension>(values, program, &value); });

		// D3D11 sees the graph-owned buffer through a DXVK wrapper.
		rhi::VulkanResourceInfo resourceInfo{};
		REQUIRE(rhi::vulkan::get_resource_info(values->GetAPIResource(), resourceInfo), "graph buffer handle");
		D3D11_BUFFER_DESC wrapDesc{};
		wrapDesc.ByteWidth = kBytes;
		wrapDesc.Usage = D3D11_USAGE_DEFAULT;
		wrapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		wrapDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		wrapDesc.StructureByteStride = sizeof(uint32_t);
		ComPtr<ID3D11Buffer> wrapped;
		REQUIRE(SUCCEEDED(wrapBuffer(d3dDevice.Get(), &wrapDesc, rhi::vulkan::from_native_void<VkBuffer>(resourceInfo.resource), &wrapped)),
			"dxvkCreateBufferFromVkBuffer");
		D3D11_BUFFER_DESC dynamicDesc = wrapDesc;
		dynamicDesc.Usage = D3D11_USAGE_DYNAMIC;
		dynamicDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ComPtr<ID3D11Buffer> rejected;
		REQUIRE(FAILED(wrapBuffer(d3dDevice.Get(), &dynamicDesc, rhi::vulkan::from_native_void<VkBuffer>(resourceInfo.resource), &rejected)),
			"DXVK refuses to wrap an external buffer it would have to rename");

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = kBytes;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Buffer> staging;
		REQUIRE(SUCCEEDED(d3dDevice->CreateBuffer(&stagingDesc, nullptr, &staging)), "staging buffer");

		const std::vector<uint32_t> sentinel(kCount, 0xDEADBEEFu);
		for (uint32_t frame = 0; frame < 8; ++frame) {
			value = 5000u + frame * 100u;
			// D3D11 writes the buffer before the epoch: the graph must run after this.
			context->UpdateSubresource(wrapped.Get(), 0, nullptr, sentinel.data(), 0, 0);
			// RenderGraphRuntime::ExecuteEpoch.
			if (!stream)
				interop->FlushRenderingCommands();
			host->ExecuteFrame();
			// Recorded after the epoch on the same queue: must observe this frame's values.
			context->CopyResource(staging.Get(), wrapped.Get());
			D3D11_MAPPED_SUBRESOURCE mapped{};
			REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map staging");
			const auto* words = static_cast<const uint32_t*>(mapped.pData);
			bool match = true;
			for (uint32_t i = 0; i < kCount && match; ++i)
				match = words[i] == value + i;
			const uint32_t first = words[0];
			context->Unmap(staging.Get(), 0);
			if (!match) {
				std::fprintf(stderr, "FAIL: frame %u read %u, expected %u\n", frame, first, value);
				return 1;
			}
		}
		if (stream) {
			REQUIRE(streaming.enqueued >= 8 && streaming.failed == 0, "graph submissions through DXVK's stream");
			REQUIRE(locking.locks == 0, "no locked submissions in stream mode");
		} else {
			REQUIRE(locking.locks >= 8 && !locking.unbalanced && locking.depth == 0, "graph submissions under DXVK's lock");
		}
	}

	// DXVK tears its device down with the last D3D11 reference; the callback retires ORG first.
	context.Reset();
	interop.Reset();
	d3dDevice.Reset();
	REQUIRE(teardown.fired, "DXVK teardown callback fired");
	REQUIRE(!host && !device, "render graph retired before the VkDevice");

	if (stream)
		REQUIRE(streaming.submitted == streaming.enqueued, "every streamed submission reached the queue");
	std::printf("OrgDxvkInteropTest: ok (%s: %d locked, %d streamed submissions)\n", stream ? "stream" : "lock", locking.locks,
		streaming.submitted.load());
	return 0;
}
