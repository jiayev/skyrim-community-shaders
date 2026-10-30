#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/ScheduleWindow.h"

#	include "RenderGraph/RenderGraphRuntime.h"

#	include <DebugUI/ScheduleInspector.h>
#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Render/Runtime/IReadbackService.h>

#	include <atomic>
#	include <cstring>
#	include <mutex>
#	include <optional>

namespace RenderGraphSchedule
{
	namespace
	{
		std::atomic<bool> g_windowWants{ false };
		std::atomic<bool> g_devBenchWants{ false };
		bool g_windowOpen = false;  // UI thread
		org::debugui::ScheduleInspectorState g_inspector;  // UI thread

		// A resource as a pass left it (the inspector's capture). The readback completes on the render thread.
		struct Capture
		{
			std::string pass, resource;
			std::optional<org::ReadbackCaptureResult> result;
		};
		std::mutex g_captureMutex;
		std::shared_ptr<Capture> g_capture;  // guarded by g_captureMutex
		int g_captureView = 0;               // UI thread: 0 bytes, 1 u32, 2 f32

		org::RenderGraph* Graph()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			return host ? host->Graph() : nullptr;
		}

		bool Wanted()
		{
			return g_windowWants.load(std::memory_order_acquire) || g_devBenchWants.load(std::memory_order_acquire);
		}

		// The graph's switch to what the requesters want; again whenever they read, so a graph made again (a device reset) follows.
		org::RenderGraph* Apply()
		{
			auto* graph = Graph();
			if (graph) {
				const bool wanted = Wanted();
				if (graph->ScheduleTraceEnabled() != wanted)
					graph->SetScheduleTraceEnabled(wanted);
			}
			return graph;
		}
	}

	void SetTracing(Requester a_requester, bool a_enabled)
	{
		(a_requester == Requester::Window ? g_windowWants : g_devBenchWants).store(a_enabled, std::memory_order_release);
		Apply();
	}

	bool TracingEnabled()
	{
		auto* graph = Graph();
		return graph && graph->ScheduleTraceEnabled();
	}

	std::shared_ptr<const org::PersistentScheduleSnapshot> Snapshot()
	{
		auto* graph = Apply();
		return graph ? graph->GetPersistentScheduleSnapshot() : nullptr;
	}

	void OpenWindow()
	{
		g_windowOpen = true;
		SetTracing(Requester::Window, true);
	}

	namespace
	{
		void RequestCapture(const std::string& a_pass, uint64_t a_resourceId, const std::string& a_resource)
		{
			auto* graph = Graph();
			auto* readback = graph ? graph->GetReadbackService() : nullptr;
			if (!readback)
				return;
			auto capture = std::make_shared<Capture>();
			capture->pass = a_pass;
			capture->resource = a_resource;
			{
				std::scoped_lock lock(g_captureMutex);
				g_capture = capture;
			}
			readback->RequestScheduledReadbackCapture(a_pass, a_resourceId, {}, [capture](org::ReadbackCaptureResult&& a_result) {
				std::scoped_lock lock(g_captureMutex);
				capture->result = std::move(a_result);
			});
		}

		void DrawCaptureContents(Capture& capture)
		{
			std::scoped_lock lock(g_captureMutex);
			ImGui::Text("%s after %s", capture.resource.c_str(), capture.pass.c_str());
			if (!capture.result) {
				ImGui::TextDisabled("Waiting for the readback (the pass's next frame).");
				return;
			}
			const auto& result = *capture.result;
			ImGui::Text("%zu bytes", result.data.size());
			if (result.desc.kind == org::ReadbackResourceKind::Texture) {
				ImGui::SameLine();
				ImGui::Text("texture %ux%u, format %u, %zu subresources (row pitch %u)", result.width, result.height,
					static_cast<uint32_t>(result.format), result.layouts.size(), result.layouts.empty() ? 0u : result.layouts.front().rowPitch);
			}
			ImGui::RadioButton("bytes", &g_captureView, 0);
			ImGui::SameLine();
			ImGui::RadioButton("u32", &g_captureView, 1);
			ImGui::SameLine();
			ImGui::RadioButton("f32", &g_captureView, 2);
			if (ImGui::BeginChild("##captureData", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
				const size_t rowBytes = 16;
				const size_t rows = (result.data.size() + rowBytes - 1) / rowBytes;
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(rows));
				while (clipper.Step()) {
					for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
						const size_t offset = static_cast<size_t>(row) * rowBytes;
						const size_t count = std::min(rowBytes, result.data.size() - offset);
						char line[160];
						int written = std::snprintf(line, sizeof(line), "%08zx ", offset);
						for (size_t i = 0; i < count && written < static_cast<int>(sizeof(line)) - 24;) {
							if (g_captureView == 0 || count - i < 4) {
								written += std::snprintf(line + written, sizeof(line) - written, " %02x", static_cast<unsigned>(result.data[offset + i]));
								++i;
								continue;
							}
							uint32_t word = 0;
							std::memcpy(&word, result.data.data() + offset + i, sizeof(word));
							if (g_captureView == 1)
								written += std::snprintf(line + written, sizeof(line) - written, " %10u", word);
							else {
								float value = 0.0f;
								std::memcpy(&value, &word, sizeof(value));
								written += std::snprintf(line + written, sizeof(line) - written, " %12.5g", value);
							}
							i += 4;
						}
						ImGui::TextUnformatted(line);
					}
				}
			}
			ImGui::EndChild();
		}

		void DrawCapture()
		{
			std::shared_ptr<Capture> capture;
			{
				std::scoped_lock lock(g_captureMutex);
				capture = g_capture;
			}
			if (!capture)
				return;
			ImGui::SetNextWindowSize(ImVec2(700.0f, 450.0f), ImGuiCond_FirstUseEver);
			bool open = true;
			if (ImGui::Begin("Render graph capture", &open))
				DrawCaptureContents(*capture);
			ImGui::End();
			if (!open) {
				std::scoped_lock lock(g_captureMutex);
				if (g_capture == capture)
					g_capture.reset();
			}
		}
	}

	void DrawWindow()
	{
		if (!g_windowOpen)
			return;
		ImGui::SetNextWindowSize(ImVec2(1200.0f, 650.0f), ImGuiCond_FirstUseEver);
		bool open = true;
		if (ImGui::Begin("Render graph schedule", &open)) {
			if (const auto snapshot = Snapshot())
				org::debugui::ShowScheduleInspector(*snapshot, g_inspector, &RequestCapture);
			else if (!Graph())
				ImGui::TextWrapped("The render graph is not running: %s", RenderGraphRuntime::Get().GetDisabledReason().c_str());
			else
				ImGui::TextWrapped("No schedule yet: the graph traces its executables from the next frame it compiles or runs.");
		}
		ImGui::End();
		DrawCapture();
		if (!open) {
			g_windowOpen = false;
			SetTracing(Requester::Window, false);
		}
	}
}

#endif
