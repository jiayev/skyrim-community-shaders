// volk must precede every Vulkan header of the translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteStats.h"

#include <array>
#include <mutex>
#include <vector>

namespace DCLF::Direct::SiteStats
{
	namespace
	{
		constexpr std::uint32_t kSlots = 4096;
		constexpr std::size_t kSites = 6;
		constexpr std::uint32_t kLogFrames = 300;
		constexpr std::uint32_t kGiveUpFrames = 32;
		constexpr std::array<const char*, kSites> kSiteNames{ "shadow", "depth", "colour", "forward", "first person", "environment probe" };
		// Input vertices, input primitives, vertex shader invocations, clipper input and output, fragment shader invocations.
		constexpr VkQueryPipelineStatisticFlags kStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
		                                                      VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
		                                                      VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
		                                                      VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
		                                                      VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
		                                                      VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
		constexpr std::size_t kValues = 6;

		struct Pending
		{
			std::uint32_t slot;
			std::size_t site;
			std::uint64_t frame;
		};

		struct State
		{
			std::once_flag once;
			bool ready = false;
			VkDevice device = VK_NULL_HANDLE;
			VkQueryPool pool = VK_NULL_HANDLE;
			PFN_vkCmdResetQueryPool cmdReset = nullptr;
			PFN_vkCmdBeginQuery cmdBegin = nullptr;
			PFN_vkCmdEndQuery cmdEnd = nullptr;
			PFN_vkGetQueryPoolResults getResults = nullptr;
			std::mutex mutex;
			std::uint32_t next = 0;
			std::vector<Pending> pending;
			std::uint64_t frame = 0;
			std::uint32_t frames = 0;
			std::array<std::array<double, kValues>, kSites> sums{};
			std::array<std::uint32_t, kSites> queries{};
			std::uint64_t dropped = 0;
		};

		State& S()
		{
			static State state;
			return state;
		}

		void Setup(rhi::Device a_device)
		{
			auto& s = S();
			const HMODULE loader = ::GetModuleHandleW(L"vulkan-1.dll");
			const auto getDeviceProcAddr = loader ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(::GetProcAddress(loader, "vkGetDeviceProcAddr")) : nullptr;
			s.device = rhi::vulkan::get_device(a_device);
			if (!getDeviceProcAddr || !s.device) {
				logger::warn("[DCLF] site stats: no Vulkan device");
				return;
			}
			const auto create = reinterpret_cast<PFN_vkCreateQueryPool>(getDeviceProcAddr(s.device, "vkCreateQueryPool"));
			s.cmdReset = reinterpret_cast<PFN_vkCmdResetQueryPool>(getDeviceProcAddr(s.device, "vkCmdResetQueryPool"));
			s.cmdBegin = reinterpret_cast<PFN_vkCmdBeginQuery>(getDeviceProcAddr(s.device, "vkCmdBeginQuery"));
			s.cmdEnd = reinterpret_cast<PFN_vkCmdEndQuery>(getDeviceProcAddr(s.device, "vkCmdEndQuery"));
			s.getResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(getDeviceProcAddr(s.device, "vkGetQueryPoolResults"));
			VkQueryPoolCreateInfo info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
			info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
			info.queryCount = kSlots;
			info.pipelineStatistics = kStatistics;
			if (!create || !s.cmdReset || !s.cmdBegin || !s.cmdEnd || !s.getResults || create(s.device, &info, nullptr, &s.pool) != VK_SUCCESS) {
				logger::warn("[DCLF] site stats: could not create the pipeline statistics query pool");
				return;
			}
			s.ready = true;
			logger::info("[DCLF] site stats on: pipeline statistics per CPU pipeline site, logged every {} frames", kLogFrames);
		}
	}

	bool Enabled()
	{
		static const bool enabled = [] {
			char value[8]{};
			return ::GetEnvironmentVariableA("CS_DCLF_SITE_STATS", value, sizeof(value)) && value[0] == '1';
		}();
		return enabled;
	}

	std::uint32_t Begin(rhi::Device a_device, void* a_commandBuffer, std::size_t a_site)
	{
		if (!Enabled() || a_site >= kSites || !a_commandBuffer)
			return ~0u;
		auto& s = S();
		std::call_once(s.once, [&] { Setup(a_device); });
		if (!s.ready)
			return ~0u;
		const auto commandBuffer = static_cast<VkCommandBuffer>(a_commandBuffer);
		std::uint32_t slot = 0;
		{
			std::lock_guard lock(s.mutex);
			slot = s.next++ % kSlots;
			s.pending.push_back({ slot, a_site, s.frame });
		}
		s.cmdReset(commandBuffer, s.pool, slot, 1);
		s.cmdBegin(commandBuffer, s.pool, slot, 0);
		return slot;
	}

	void End(void* a_commandBuffer, std::uint32_t a_slot)
	{
		if (a_slot == ~0u)
			return;
		S().cmdEnd(static_cast<VkCommandBuffer>(a_commandBuffer), S().pool, a_slot);
	}

	void Collect()
	{
		if (!Enabled())
			return;
		auto& s = S();
		if (!s.ready)
			return;
		std::lock_guard lock(s.mutex);
		++s.frame;
		std::vector<Pending> waiting;
		for (const auto& entry : s.pending) {
			std::array<std::uint64_t, kValues> values{};
			const VkResult result = s.getResults(s.device, s.pool, entry.slot, 1, sizeof(values), values.data(), sizeof(values), VK_QUERY_RESULT_64_BIT);
			if (result == VK_SUCCESS) {
				for (std::size_t v = 0; v < kValues; ++v)
					s.sums[entry.site][v] += static_cast<double>(values[v]);
				++s.queries[entry.site];
			} else if (s.frame - entry.frame < kGiveUpFrames) {
				waiting.push_back(entry);
			} else {
				++s.dropped;
			}
		}
		s.pending.swap(waiting);
		if (++s.frames < kLogFrames)
			return;
		for (std::size_t site = 0; site < kSites; ++site) {
			if (!s.queries[site])
				continue;
			const auto& v = s.sums[site];
			const double f = s.frames;
			logger::info("[DCLF] site stats {}: {:.2f} queries, IA vertices {:.0f}, IA prims {:.0f}, VS {:.0f}, clip in {:.0f} ({:.1f}% of IA), clip out {:.0f}, PS {:.0f} (per frame, {} frames, {} dropped)",
				kSiteNames[site], s.queries[site] / f, v[0] / f, v[1] / f, v[2] / f, v[3] / f, v[1] ? 100.0 * v[3] / v[1] : 0.0, v[4] / f, v[5] / f, s.frames, s.dropped);
		}
		s.sums = {};
		s.queries = {};
		s.frames = 0;
		s.dropped = 0;
	}
}
