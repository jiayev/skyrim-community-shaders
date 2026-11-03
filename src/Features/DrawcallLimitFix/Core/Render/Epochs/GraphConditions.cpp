// The graph's conditions (see the header).
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace DCLF::Draws
{
	namespace
	{
		GraphConditions current;  // render thread; the defaults until the first build point's (what an earlier build's makes saw)
		std::mutex cutsMutex;
		std::string requestedCuts;  // cutsMutex
		std::atomic<bool> latchedRecordCut{ false };
		std::atomic<bool> builtForCpu{ false };
	}

	bool GraphBuiltForCpu()
	{
		return builtForCpu.load(std::memory_order_acquire);
	}

	bool LatchedRecordCut()
	{
		return latchedRecordCut.load(std::memory_order_relaxed);
	}

	void SetPassCuts(std::string a_cuts)
	{
		std::lock_guard lock(cutsMutex);
		requestedCuts = std::move(a_cuts);
	}

	bool PassCut(std::string_view a_key)
	{
		std::string_view cuts = current.cuts;
		while (!cuts.empty()) {
			const auto comma = cuts.find(',');
			const auto prefix = cuts.substr(0, comma);
			if (prefix == "*" || (!prefix.empty() && a_key.starts_with(prefix)))
				return true;
			cuts = comma == std::string_view::npos ? std::string_view{} : cuts.substr(comma + 1);
		}
		return false;
	}

	bool CpuPipelineForced()
	{
		const auto& pipelines = DrawPipelines::Get();
		return pipelines.Enabled() && !pipelines.DeviceGenerated();
	}

	bool CpuPipelineForCell(const ToggleSet& a_set, bool a_interior)
	{
		return CpuPipelineFor(a_set, a_interior) || CpuPipelineForced();
	}

	void RefreshGraphConditions(org::GraphHost& a_host)
	{
		GraphConditions next;
		const auto* tes = RE::TES::GetSingleton();
		next.interior = tes && tes->interiorCell;
		next.twoPassOcclusion = Toggles::Get().Active().cullMode >= 2;
		// The pipeline from the newest applied toggles, as the cull mode: the rebuild here makes every recording of the old graph stale, so
		// the frame waits at its start for a snapshot made for the graph as built (AdoptCurrentSnapshot), whose revision records the
		// epochs (the CPU pipeline's: its external epochs, RevisionInputs::cpuPipeline).
		// Which one draws this kind of cell (Auto: interiors CPU-driven): entering or leaving an interior rebuilds the graph anyway. Every
		// cell without device-generated commands.
		next.cpuPipeline = CpuPipelineForCell(Toggles::Get().Active(), next.interior);
		next.forwardDirect = !next.cpuPipeline && SwitchOnUnlessZero(Switch::ForwardDirect) &&
		                     std::ranges::contains(RenderGraphRuntime::Get().InStreamEpochs(), RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::Forward));
		next.firstPersonForwardDirect = next.forwardDirect &&
		                                std::ranges::contains(RenderGraphRuntime::Get().InStreamEpochs(), RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::FirstPerson));
		{
			std::lock_guard lock(cutsMutex);
			next.cuts = requestedCuts;
		}
		if (next == current)
			return;
		logger::info("[DCLF] graph conditions: {} cell, {}, {} pipeline{}{}{}; the graph is rebuilt", next.interior ? "interior" : "exterior",
				next.twoPassOcclusion ? "two-pass occlusion" : "no occlusion pass (portal and frustum culling)", next.cpuPipeline ? "CPU" : "GPU",
				next.forwardDirect ? ", forward draws on the CPU" : "",
				next.cuts.empty() ? "" : ", passes cut: ", next.cuts);
		// The epochs the CPU pipeline's sites execute themselves (their passes before the draws), before the build the change requests,
		// with the ones executed in DXVK's stream (RenderGraphRuntime::InStreamEpochs).
		if (next.cpuPipeline != current.cpuPipeline) {
			auto external = RenderGraphRuntime::Get().InStreamEpochs();
			if (next.cpuPipeline)
				for (const auto epoch : CpuPipelineEpochs())
					if (std::ranges::find(external, epoch) == external.end())
						external.push_back(epoch);
			std::ranges::sort(external);
			a_host.SetExternalEpochs(std::move(external));
		}
		current = next;
		builtForCpu.store(current.cpuPipeline, std::memory_order_release);
		latchedRecordCut.store(("," + current.cuts + ",").find(",~latched-record,") != std::string::npos, std::memory_order_relaxed);
		// The conditions are Drawcall Limit Fix's: its epochs' graphs are built again, the others' (Light Limit Fix's light culling)
		// keep theirs. A hosted segment has no graph of its own (its host's carries its passes).
		for (std::uint32_t segment = 0; segment < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count); ++segment)
			if (static_cast<RenderGraphRuntime::Segment>(segment) != RenderGraphRuntime::Segment::LightCulling)
				a_host.RequestRebuild(RenderGraphRuntime::EpochOf(static_cast<RenderGraphRuntime::Segment>(segment)));
	}

	const GraphConditions& CurrentGraphConditions()
	{
		return current;
	}

	bool OcclusionTested(const Resources& a_resources, const PassFrame& a_frame)
	{
		return a_resources.hzb && current.twoPassOcclusion && !current.cpuPipeline && a_frame.cullMode >= 2 && a_frame.width && a_frame.height;
	}
}
