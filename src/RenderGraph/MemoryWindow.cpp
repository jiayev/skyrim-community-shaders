#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/MemoryWindow.h"

#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Vram/VramLedger.h"

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

		// The widget's snapshot of VRAM attribution's: a row per resource (Vram::RowFlags are the widget's MemoryRowFlags), a
		// category per source and type, the heaps and the reconciliation.
		org::ui::MemorySnapshot Snapshot(const Vram::Snapshot& a_snapshot)
		{
			constexpr auto same = [](std::uint32_t a, std::uint32_t b) { return a == b; };
			static_assert(same(Vram::kRowAliased, org::ui::MemoryRowFlag_Aliased) && same(Vram::kRowNotCounted, org::ui::MemoryRowFlag_NotCounted) &&
						  same(Vram::kRowImported, org::ui::MemoryRowFlag_Imported) && same(Vram::kRowUnattributed, org::ui::MemoryRowFlag_Unattributed) &&
						  same(Vram::kRowInternal, org::ui::MemoryRowFlag_Internal));

			org::ui::MemorySnapshot out;
			std::unordered_map<std::string, std::uint64_t> categories;
			out.resources.reserve(a_snapshot.rows.size());
			for (const auto& row : a_snapshot.rows) {
				if (!(row.flags & Vram::kRowNotCounted)) {
					out.totalBytes += row.bytes;
					categories[row.category + (row.deviceLocal ? "" : " (host)")] += row.bytes;
				}
				out.resources.push_back({ .name = row.name,
					.type = row.detail.empty() ? row.type : row.type + " " + row.detail,
					.bytes = row.bytes,
					.uid = row.uid,
					.owner = row.owner,
					.source = row.source,
					.heap = row.deviceLocal ? "Device" : "Host",
					.callsite = row.callsite,
					.flags = row.flags });
			}
			for (auto& [label, bytes] : categories)
				if (bytes)
					out.categories.push_back({ label, bytes });
			std::ranges::sort(out.categories, [](const auto& a, const auto& b) { return a.bytes > b.bytes; });

			for (const auto& heap : a_snapshot.heaps)
				out.heaps.push_back({ .label = fmt::format("Heap {} ({:.1f} GiB)", heap.index, double(heap.size) / (1024.0 * 1024.0 * 1024.0)),
					.usageBytes = heap.usage,
					.budgetBytes = heap.budget,
					.attributedBytes = heap.attributed,
					.deviceLocal = heap.deviceLocal });
			for (const auto& line : a_snapshot.reconciliation)
				out.reconciliation.push_back({ .label = line.label,
					.bytes = line.bytes,
					.kind = static_cast<org::ui::MemoryReconciliationKind>(line.kind),
					.depth = line.depth,
					.note = line.note });
			return out;
		}

		// Rebuilt at most twice a second: the rows are thousands of strings.
		org::ui::MemorySnapshot g_snapshot;  // UI thread
		std::chrono::steady_clock::time_point g_snapshotTime{};

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
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration<double>(now - g_snapshotTime).count() >= 0.5) {
			g_snapshot = Snapshot(Vram::Current());
			g_snapshotTime = now;
		}
		g_widget.PushFrameSample(std::chrono::duration<double>(now - g_start).count(), g_snapshot.totalBytes);
		ImGui::SetNextWindowSize(ImVec2(1100.0f, 650.0f), ImGuiCond_FirstUseEver);
		// No frame-graph batches: the persistent graph's are its build's, not readable while it runs (the widget says so).
		g_widget.Draw(&g_windowOpen, &g_snapshot, nullptr);
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
			// A placement in an aliasing pool is the pool's memory, which the pool's own record counts.
			if (!record.CountsTowardTotal())
				continue;
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
