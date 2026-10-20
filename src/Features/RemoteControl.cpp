// Remote Control: devbench bridge status and Nsight Perf panels, shown as tabs of the
// Developer Tools feature (src/Features/DeveloperTools.cpp).
//
// The plugin's Model Context Protocol tools register into the external devbench host (the
// devbench SKSE plugin, https://github.com/alandtse/devbench) via DevBenchBridge
// (src/Features/RemoteControl/DevBenchBridge.cpp), exposed over both MCP and REST. This feature is a read-only
// panel: it reports whether devbench is present, what was registered, and the port
// devbench bound, so users can confirm the integration without leaving the game. It also
// hosts the Nsight Perf capture controls (Features/RemoteControl/NvPerf), which the devbench
// tool communityshaders.nvperf drives too.

#include "Features/RemoteControl.h"

#include "Features/RemoteControl/DevBenchBridge.h"
#include "Features/RemoteControl/NvPerf.h"
#include "Features/RemoteControl/NvPerfHost.h"
#include "Features/RemoteControl/NvPerfTrace.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/UI.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

#ifdef DEVBENCH_BRIDGE_ENABLED
#	include <DevBenchAPI.h>
#endif

namespace
{
	// devbench writes the host port it bound to here on startup. We only read it for
	// display; the bridge itself talks to devbench in-process via the C-ABI, not the port.
	constexpr const char* kRuntimeJsonPath = "Data/SKSE/Plugins/devbench/runtime.json";

	/**
	 * @brief Retrieves the bound port from devbench's runtime configuration.
	 * @return The port number from runtime.json, or 0 if the file is missing, unreadable, or malformed.
	 */
	int ReadDevBenchPort()
	{
		std::error_code ec;
		if (!std::filesystem::exists(kRuntimeJsonPath, ec))
			return 0;
		try {
			std::ifstream in(kRuntimeJsonPath);
			if (!in)
				return 0;
			json runtime = json::parse(in, nullptr, /*allow_exceptions=*/false);
			if (runtime.is_discarded() || !runtime.is_object())
				return 0;
			return runtime.value("port", 0);
		} catch (...) {
			return 0;  // malformed runtime.json is non-fatal — just hide the port
		}
	}

	/**
	 * @brief Splits a comma-separated list, trimming spaces and dropping empty entries.
	 */
	std::vector<std::string> SplitList(const std::string& a_text)
	{
		std::vector<std::string> parts;
		std::stringstream stream(a_text);
		for (std::string part; std::getline(stream, part, ',');) {
			const auto first = part.find_first_not_of(" \t");
			if (first == std::string::npos)
				continue;
			parts.push_back(part.substr(first, part.find_last_not_of(" \t") - first + 1));
		}
		return parts;
	}

	bool IsTimeMetric(const NvPerf::MetricRequest& a_metric)
	{
		return a_metric.name == "gpu__time_duration";
	}
}

/**
 * @brief Obtains the singleton instance.
 * @return RemoteControl* The global singleton instance.
 */
RemoteControl* RemoteControl::GetSingleton()
{
	return &globals::features::remoteControl;
}

/**
 * @brief Registers the plugin's tools into the devbench host.
 *
 * Idempotent and inert when the host is absent or the devbench bridge is disabled.
 */
void RemoteControl::DataLoaded()
{
	// Register the plugin's tools into the devbench host. This component owns the install; it runs at
	// DataLoaded rather than Load because devbench publishes its cross-plugin interface at
	// kPostLoad — by DataLoaded it's ready. Inert (logged) when no host is present or the
	// bridge was built disabled; idempotent on the devbench side (re-registering replaces).
	DevBenchBridge::Install();
}

bool RemoteControl::IsDevBenchHostPresent()
{
#ifdef DEVBENCH_BRIDGE_ENABLED
	return DevBenchAPI::GetDevBenchInterface001() != nullptr;
#else
	return false;
#endif
}

bool RemoteControl::IsDevBenchBridgeBuilt()
{
#ifdef DEVBENCH_BRIDGE_ENABLED
	return true;
#else
	return false;
#endif
}

/**
 * @brief Renders the devbench integration status panel and tool list.
 *
 * Displays connection status, host build number, bound port, and the set of devbench-exposed tools
 * (feature management, engine inspection, shader cache control, capture, and settings). Shows a
 * warning if the devbench host is not detected or if the build was compiled without the devbench
 * bridge.
 */
int RemoteControl::GetDevBenchPort()
{
	// runtime.json I/O + JSON parse every frame would hitch the UI, and devbench may bind after the panel
	// first opens, so re-read on a coarse interval. QPC, not std::chrono.
	static int cachedPort = -1;       // -1 = not yet read
	static LONGLONG lastReadQpc = 0;  // ticks at last read
	LARGE_INTEGER freq, nowQpc;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&nowQpc);
	if (cachedPort < 0 || nowQpc.QuadPart - lastReadQpc > 2 * freq.QuadPart) {  // > 2s
		cachedPort = ReadDevBenchPort();
		lastReadQpc = nowQpc.QuadPart;
	}
	return std::max(cachedPort, 0);
}

void RemoteControl::DrawDevBenchPanel()
{
	const auto& theme = Menu::GetSingleton()->GetTheme().StatusPalette;

	ImGui::TextWrapped(
		"Registers graphics-feature, inspect, capture, shader-cache, and settings tools "
		"into the external devbench host so AI assistants (Claude Code, Cursor, etc.) can "
		"toggle features, inspect engine state, trigger captures, and save/load settings "
		"over MCP and REST. There is no in-game server — install the devbench SKSE plugin "
		"to enable the integration.");
	ImGui::Spacing();

#ifdef DEVBENCH_BRIDGE_ENABLED
	auto* dvb = DevBenchAPI::GetDevBenchInterface001();
	if (dvb) {
		ImGui::TextColored(theme.SuccessColor, "devbench host present (build %u)", dvb->GetBuildNumber());

		const int port = GetDevBenchPort();
		if (port > 0) {
			const std::string endpoint = std::format("http://127.0.0.1:{}", port);
			ImGui::AlignTextToFramePadding();
			ImGui::Text(T("feature.remote_control.devbench.rest_endpoint", "REST endpoint: %s"), endpoint.c_str());
			ImGui::SameLine();
			if (Util::Button(T("feature.remote_control.devbench.copy_endpoint", "Copy")))
				ImGui::SetClipboardText(endpoint.c_str());
			ImGui::TextDisabled("%s", T("feature.remote_control.devbench.mcp_hint", "MCP clients connect through devbench's own bridge executable, which forwards to this endpoint."));
		} else {
			ImGui::TextDisabled(
				"Port unknown — devbench writes it to %s once it binds.",
				kRuntimeJsonPath);
		}
		if (const auto request = DevBenchBridge::GetLastRequest())
			ImGui::Text(T("feature.remote_control.devbench.last_request", "Last request: %s, %s"), request->tool.c_str(), Util::FormatTimeAgo(request->time).c_str());
		else
			ImGui::TextDisabled("%s", T("feature.remote_control.devbench.no_requests", "No requests yet this session."));
	} else {
		ImGui::TextColored(theme.Warning,
			"devbench host not detected. Install the devbench SKSE plugin; "
			"the tools register automatically once it is present.");
	}

	ImGui::Separator();
	ImGui::TextUnformatted("Tools exposed through devbench:");
	ImGui::BulletText("communityshaders.feature — list / get / set / reset / toggle features");
	ImGui::BulletText("communityshaders.inspect — engine state and shader-cache status");
	ImGui::BulletText("communityshaders.shadercache — clear / delete the compiled cache");
	ImGui::BulletText("communityshaders.capture — RenderDoc / screenshot capture");
	ImGui::BulletText("%s", T("feature.remote_control.nvperf.tool_bullet", "communityshaders.nvperf — Nsight Perf GPU counters per render pass"));
	ImGui::BulletText("communityshaders.settings — save / load / reset the global config");
	ImGui::TextDisabled(
		"Note: the console tool is provided by devbench itself, not this plugin.");
#else
	ImGui::TextColored(theme.Warning,
		"This build was compiled without the devbench bridge "
		"(DEVBENCH_BRIDGE=OFF). No tools are registered.");
#endif
}

#define I18N_KEY_PREFIX "feature.remote_control.nvperf."

bool RemoteControl::RequestNvPerfCapture()
{
	auto& panel = nvperfPanel;
	const auto defaults = NvPerf::DefaultMetrics();
	if (panel.defaultMetricEnabled.size() != defaults.size())
		panel.defaultMetricEnabled.assign(defaults.size(), 1);

	NvPerf::CaptureConfiguration configuration;
	for (std::size_t i = 0; i < defaults.size(); ++i)
		if (panel.defaultMetricEnabled[i])
			configuration.metrics.push_back(defaults[i]);
	panel.requestError.clear();
	for (const auto& text : SplitList(panel.extraMetrics)) {
		NvPerf::MetricRequest metric;
		if (!NvPerf::ParseMetric(text, metric, panel.requestError))
			break;
		configuration.metrics.push_back(std::move(metric));
	}
	if (panel.requestError.empty() && configuration.metrics.empty())
		panel.requestError = T(TKEY("no_metrics"), "Select at least one metric.");
	configuration.ranges = SplitList(panel.ranges);
	configuration.samples = static_cast<std::uint32_t>(panel.samples);
	configuration.delayFrames = static_cast<std::uint32_t>(panel.delayFrames);
	configuration.nestingLevels = static_cast<std::uint16_t>(panel.nestingLevels);
	std::uint64_t id = 0;
	if (panel.requestError.empty())
		NvPerf::RequestCapture(configuration, id, panel.requestError);
	return panel.requestError.empty();
}

void RemoteControl::UpdateNvPerfHistory()
{
	auto& recent = nvperfPanel.recent;
	auto latest = NvPerf::LatestResult();
	if (!latest || (!recent.empty() && recent.front().result->id == latest->id))
		return;
	recent.insert(recent.begin(), { std::move(latest), std::filesystem::file_time_type::clock::now() });
	if (recent.size() > kMaxRecentNvPerfCaptures)
		recent.resize(kMaxRecentNvPerfCaptures);
	nvperfPanel.viewedCaptureId = 0;  // follow the new capture
}

/**
 * @brief Lists the captures of this session, newest first: picking one shows its summary below, and each row opens
 * its CSV or the folder holding it.
 */
void RemoteControl::DrawRecentNvPerfCaptures(NvPerfPanel& a_panel)
{
	if (a_panel.recent.empty())
		return;
	const auto& theme = Menu::GetSingleton()->GetTheme().StatusPalette;
	ImGui::Separator();
	ImGui::TextUnformatted(T(TKEY("recent"), "Recent captures"));
	if (!ImGui::BeginTable("##NvPerfRecent", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
		return;
	ImGui::TableSetupColumn(T(TKEY("recent_capture"), "Capture"));
	ImGui::TableSetupColumn(T(TKEY("recent_when"), "When"));
	ImGui::TableSetupColumn(T(TKEY("recent_result"), "Result"), ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn(T(TKEY("recent_actions"), "Files"));
	ImGui::TableHeadersRow();
	const std::uint64_t viewed = a_panel.viewedCaptureId ? a_panel.viewedCaptureId : a_panel.recent.front().result->id;
	for (const auto& recent : a_panel.recent) {
		const auto& result = *recent.result;
		ImGui::PushID(static_cast<int>(result.id));
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		const std::string label = std::format("#{}", result.id);
		if (Util::Selectable(label.c_str(), result.id == viewed, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
			a_panel.viewedCaptureId = result.id;
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(Util::FormatTimeAgo(recent.time).c_str());
		ImGui::TableNextColumn();
		if (result.success)
			ImGui::Text(T(TKEY("recent_ok"), "%zu ranges, %u samples"), result.summary.size(), result.samples);
		else
			ImGui::TextColored(theme.Error, "%s", result.error.c_str());
		ImGui::TableNextColumn();
		{
			Util::DisableGuard noCsv(result.csvPath.empty());
			if (ImGui::SmallButton(T(TKEY("open_csv"), "Open CSV")))
				ShellExecuteW(nullptr, L"open", result.csvPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			ImGui::SameLine();
			if (ImGui::SmallButton(T(TKEY("show_in_folder"), "Show in folder"))) {
				const std::wstring args = L"/select,\"" + result.csvPath.wstring() + L"\"";
				ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
			}
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

/**
 * @brief Renders the Nsight Perf capture controls and the recent captures and the viewed capture's per-range summary.
 *
 * The profiler library is probed the first time the section is opened. A capture runs over the next frames on the
 * render thread; this panel only requests it and reads its status, so it works with or without the devbench host.
 */
void RemoteControl::DrawNvPerfPanel()
{
	const auto& theme = Menu::GetSingleton()->GetTheme().StatusPalette;
	auto& panel = nvperfPanel;

	ImGui::TextWrapped("%s", T(TKEY("description"), "Measures NVIDIA GPU hardware counters (time, unit throughput, warp stalls) over the frame's perf events with the Nsight Perf SDK. Each frame is one profiler pass; a sample takes as many frames as its metrics need passes. Also drivable through the devbench tool communityshaders.nvperf."));
	ImGui::Spacing();

	NvPerf::Probe();
	const auto device = NvPerf::GetDeviceInfo();
	if (!device.built) {
		ImGui::TextColored(theme.Warning, T(TKEY("not_built"), "This build was compiled without Nsight Perf: %s."), NvPerf::Host::BuildHint());
		return;
	}
	if (!device.gpuSupported) {
		ImGui::TextColored(theme.Warning, T(TKEY("unavailable"), "Nsight Perf is unavailable: %s"), device.error.c_str());
		ImGui::TextDisabled(T(TKEY("library_path"), "Library: %s"), Util::PathHelpers::ToUtf8(device.libraryPath).c_str());
		return;
	}
	ImGui::TextColored(theme.SuccessColor, T(TKEY("device"), "%s (chip %s)"), device.deviceName.c_str(), device.chipName.c_str());
	ImGui::TextDisabled(T(TKEY("backend"), "Backend: %s"), device.backend.c_str());

	DrawNvPerfTraces();
	if (!ImGui::TreeNode(T(TKEY("manual"), "Single capture")))
		return;
	struct TreeClose
	{
		~TreeClose() { ImGui::TreePop(); }
	} treeClose;

	const auto defaults = NvPerf::DefaultMetrics();
	if (panel.defaultMetricEnabled.size() != defaults.size())
		panel.defaultMetricEnabled.assign(defaults.size(), 1);

	ImGui::InputTextWithHint(T(TKEY("ranges"), "Ranges"), T(TKEY("ranges_hint"), "every outermost range"), &panel.ranges);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("ranges_tooltip"), "Comma-separated perf event names; a trailing * matches every name with that prefix."));
		ImGui::Text("%s", T(TKEY("ranges_tooltip2"), "Ranges are measured to the nesting depth below; empty measures every outermost range."));
		ImGui::Text("%s", T(TKEY("ranges_tooltip3"), "Engine passes (shadow maps, BSShaderAccumulator batches) only exist when Frame Annotations was enabled at startup."));
	}
	if (ImGui::TreeNode(T(TKEY("metrics"), "Metrics"))) {
		for (std::size_t i = 0; i < defaults.size(); ++i) {
			bool enabled = panel.defaultMetricEnabled[i] != 0;
			if (ImGui::Checkbox(NvPerf::FormatMetric(defaults[i]).c_str(), &enabled))
				panel.defaultMetricEnabled[i] = enabled ? 1 : 0;
		}
		ImGui::InputTextWithHint(T(TKEY("extra_metrics"), "Extra metrics"), "sm__cycles_active:elapsed, ...", &panel.extraMetrics);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("extra_metrics_tooltip"), "Comma-separated name[:counter|ratio|throughput|active|elapsed|percycle]."));
		ImGui::TreePop();
	}
	ImGui::SliderInt(T(TKEY("samples"), "Samples"), &panel.samples, 1, 16);
	ImGui::SliderInt(T(TKEY("nesting"), "Nesting levels"), &panel.nestingLevels, 1, 4);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("nesting_tooltip"), "Selected ranges nested this deep are measured too. Each level multiplies the passes."));
		ImGui::Text("%s", T(TKEY("nesting_tooltip2"), "The Vulkan backend measures one level."));
	}
	ImGui::SliderInt(T(TKEY("delay"), "Delay (frames)"), &panel.delayFrames, 0, 600);

	const auto status = NvPerf::GetStatus();
	const bool busy = status.state == NvPerf::CaptureState::Waiting || status.state == NvPerf::CaptureState::Capturing ||
	                  status.state == NvPerf::CaptureState::Decoding;
	if (busy) {
		if (Util::Button(T(TKEY("cancel"), "Cancel capture")))
			NvPerf::CancelCapture();
	} else if (Util::Button(T(TKEY("capture"), "Capture"))) {
		RequestNvPerfCapture();
	}
	if (!panel.requestError.empty())
		ImGui::TextColored(theme.Error, "%s", panel.requestError.c_str());

	switch (status.state) {
	case NvPerf::CaptureState::Waiting:
		ImGui::TextColored(theme.InfoColor, T(TKEY("status_waiting"), "Capture %llu: waiting to start"), status.captureId);
		break;
	case NvPerf::CaptureState::Capturing:
		ImGui::TextColored(theme.InfoColor, T(TKEY("status_capturing"), "Capture %llu: sample %u/%u, pass %zu/%zu"), status.captureId, status.sample + 1,
			status.samples, status.pass + 1, status.passes);
		break;
	case NvPerf::CaptureState::Decoding:
		ImGui::TextColored(theme.InfoColor, T(TKEY("status_decoding"), "Capture %llu: sample %u/%u, decoding"), status.captureId, status.sample + 1,
			status.samples);
		break;
	default:
		break;
	}

	UpdateNvPerfHistory();
	DrawRecentNvPerfCaptures(panel);

	// The picked capture, else the newest
	std::shared_ptr<const NvPerf::CaptureResult> result = panel.recent.empty() ? nullptr : panel.recent.front().result;
	for (const auto& recent : panel.recent)
		if (recent.result->id == panel.viewedCaptureId)
			result = recent.result;
	if (!result)
		return;
	ImGui::Separator();
	if (result->success)
		ImGui::TextColored(theme.SuccessColor, T(TKEY("result_ok"), "Capture %llu: %zu ranges, frames %llu-%llu, %u samples x %zu passes"), result->id,
			result->rangeResults.size(), result->startFrame, result->endFrame, result->samples, result->passesPerSample);
	else
		ImGui::TextColored(theme.Error, T(TKEY("result_failed"), "Capture %llu failed: %s"), result->id, result->error.c_str());
	for (const auto& metric : result->unsupportedMetrics)
		ImGui::TextColored(theme.Warning, T(TKEY("unsupported_metric"), "Unavailable on this GPU: %s"), NvPerf::FormatMetric(metric).c_str());

	if (!result->summary.empty()) {
		const int columns = static_cast<int>(result->metrics.size()) + 2;
		const float height = std::min(ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(result->summary.size()) + 2.0f), 400.0f);
		if (ImGui::BeginTable("##NvPerfSummary", columns,
				ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
				ImVec2(0.0f, height))) {
			ImGui::TableSetupScrollFreeze(1, 1);
			ImGui::TableSetupColumn(T(TKEY("column_range"), "Range"), ImGuiTableColumnFlags_WidthFixed, 260.0f);
			ImGui::TableSetupColumn(T(TKEY("column_count"), "Per frame"), ImGuiTableColumnFlags_WidthFixed);
			for (const auto& metric : result->metrics) {
				const std::string label = IsTimeMetric(metric) ? std::string(T(TKEY("column_time"), "GPU time (ms)")) : metric.name;
				ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed);
			}
			ImGui::TableHeadersRow();
			for (const auto& entry : result->summary) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(entry.name.c_str());
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted(entry.name.c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%.3g", entry.occurrencesPerFrame);
				for (std::size_t m = 0; m < result->metrics.size(); ++m) {
					ImGui::TableNextColumn();
					const double value = m < entry.values.size() ? entry.values[m] : std::nan("");
					if (std::isnan(value))
						ImGui::TextDisabled("-");
					else
						ImGui::Text("%.4g", IsTimeMetric(result->metrics[m]) ? value / 1.0e6 : value);
				}
			}
			ImGui::EndTable();
		}
	}
	ImGui::TextDisabled(T(TKEY("csv_path"), "Every range of every sample: %s"), Util::PathHelpers::ToUtf8(result->csvPath).c_str());
	ImGui::SameLine();
	if (ImGui::SmallButton(T(TKEY("copy_path"), "Copy path")))
		ImGui::SetClipboardText(Util::PathHelpers::ToUtf8(result->csvPath).c_str());
}

/**
 * @brief The trace controls (NvPerfTrace.h): a template, a label and an optional baseline report; the running trace's
 * progress; the latest report's groups, rows and warnings.
 */
void RemoteControl::DrawNvPerfTraces()
{
	const auto& theme = Menu::GetSingleton()->GetTheme().StatusPalette;
	auto& panel = nvperfPanel;
	if (!ImGui::TreeNodeEx(T(TKEY("traces"), "Traces"), ImGuiTreeNodeFlags_DefaultOpen))
		return;
	struct TreeClose
	{
		~TreeClose() { ImGui::TreePop(); }
	} treeClose;

	const auto templates = NvPerf::Trace::Templates();
	const NvPerf::Trace::Template* selected = nullptr;
	for (const auto& t : templates)
		if (t.name == panel.traceTemplate)
			selected = &t;
	if (ImGui::BeginCombo(T(TKEY("trace_template"), "Template"), panel.traceTemplate.c_str())) {
		for (const auto& t : templates) {
			const std::string label = t.source.empty() ? t.name : std::format("{} ({})", t.name, t.source.filename().string());
			if (ImGui::Selectable(label.c_str(), t.name == panel.traceTemplate))
				panel.traceTemplate = t.name;
			if (!t.description.empty())
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted(t.description.c_str());
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(T(TKEY("trace_reload"), "Reload")))
		panel.traceError = NvPerf::Trace::ReloadTemplates();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T(TKEY("trace_reload_tooltip"), "Reads the *.json templates in %s again."), NvPerf::Trace::TemplateDirectory().string().c_str());
	if (selected) {
		ImGui::TextWrapped("%s", selected->description.c_str());
		if (selected->timingSource == NvPerf::Trace::TimingSource::Timestamps) {
			ImGui::TextDisabled(T(TKEY("trace_shape_timestamps"), "Pass timestamps: every frame a sample after %u settling frames, %u-%u samples, +/-%.1f%% at %.0f%% confidence, %zu range filters"),
				selected->settlingFrames, selected->minSamples, selected->maxSamples, 100.0 * selected->relativeHalfWidth, 100.0 * selected->confidence,
				selected->ranges.size());
		} else {
			const std::size_t captures = selected->metricGroups.size() * selected->repeats;
			ImGui::TextDisabled(T(TKEY("trace_shape"), "%zu captures (%zu metric groups x %u repeats), %u samples each, %zu range filters"), captures,
				selected->metricGroups.size(), selected->repeats, selected->samples, selected->ranges.size());
			if (selected->maxRepeats > selected->repeats)
				ImGui::TextDisabled(T(TKEY("trace_shape_convergence"), "More rounds, up to %u, until every row's time is within +/-%.1f%% at %.0f%% confidence"),
					selected->maxRepeats, 100.0 * selected->relativeHalfWidth, 100.0 * selected->confidence);
		}
	}
	ImGui::InputTextWithHint(T(TKEY("trace_label"), "Label"), panel.traceTemplate.c_str(), &panel.traceLabel);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T(TKEY("trace_label_tooltip"), "The report's name in %s (e.g. dclf-on, dclf-off)."), NvPerf::Trace::ReportDirectory().string().c_str());
	const auto saved = NvPerf::Trace::SavedReports();
	const char* none = T(TKEY("trace_no_baseline"), "(none)");
	if (ImGui::BeginCombo(T(TKEY("trace_baseline"), "Compare with"), panel.traceBaseline.empty() ? none : panel.traceBaseline.c_str())) {
		if (ImGui::Selectable(none, panel.traceBaseline.empty()))
			panel.traceBaseline.clear();
		for (const auto& label : saved)
			if (ImGui::Selectable(label.c_str(), label == panel.traceBaseline))
				panel.traceBaseline = label;
		ImGui::EndCombo();
	}

	const auto status = NvPerf::Trace::GetStatus();
	if (status.state == NvPerf::Trace::State::Running) {
		if (ImGui::Button(T(TKEY("trace_cancel"), "Cancel trace")))
			NvPerf::Trace::Cancel();
		ImGui::SameLine();
		if (status.captures)
			ImGui::TextColored(theme.InfoColor, T(TKEY("trace_running"), "Trace '%s': capture %u of %u (%s)"), status.label.c_str(), status.capture, status.captures,
				status.phase.c_str());
		else
			ImGui::TextColored(theme.InfoColor, T(TKEY("trace_running_timestamps"), "Trace '%s': %s"), status.label.c_str(), status.phase.c_str());
		// Convergence, as of the trace's last check: samples, rows and groups converged, the one furthest from its target.
		if (status.convergence) {
			const float progress = status.series ? static_cast<float>(status.converged) / static_cast<float>(status.series) : 0.0f;
			const std::string overlay = std::format("{} / {}", status.converged, status.series);
			ImGui::ProgressBar(progress, ImVec2(ImGui::GetFontSize() * 10.0f, 0.0f), overlay.c_str());
			ImGui::SameLine();
			if (status.maxSamples)
				ImGui::Text(T(TKEY("trace_convergence_samples"), "%u samples (%u-%u)"), status.samples, status.minSamples, status.maxSamples);
			else
				ImGui::Text(T(TKEY("trace_convergence_samples_min"), "%u samples (at least %u)"), status.samples, status.minSamples);
			if (!status.worst.empty())
				ImGui::TextDisabled(T(TKEY("trace_convergence_worst"), "Furthest: %s at +/-%.1f%% (target %.1f%%)"), status.worst.c_str(),
					100.0 * status.worstRelativeHalfWidth, 100.0 * status.targetRelativeHalfWidth);
		}
	} else if (ImGui::Button(T(TKEY("trace_run"), "Run trace"))) {
		panel.traceError.clear();
		NvPerf::Trace::Start(panel.traceTemplate, panel.traceLabel, panel.traceBaseline, panel.traceError);
	}
	if (!panel.traceError.empty())
		ImGui::TextColored(theme.Error, "%s", panel.traceError.c_str());

	const auto report = NvPerf::Trace::Latest();
	if (!report)
		return;
	ImGui::Separator();
	if (report->success && report->timingSource == NvPerf::Trace::TimingSource::Timestamps)
		ImGui::TextColored(theme.SuccessColor, T(TKEY("trace_result_timestamps"), "Trace '%s' (%s, timestamps): %u frames, total %.1f us"), report->label.c_str(),
			report->templateName.c_str(), report->samples, report->totalUs);
	else if (report->success)
		ImGui::TextColored(theme.SuccessColor, T(TKEY("trace_result"), "Trace '%s' (%s): %u captures, %u samples, total %.1f us"), report->label.c_str(),
			report->templateName.c_str(), report->captures, report->samples, report->totalUs);
	else
		ImGui::TextColored(theme.Error, T(TKEY("trace_failed"), "Trace '%s' failed: %s"), report->label.c_str(), report->error.c_str());
	if (report->success && report->confidence > 0.0) {
		if (report->converged)
			ImGui::TextColored(theme.SuccessColor, T(TKEY("trace_converged"), "Converged: every mean within +/-%.1f%% at %.0f%% confidence"),
				100.0 * report->relativeHalfWidth, 100.0 * report->confidence);
		else
			ImGui::TextColored(theme.Warning, T(TKEY("trace_not_converged"), "Not converged to +/-%.1f%% at %.0f%% confidence (see the conv column)"),
				100.0 * report->relativeHalfWidth, 100.0 * report->confidence);
	}
	for (const auto& warning : report->warnings)
		ImGui::TextColored(theme.Warning, "%s", warning.c_str());
	const auto* baseline = report->baseline.get();
	// A delta's colour: worse or better only when the means differ by more than their intervals combined.
	const auto deltaColor = [&](double a_delta, const NvPerf::Trace::SeriesStats& a_stats, const NvPerf::Trace::SeriesStats* a_base) {
		const bool significant = a_base && std::isfinite(a_stats.ciUs) && std::isfinite(a_base->ciUs) &&
		                         std::abs(a_stats.meanUs - a_base->meanUs) > std::sqrt(a_stats.ciUs * a_stats.ciUs + a_base->ciUs * a_base->ciUs);
		if (!significant && a_base && std::isfinite(a_stats.ciUs))
			return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
		return a_delta > 0.0 ? theme.Warning : theme.SuccessColor;
	};
	const auto ciText = [](const NvPerf::Trace::SeriesStats& a_stats) { return std::isfinite(a_stats.ciUs) ? std::format("{:.2f}", a_stats.ciUs) : std::string("-"); };
	if (baseline)
		ImGui::TextDisabled(T(TKEY("trace_compared"), "Compared with '%s': total %.1f us (%+.1f)"), baseline->label.c_str(), baseline->totalUs,
			report->totalUs - baseline->totalUs);

	const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
	const float groupsHeight = std::min(ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(report->groups.size()) + 2.0f), 260.0f);
	if (!report->groups.empty() && ImGui::BeginTable("##NvPerfTraceGroups", baseline ? 6 : 4, flags, ImVec2(0.0f, groupsHeight))) {
		ImGui::TableSetupColumn(T(TKEY("trace_group"), "Group"), ImGuiTableColumnFlags_WidthFixed, 200.0f);
		ImGui::TableSetupColumn(T(TKEY("trace_time"), "GPU time (us)"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("trace_ci"), "+/- (us)"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("trace_converged_column"), "Conv"), ImGuiTableColumnFlags_WidthFixed);
		if (baseline) {
			ImGui::TableSetupColumn(T(TKEY("trace_baseline_time"), "Baseline (us)"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn(T(TKEY("trace_delta"), "Delta (us)"), ImGuiTableColumnFlags_WidthFixed);
		}
		ImGui::TableHeadersRow();
		for (const auto& group : report->groups) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(group.name.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%.1f", group.timeUs);
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", ciText(group.stats).c_str());
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(T(TKEY("trace_stats_tooltip"), "mean %.2f us, median %.2f, p05-p95 %.2f-%.2f, n_eff %.0f, %u MAD flags"), group.stats.meanUs, group.stats.medianUs,
					group.stats.p05Us, group.stats.p95Us, group.stats.effectiveSamples, group.stats.madOutliers);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(group.stats.converged ? T(TKEY("trace_yes"), "yes") : T(TKEY("trace_no"), "no"));
			if (baseline) {
				const auto found = std::ranges::find_if(baseline->groups, [&](const auto& g) { return g.name == group.name; });
				const double base = found == baseline->groups.end() ? 0.0 : found->timeUs;
				ImGui::TableNextColumn();
				ImGui::Text("%.1f", base);
				ImGui::TableNextColumn();
				ImGui::TextColored(deltaColor(group.timeUs - base, group.stats, found == baseline->groups.end() ? nullptr : &found->stats), "%+.1f", group.timeUs - base);
			}
		}
		ImGui::EndTable();
	}

	ImGui::SliderInt(T(TKEY("trace_rows"), "Rows shown"), &panel.traceRows, 5, 400);
	const int fixedColumns = baseline ? 9 : 8;
	const int columns = fixedColumns + static_cast<int>(report->metricLabels.size());
	const auto shown = std::min<std::size_t>(report->rows.size(), static_cast<std::size_t>(panel.traceRows));
	const float rowsHeight = std::min(ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(shown) + 2.0f), 420.0f);
	if (ImGui::BeginTable("##NvPerfTraceRows", columns, flags, ImVec2(0.0f, rowsHeight))) {
		ImGui::TableSetupScrollFreeze(1, 1);
		ImGui::TableSetupColumn(T(TKEY("column_range"), "Range"), ImGuiTableColumnFlags_WidthFixed, 280.0f);
		ImGui::TableSetupColumn(T(TKEY("trace_group"), "Group"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
		ImGui::TableSetupColumn(T(TKEY("trace_time"), "GPU time (us)"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("trace_ci"), "+/- (us)"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("trace_percentiles"), "p05-p95"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("trace_spread"), "Min-max"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("trace_effective"), "n_eff"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("trace_converged_column"), "Conv"), ImGuiTableColumnFlags_WidthFixed);
		if (baseline)
			ImGui::TableSetupColumn(T(TKEY("trace_delta"), "Delta (us)"), ImGuiTableColumnFlags_WidthFixed);
		for (const auto& label : report->metricLabels)
			ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();
		for (std::size_t i = 0; i < shown; ++i) {
			const auto& row = report->rows[i];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(row.name.c_str());
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(T(TKEY("trace_row_tooltip_stats"), "%s%s%s: %.3g per frame, in %u samples; mean %.2f us, median %.2f, %u MAD flags"), row.name.c_str(),
					row.queue.empty() ? "" : ", queue ", row.queue.c_str(), row.occurrencesPerFrame, row.samples, row.stats.meanUs, row.stats.medianUs, row.stats.madOutliers);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(row.group.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%.1f", row.timeUs);
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", ciText(row.stats).c_str());
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%.1f-%.1f", row.stats.p05Us, row.stats.p95Us);
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%.1f-%.1f", row.timeMinUs, row.timeMaxUs);
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%.0f", row.stats.effectiveSamples);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(row.stats.converged ? T(TKEY("trace_yes"), "yes") : T(TKEY("trace_no"), "no"));
			if (baseline) {
				const auto found = std::ranges::find_if(baseline->rows, [&](const auto& r) { return r.name == row.name; });
				const double base = found == baseline->rows.end() ? 0.0 : found->timeUs;
				ImGui::TableNextColumn();
				ImGui::TextColored(deltaColor(row.timeUs - base, row.stats, found == baseline->rows.end() ? nullptr : &found->stats), "%+.1f", row.timeUs - base);
			}
			for (const auto& label : report->metricLabels) {
				ImGui::TableNextColumn();
				const auto found = row.metrics.find(label);
				if (found == row.metrics.end() || std::isnan(found->second))
					ImGui::TextDisabled("-");
				else
					ImGui::Text("%.4g", found->second);
			}
		}
		ImGui::EndTable();
	}
	ImGui::TextDisabled(T(TKEY("trace_report_path"), "Report: %s"), report->textPath.string().c_str());
	ImGui::SameLine();
	if (ImGui::SmallButton(T(TKEY("trace_copy_report"), "Copy report")))
		ImGui::SetClipboardText(NvPerf::Trace::FormatReport(*report).c_str());
}

#undef I18N_KEY_PREFIX
