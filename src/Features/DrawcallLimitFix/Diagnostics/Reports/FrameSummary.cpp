#include "FrameSummary.h"

#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "RenderGraph/RenderGraphRuntime.h"

std::vector<std::string> DCLF::Reports::FrameSummary()
{
	std::vector<std::string> lines;
	const auto active = DCLF::ActiveToggles();
	const auto& stats = DCLF::Scene::Coordinator::Get().GetStats();
	const auto pipelineOf = [&](bool a_interior) { return DCLF::CpuPipelineFor(active, a_interior) ? (active.cpuRecord == 2 ? "cpu indirect" : active.cpuRecord == 1 ? "cpu parallel" : "cpu serial") : "gpu"; };
	std::string flags = fmt::format("Active: cull {}, pipeline {} (interiors {}, exteriors {})", active.cullMode,
		active.pipelineMode == DCLF::PipelineMode::Manual ? "manual" : "auto", pipelineOf(true), pipelineOf(false));
	for (const auto& toggle : DCLF::ToggleTable())
		flags += fmt::format(", {} {}", toggle.name, DCLF::ToggleValue(active, toggle) ? 1 : 0);
	lines.push_back(std::move(flags));
	lines.push_back(fmt::format("Tracked geometry: {} (under {} category nodes)", stats.tracked, stats.categoryNodes));
	lines.push_back(fmt::format("Objects this frame: {}, geometries: {}, pipelines: {}", stats.objects, stats.geometries, stats.pipelines));
	for (std::size_t i = 1; i < stats.ineligible.size(); ++i)
		if (stats.ineligible[i])
			lines.push_back(fmt::format("Left native ({}): {}", DCLF::kIneligibleNames[i], stats.ineligible[i]));
	const auto& draws = DCLF::IndirectDraws::Get().GetStats();
	lines.push_back(fmt::format("Indirect draws: {} drawn from {} records, {} epochs; culling tested {}, rejected {}",
		draws.drawn, draws.records, draws.epochs, draws.cullTested, draws.cullRejected));
	if (const auto& gpu = RenderGraphRuntime::Get().GpuTimingSummary(); !gpu.empty())
		lines.push_back("GPU (ORG pass timestamps, last report):\n" + gpu);
	const auto& capture = DCLF::PassCapture::Get().GetStats();
	const auto set = DCLF::Scene::FrameView::Get().FrameClaims();
	lines.push_back(fmt::format("DCLF set: {} members; main-view passes withheld {}", set ? static_cast<std::uint32_t>(set->phases.size()) : 0u, capture.mainWithheld));
	if (active.shadows) {
		const auto& shadow = DCLF::IndirectDraws::Get().GetShadowStats();
		lines.push_back(fmt::format("Shadow views (since the last report): {} offered, {} drawn, {} not ready; last view {} inputs, {} records; sampled culling: {} tested, {} rejected",
			shadow.views, shadow.viewsDrawn, shadow.notReady, shadow.inputs, shadow.records, shadow.cullTested, shadow.cullRejected));
		lines.push_back(fmt::format("Shadow views: withheld {} / {} / {} passes (plain / clamped / paraboloid); the set's casters {} / {} / {}",
			capture.shadowWithheld[0], capture.shadowWithheld[1], capture.shadowWithheld[2], shadow.casters[0], shadow.casters[1], shadow.casters[2]));
	}
	return lines;
}
