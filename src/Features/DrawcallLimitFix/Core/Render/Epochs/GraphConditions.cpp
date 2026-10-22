// The graph's conditions (see the header).
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

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

	void RefreshGraphConditions(org::PersistentGraphHost& a_host)
	{
		GraphConditions next;
		const auto* tes = RE::TES::GetSingleton();
		next.interior = tes && tes->interiorCell;
		next.twoPassOcclusion = Toggles::Get().Active().cullMode >= 2;
		{
			std::lock_guard lock(cutsMutex);
			next.cuts = requestedCuts;
		}
		if (next == current)
			return;
		logger::info("[DCLF] graph conditions: {} cell, {}{}{}; the graph is rebuilt", next.interior ? "interior" : "exterior",
				next.twoPassOcclusion ? "two-pass occlusion" : "no occlusion pass (portal and frustum culling)", next.cuts.empty() ? "" : ", passes cut: ",
				next.cuts);
		current = next;
		latchedRecordCut.store(("," + current.cuts + ",").find(",~latched-record,") != std::string::npos, std::memory_order_relaxed);
		a_host.RequestRebuild();
	}

	const GraphConditions& CurrentGraphConditions()
	{
		return current;
	}

	bool OcclusionTested(const Resources& a_resources, const PassFrame& a_frame)
	{
		return a_resources.hzb && current.twoPassOcclusion && a_frame.cullMode >= 2 && a_frame.width && a_frame.height;
	}
}
