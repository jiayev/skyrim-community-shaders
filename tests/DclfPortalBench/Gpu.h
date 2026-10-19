#pragma once

// The bench's GPU: a BasicRHI Vulkan device of its own (no game, DXVK or render graph), the production shaders compiled at build time
// (CMakeLists.txt), buffers with bindless views in one shader-visible heap, and timestamp pairs around each repetition.

#include <rhi.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace Bench
{
	inline void Check(rhi::Result a_result, const char* a_what)
	{
		if (a_result != rhi::Result::Ok)
			throw std::runtime_error(std::string("BasicRHI: ") + a_what + " failed");
	}

	struct GpuBuffer
	{
		rhi::ResourcePtr resource;
		std::uint64_t bytes = 0;
		std::uint32_t srv = 0, uav = 0;  // descriptor heap indices (ResourceDescriptorHeap)
	};

	struct GpuProgram
	{
		rhi::PipelineLayoutPtr layout;
		rhi::PipelinePtr pipeline;
		rhi::CommandSignaturePtr dispatchSignature;
	};

	class Gpu
	{
	public:
		static constexpr std::uint32_t kConstantWords = 20;
		static constexpr std::uint32_t kHeapCapacity = 256;
		static constexpr std::uint32_t kMaxTimestamps = 4096;

		explicit Gpu(bool a_debug)
		{
			rhi::DeviceCreateInfo info{};
			info.backend = rhi::Backend::Vulkan;
			info.enableDebug = a_debug;
			info.framesInFlight = 1;
			Check(rhi::CreateVulkanDevice(info, device), "CreateVulkanDevice");
			queueKind = rhi::QueueKind::Compute;
			queue = device->GetQueue(queueKind);
			if (!queue) {
				queueKind = rhi::QueueKind::Graphics;
				queue = device->GetQueue(queueKind);
			}
			if (!queue)
				throw std::runtime_error("no queue");
			rhi::DescriptorHeapDesc heapDesc{};
			heapDesc.type = rhi::DescriptorHeapType::CbvSrvUav;
			heapDesc.capacity = kHeapCapacity;
			heapDesc.shaderVisible = true;
			heapDesc.debugName = "DclfPortalBench heap";
			Check(device->CreateDescriptorHeap(heapDesc, heap), "CreateDescriptorHeap");
			Check(device->CreateQueryPool(rhi::QueryPoolDesc{ .type = rhi::QueryType::Timestamp, .count = kMaxTimestamps }, timestamps), "CreateQueryPool");
			timestampBytes = std::max<std::uint32_t>(8, timestamps->GetQueryResultInfo().elementSize);
			Check(device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(std::uint64_t(kMaxTimestamps) * timestampBytes, rhi::HeapType::Readback, {}, "DclfPortalBench timestamps"),
					  timestampReadback),
				"timestamp readback");
			ticksPerSecond = device->GetTimestampCalibration(queueKind).ticksPerSecond;
			if (!ticksPerSecond)
				throw std::runtime_error("no timestamp calibration");
			// Slot 0 stays empty: index 0 means "none" to the shaders.
			nextSlot = 1;
		}

		~Gpu()
		{
			if (device)
				(void)device->WaitIdle();
		}

		std::uint64_t TicksPerSecond() const { return ticksPerSecond; }
		const char* QueueName() const { return queueKind == rhi::QueueKind::Compute ? "compute" : "graphics"; }

		GpuProgram Load(const std::string& a_spirv, const char* a_entry)
		{
			std::ifstream file(a_spirv, std::ios::binary);
			std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			if (code.empty())
				throw std::runtime_error("missing SPIR-V " + a_spirv);
			GpuProgram program;
			rhi::PushConstantRangeDesc constants{};
			constants.visibility = rhi::ShaderStage::Compute;
			constants.num32BitValues = kConstantWords;
			Check(device->CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PipelineLayoutFlags::PF_None }, program.layout),
				"CreatePipelineLayout");
			const rhi::SubobjLayout layout{ program.layout->GetHandle() };
			const rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { code.data(), static_cast<std::uint32_t>(code.size()) }, a_entry };
			const rhi::PipelineStreamItem items[] = { rhi::Make(layout), rhi::Make(shader) };
			Check(device->CreatePipeline(items, 2, program.pipeline), ("CreatePipeline " + a_spirv).c_str());
			rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
			Check(device->CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 }, program.layout->GetHandle(),
					  program.dispatchSignature),
				"CreateCommandSignature");
			return program;
		}

		/** @brief A device-local buffer of a_bytes, with an SRV and (a_uav) a UAV of a_stride (0: raw words). */
		GpuBuffer Create(std::uint64_t a_bytes, std::uint32_t a_stride, bool a_uav, const char* a_name)
		{
			GpuBuffer b;
			b.bytes = std::max<std::uint64_t>(a_bytes, 16);
			Check(device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(b.bytes, rhi::HeapType::DeviceLocal, a_uav ? rhi::RF_AllowUnorderedAccess : rhi::RF_None, a_name),
					  b.resource),
				a_name);
			const std::uint32_t stride = a_stride ? a_stride : 4;
			rhi::SrvDesc srv{};
			srv.dimension = rhi::SrvDim::Buffer;
			srv.buffer.kind = a_stride ? rhi::BufferViewKind::Structured : rhi::BufferViewKind::Raw;
			srv.buffer.numElements = static_cast<std::uint32_t>(b.bytes / stride);
			srv.buffer.structureByteStride = a_stride;
			b.srv = Slot();
			Check(device->CreateShaderResourceView({ heap->GetHandle(), b.srv }, b.resource->GetHandle(), srv), "CreateShaderResourceView");
			if (a_uav) {
				rhi::UavDesc uav{};
				uav.dimension = rhi::UavDim::Buffer;
				uav.buffer.kind = a_stride ? rhi::BufferViewKind::Structured : rhi::BufferViewKind::Raw;
				uav.buffer.numElements = static_cast<std::uint32_t>(b.bytes / stride);
				uav.buffer.structureByteStride = a_stride;
				b.uav = Slot();
				Check(device->CreateUnorderedAccessView({ heap->GetHandle(), b.uav }, b.resource->GetHandle(), uav), "CreateUnorderedAccessView");
			}
			return b;
		}

		void Upload(const GpuBuffer& a_buffer, const void* a_data, std::uint64_t a_bytes)
		{
			rhi::ResourcePtr staging;
			Check(device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(std::max<std::uint64_t>(a_bytes, 16), rhi::HeapType::Upload, {}, "upload"), staging), "upload");
			void* mapped = nullptr;
			staging->Map(&mapped, 0, a_bytes);
			std::memcpy(mapped, a_data, a_bytes);
			staging->Unmap(0, a_bytes);
			Submit([&](rhi::CommandList& a_list) {
				a_list.CopyBufferRegion(a_buffer.resource->GetHandle(), 0, staging->GetHandle(), 0, a_bytes);
			});
		}

		std::vector<std::uint32_t> Read(const GpuBuffer& a_buffer, std::uint64_t a_bytes)
		{
			rhi::ResourcePtr staging;
			Check(device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(std::max<std::uint64_t>(a_bytes, 16), rhi::HeapType::Readback, {}, "readback"), staging), "readback");
			Submit([&](rhi::CommandList& a_list) {
				Barrier(a_list);
				a_list.CopyBufferRegion(staging->GetHandle(), 0, a_buffer.resource->GetHandle(), 0, a_bytes);
			});
			void* mapped = nullptr;
			staging->Map(&mapped, 0, a_bytes);
			std::vector<std::uint32_t> words(a_bytes / 4);
			std::memcpy(words.data(), mapped, a_bytes);
			staging->Unmap(0, 0);
			return words;
		}

		static void Barrier(rhi::CommandList& a_list)
		{
			const auto barrier = rhi::FullMemoryBarrier();
			a_list.Barriers(rhi::BarrierBatch{ {}, {}, { &barrier, 1 } });
		}

		void Bind(rhi::CommandList& a_list, const GpuProgram& a_program, const std::uint32_t* a_constants) const
		{
			a_list.BindLayout(a_program.layout->GetHandle());
			a_list.BindPipeline(a_program.pipeline->GetHandle());
			a_list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, a_constants);
		}

		/**
		 * @brief Records a_reps repetitions of a_body, each between two timestamps and full barriers, in one submission; returns each
		 * repetition's GPU time in microseconds.
		 */
		template <class Body>
		std::vector<double> Time(std::uint32_t a_reps, Body&& a_body)
		{
			if (2 * a_reps > kMaxTimestamps)
				throw std::runtime_error("too many repetitions");
			Submit([&](rhi::CommandList& a_list) {
				a_list.ResetQueries(timestamps->GetHandle(), 0, 2 * a_reps);
				for (std::uint32_t r = 0; r < a_reps; ++r) {
					Barrier(a_list);
					a_list.WriteTimestamp(timestamps->GetHandle(), 2 * r, rhi::Stage::Bottom);
					a_body(a_list);
					Barrier(a_list);
					a_list.WriteTimestamp(timestamps->GetHandle(), 2 * r + 1, rhi::Stage::Bottom);
				}
				a_list.ResolveQueryData(timestamps->GetHandle(), 0, 2 * a_reps, timestampReadback->GetHandle(), 0);
			});
			void* mapped = nullptr;
			timestampReadback->Map(&mapped, 0, std::uint64_t(2) * a_reps * timestampBytes);
			std::vector<double> times(a_reps);
			for (std::uint32_t r = 0; r < a_reps; ++r) {
				std::uint64_t begin = 0, end = 0;
				std::memcpy(&begin, static_cast<const std::byte*>(mapped) + std::size_t(2 * r) * timestampBytes, 8);
				std::memcpy(&end, static_cast<const std::byte*>(mapped) + std::size_t(2 * r + 1) * timestampBytes, 8);
				times[r] = static_cast<double>(end - begin) * 1.0e6 / static_cast<double>(ticksPerSecond);
			}
			timestampReadback->Unmap(0, 0);
			return times;
		}

		/** @brief Records a_record into a fresh list with the heap bound, submits it and waits. */
		template <class Record>
		void Submit(Record&& a_record)
		{
			rhi::CommandAllocatorPtr allocator;
			rhi::CommandListPtr list;
			Check(device->CreateCommandAllocator(queueKind, allocator), "CreateCommandAllocator");
			Check(device->CreateCommandList(queueKind, allocator.Get(), list), "CreateCommandList");
			auto commands = list.Get();
			commands.SetDescriptorHeaps(heap->GetHandle(), std::nullopt);
			a_record(commands);
			commands.End();
			const rhi::CommandList lists[] = { commands };
			Check(queue.Submit(lists, {}), "Submit");
			Check(device->WaitIdle(), "WaitIdle");
		}

		/** @brief Views are not reused; a run creates its buffers once per corpus. */
		void ResetHeap() { nextSlot = 1; }

	private:
		std::uint32_t Slot()
		{
			if (nextSlot >= kHeapCapacity)
				throw std::runtime_error("descriptor heap exhausted");
			return nextSlot++;
		}

		rhi::DevicePtr device;
		rhi::QueueKind queueKind = rhi::QueueKind::Graphics;
		rhi::Queue queue;
		rhi::DescriptorHeapPtr heap;
		rhi::QueryPoolPtr timestamps;
		rhi::ResourcePtr timestampReadback;
		std::uint32_t timestampBytes = 8;
		std::uint64_t ticksPerSecond = 0;
		std::uint32_t nextSlot = 1;
	};
}
