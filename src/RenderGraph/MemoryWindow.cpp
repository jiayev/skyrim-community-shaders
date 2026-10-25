#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/MemoryWindow.h"

#	include "RenderGraph/RenderGraphRuntime.h"

#	include <DebugUI/MemoryIntrospectionWidget.h>
#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Render/MemoryIntrospectionAPI.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <implot.h>

#	include <algorithm>
#	include <chrono>
#	include <cctype>
#	include <unordered_map>
#	include <vector>

namespace RenderGraphMemory
{
	namespace
	{
		bool g_windowOpen = false;                 // UI thread
		org::ui::MemoryIntrospectionWidget g_widget;  // UI thread
		const auto g_start = std::chrono::steady_clock::now();

		org::RenderGraph* Graph()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			return host ? host->Graph() : nullptr;
		}

		const char* MajorCategory(rhi::ResourceType a_type)
		{
			switch (a_type) {
			case rhi::ResourceType::Buffer:
				return "Buffers";
			case rhi::ResourceType::Texture1D:
			case rhi::ResourceType::Texture2D:
			case rhi::ResourceType::Texture3D:
				return "Textures";
			case rhi::ResourceType::AccelerationStructure:
				return "AccelStructs";
			default:
				return "Other";
			}
		}

		// Host memory on a discrete GPU (the upload pages and readbacks), not the device's budget.
		bool InHostMemory(const org::memory::ResourceMemoryRecord& a_record)
		{
			return a_record.heapType == rhi::HeapType::Upload || a_record.heapType == rhi::HeapType::Readback || a_record.heapType == rhi::HeapType::HostCached;
		}

		// A name's family (MemoryWindow.h's ReportLine): the name up to its last '.', and the first word after it, without digits.
		std::string Family(const std::string& a_name)
		{
			if (a_name.empty())
				return "(unnamed)";
			const auto dot = a_name.rfind('.');
			const auto start = dot == std::string::npos ? 0 : dot + 1;
			const auto dash = a_name.find('-', start);
			std::string family = a_name.substr(0, dash == std::string::npos ? a_name.size() : dash);
			while (family.size() > start && std::isdigit(static_cast<unsigned char>(family.back())))
				family.pop_back();
			return family;
		}

		std::vector<org::memory::ResourceMemoryRecord> Records(org::RenderGraph& a_graph)
		{
			std::vector<org::memory::ResourceMemoryRecord> records;
			a_graph.GetMemorySnapshotProvider().BuildSnapshot(records);
			return records;
		}

		// The widget's snapshot: a category per major type and usage hint (the family where a resource has none), a row per resource.
		org::ui::MemorySnapshot Snapshot(const std::vector<org::memory::ResourceMemoryRecord>& a_records)
		{
			org::ui::MemorySnapshot out;
			std::unordered_map<std::string, std::uint64_t> categories;
			out.resources.reserve(a_records.size());
			for (const auto& record : a_records) {
				out.totalBytes += record.bytes;
				const std::string category = std::string(MajorCategory(record.resourceType)) + (InHostMemory(record) ? " (host)/" : "/") +
				                             (record.usage.empty() ? Family(record.resourceName) : record.usage);
				categories[category] += record.bytes;
				out.resources.push_back({ .name = record.resourceName.empty() ? "Resource " + std::to_string(record.resourceID) : record.resourceName,
					.type = category,
					.bytes = record.bytes,
					.uid = record.resourceID });
			}
			for (auto& [label, bytes] : categories)
				if (bytes)
					out.categories.push_back({ label, bytes });
			std::ranges::sort(out.categories, [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
			return out;
		}

		double MiB(std::uint64_t a_bytes) { return double(a_bytes) / (1024.0 * 1024.0); }

		struct Sum
		{
			std::uint64_t bytes = 0;
			std::uint32_t count = 0;
		};

		// The a_count largest of a_sums as "name M MB (n)", comma-separated.
		std::string Largest(const std::unordered_map<std::string, Sum>& a_sums, std::size_t a_count)
		{
			std::vector<std::pair<std::string, Sum>> rows(a_sums.begin(), a_sums.end());
			std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
			std::string text;
			for (std::size_t i = 0; i < rows.size() && i < a_count; ++i)
				text += fmt::format("{}{} {:.1f} MB ({})", i ? ", " : "", rows[i].first, MiB(rows[i].second.bytes), rows[i].second.count);
			return text;
		}
	}

	void OpenWindow()
	{
		g_windowOpen = true;
	}

	void DrawWindow()
	{
		if (!g_windowOpen)
			return;
		if (!ImPlot::GetCurrentContext())
			ImPlot::CreateContext();
		auto* graph = Graph();
		if (!graph) {
			ImGui::SetNextWindowSize(ImVec2(500.0f, 120.0f), ImGuiCond_FirstUseEver);
			if (ImGui::Begin("Memory Introspection", &g_windowOpen))
				ImGui::TextWrapped("The render graph is not running: %s", RenderGraphRuntime::Get().GetDisabledReason().c_str());
			ImGui::End();
			return;
		}
		const auto snapshot = Snapshot(Records(*graph));
		g_widget.PushFrameSample(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(), snapshot.totalBytes);
		ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f), ImGuiCond_FirstUseEver);
		// No frame-graph batches: the persistent graph's are its build's, not readable while it runs (the widget says so).
		g_widget.Draw(&g_windowOpen, &snapshot, nullptr);
	}

	std::string ReportLine(std::size_t a_families, std::size_t a_names)
	{
		auto* graph = Graph();
		if (!graph)
			return {};
		const auto records = Records(*graph);
		std::uint64_t device = 0, host = 0;
		std::uint32_t deviceCount = 0;
		std::unordered_map<std::string, Sum> families, names;
		for (const auto& record : records) {
			if (InHostMemory(record)) {
				host += record.bytes;
				continue;
			}
			device += record.bytes;
			++deviceCount;
			auto& family = families[Family(record.resourceName)];
			family.bytes += record.bytes;
			++family.count;
			// An unnamed resource by its type and size (texture extent and format), so a run tells them apart.
			auto& name = names[!record.resourceName.empty() ? record.resourceName
			                   : record.width                ? fmt::format("(unnamed {} {}x{}x{} format {} {:.1f} MB)", MajorCategory(record.resourceType), record.width,
																	   record.height, record.arraySize, static_cast<int>(record.format), MiB(record.bytes))
			                                                 : fmt::format("(unnamed {} {:.1f} MB)", MajorCategory(record.resourceType), MiB(record.bytes))];
			name.bytes += record.bytes;
			++name.count;
		}
		return fmt::format("render graph memory (ORG-tracked): {:.1f} MB device-local in {} resources ({:.1f} MB more in host memory: upload pages, "
						   "readbacks); device-local by family: {}; by name: {}",
			MiB(device), deviceCount, MiB(host), Largest(families, a_families), Largest(names, a_names));
	}
}

#endif
