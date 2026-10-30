#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Vram/VramLedger.h"

#include "Globals.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "Vram/Attribution.h"
#include "Vram/Debug.h"
#include "Vram/DxvkMemory.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/MemoryIntrospectionAPI.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Render/Runtime/IStatisticsService.h>
#endif

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace Vram
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		double MiB(std::uint64_t a_bytes) { return double(a_bytes) / (1024.0 * 1024.0); }

		std::int64_t Signed(std::uint64_t a_bytes) { return static_cast<std::int64_t>(a_bytes); }

		std::uint64_t Clamp(std::int64_t a_bytes) { return a_bytes > 0 ? static_cast<std::uint64_t>(a_bytes) : 0; }

		// ORG's part: its tracked resources, and its allocator's totals.
		struct OrgPart
		{
			std::vector<Row> rows;
			bool present = false;
			bool allocatorValid = false;
			std::uint64_t blockBytes = 0;
			std::uint64_t allocationBytes = 0;
			bool hasLedger = false;
			std::uint64_t ledgerBytes = 0;  // device-local vkAllocateMemory, exact (debug mode)
		};

		// This DLL's file name, lower case: frames in it are CS's.
		const std::string& OwnModule()
		{
			static const std::string name = [] {
				HMODULE module = nullptr;
				wchar_t path[MAX_PATH]{};
				if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCWSTR>(&OwnModule), &module) &&
					::GetModuleFileNameW(module, path, MAX_PATH)) {
					auto file = std::filesystem::path(path).filename().string();
					std::ranges::transform(file, file.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					return file;
				}
				return std::string();
			}();
			return name;
		}

		// A D3D11 resource's creation call site. Cached by cookie (a stack never changes).
		const Debug::CallSite& CreationSite(std::uint64_t a_cookie)
		{
			static std::unordered_map<std::uint64_t, Debug::CallSite> cache;
			auto it = cache.find(a_cookie);
			if (it == cache.end()) {
				if (cache.size() > 200000)
					cache.clear();
				it = cache.emplace(a_cookie, Debug::DxvkCreationSite(a_cookie)).first;
			}
			return it->second;
		}

		// An unattributed D3D11 resource's creator, from its creation stack.
		// What an unnamed D3D11 resource is, from its Vulkan usage (DXVK derives it from the D3D11 bind flags): "vertex buffer",
		// "render target", ...; a_origin ("D3D11 buffer") where the usage says nothing more.
		std::string UsageKind(const DxvkMemoryResourceRow& a_row)
		{
			if (a_row.isImage) {
				if (a_row.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
					return "depth stencil";
				if (a_row.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
					return "render target";
				if (a_row.usage & VK_IMAGE_USAGE_STORAGE_BIT)
					return "UAV texture";
			} else {
				if (a_row.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
					return "vertex buffer";
				if (a_row.usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
					return "index buffer";
				if (a_row.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
					return "constant buffer";
				if (a_row.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
					return "structured buffer";
			}
			return a_row.origin ? a_row.origin : "resource";
		}

		void AttributeByCallSite(Row& a_row, std::string_view a_origin, std::string_view a_kind)
		{
			const auto& site = CreationSite(a_row.uid);
			if (site.module.empty())
				return;
			a_row.callsite = site.text;
			if (site.module == "skyrimse.exe" || site.module == "skyrimvr.exe") {
				// The game's meshes (BSGraphics' geometry creation) are its vertex and index buffers.
				if (a_kind == "vertex buffer" || a_kind == "index buffer")
					a_row.owner = fmt::format("Game/Geometry/{}s", a_kind);
				else
					a_row.owner = fmt::format("Game/Unscoped/{}", a_origin);
				a_row.flags &= ~kRowUnattributed;
			} else if (site.module == OwnModule()) {
				a_row.owner = "CS/Unscoped";  // still a gap: CS code that creates outside any owner scope and without a name
			} else {
				a_row.owner = "Plugin/" + site.module;
				a_row.flags &= ~kRowUnattributed;
			}
		}

		// Debug mode: ORG's device memory by vkAllocateMemory call site, listed (not counted: the resources' rows count it).
		void ReadOrgLedger(OrgPart& a_part)
		{
			if (!Debug::HasOrgLedger())
				return;
			a_part.hasLedger = true;
			std::unordered_map<std::string, Row> bySite;
			for (const auto& allocation : Debug::OrgAllocations()) {
				if (allocation.deviceLocal)
					a_part.ledgerBytes += allocation.bytes;
				auto& row = bySite[allocation.site + (allocation.deviceLocal ? "" : " (host)")];
				if (row.name.empty()) {
					row.name = "vkAllocateMemory at " + allocation.site;
					row.owner = "ORG/Device memory";
					row.type = "Device memory";
					row.source = "ORG";
					row.category = "ORG/Device memory";
					row.callsite = allocation.site;
					row.deviceLocal = allocation.deviceLocal;
					row.flags = kRowNotCounted | kRowInternal;
				}
				row.bytes += allocation.bytes;
				++row.uid;  // the allocation count
			}
			for (auto& [site, row] : bySite) {
				row.detail = fmt::format("{} allocations", row.uid);
				a_part.rows.push_back(std::move(row));
			}
		}

#if defined(CS_HAS_RENDER_GRAPH)
		const char* OrgType(rhi::ResourceType a_type)
		{
			switch (a_type) {
			case rhi::ResourceType::Buffer:
				return "Buffer";
			case rhi::ResourceType::Texture1D:
			case rhi::ResourceType::Texture2D:
			case rhi::ResourceType::Texture3D:
				return "Texture";
			case rhi::ResourceType::AccelerationStructure:
				return "AccelStruct";
			default:
				return "Memory";
			}
		}

		OrgPart ReadOrg()
		{
			OrgPart part;
			auto* host = RenderGraphRuntime::Get().Host();
			auto* graph = host ? host->Graph() : nullptr;
			if (!graph)
				return part;
			part.present = true;

			std::vector<org::memory::ResourceMemoryRecord> records;
			graph->GetMemorySnapshotProvider().BuildSnapshot(records);
			part.rows.reserve(records.size());
			for (const auto& record : records) {
				Row row;
				row.name = !record.resourceName.empty() ? record.resourceName :
				           !record.identifier.empty()   ? record.identifier :
				           !record.usage.empty()        ? fmt::format("{} {}", record.usage, record.resourceID) :
				                                          "Resource " + std::to_string(record.resourceID);
				row.type = OrgType(record.resourceType);
				row.source = "ORG";
				row.category = "ORG/" + row.type;
				row.bytes = record.bytes;
				row.uid = record.resourceID;
				row.deviceLocal = record.heapType != rhi::HeapType::Upload && record.heapType != rhi::HeapType::Readback &&
				                  record.heapType != rhi::HeapType::HostCached;
				if (record.width)
					row.detail = fmt::format("{}x{}x{} mips {} format {}", record.width, record.height, record.arraySize, record.mipLevels,
						static_cast<int>(record.format));

				const bool aliasPool = record.resourceID == 0 && record.usage == "RenderGraph alias pools";
				if (!record.owner.empty())
					row.owner = record.owner;
				else if (aliasPool)
					row.owner = "ORG/Alias pools";
				else
					row.owner = OwnerFromOrgName(record.resourceName);
				if (row.owner.empty()) {
					row.owner = "Unattributed/ORG";
					row.flags |= kRowUnattributed;
				}
				if (aliasPool)
					row.flags |= kRowInternal;
				if (!record.CountsTowardTotal())
					row.flags |= kRowAliased | kRowNotCounted;
				part.rows.push_back(std::move(row));
			}

			if (const auto* stats = graph->GetStatisticsService()) {
				const auto budget = stats->GetMemoryBudgetStats();
				part.allocatorValid = budget.valid;
				part.blockBytes = budget.localBlockBytes;
				part.allocationBytes = budget.localAllocationBytes;
			}
			return part;
		}
#else
		OrgPart ReadOrg()
		{
			return {};
		}
#endif

		std::string ImageDetail(const DxvkMemoryResourceRow& a_row)
		{
			if (!a_row.isImage)
				return {};
			return fmt::format("{}x{}x{} mips {} layers {} VkFormat {}", a_row.width, a_row.height, a_row.depth, a_row.mipLevels, a_row.layers,
				a_row.format);
		}

		// DXVK's rows, attributed. Imported storage is another allocator's (ORG's buffers wrapped for D3D11): ORG's rows count it.
		std::vector<Row> AttributeDxvk(const std::vector<DxvkMemoryResourceRow>& a_rows, const std::vector<Heap>& a_heaps)
		{
			std::unordered_map<std::uint64_t, const DxvkMemoryResourceRow*> byCookie;
			byCookie.reserve(a_rows.size());
			for (const auto& row : a_rows)
				byCookie.emplace(row.cookie, &row);

			std::vector<Row> out;
			out.reserve(a_rows.size());
			for (const auto& src : a_rows) {
				Row row;
				row.bytes = src.size;
				row.uid = src.cookie;
				row.heapIndex = src.heapIndex;
				row.deviceLocal = src.heapIndex >= a_heaps.size() || a_heaps[src.heapIndex].deviceLocal;
				row.type = src.isImage ? "Texture" : "Buffer";
				row.detail = ImageDetail(src);

				// A texture's mapped subresource buffers are the texture's memory.
				const DxvkMemoryResourceRow* parent = nullptr;
				if (src.parentCookie)
					if (const auto it = byCookie.find(src.parentCookie); it != byCookie.end())
						parent = it->second;
				std::string name = src.name[0] ? std::string(src.name) : parent && parent->name[0] ? std::string(parent->name) :
				                                                                                     std::string();
				const char* scopeOwner = src.owner ? src.owner : parent ? parent->owner :
				                                                          nullptr;

				if (src.flags & DXVK_MEMORY_RESOURCE_IMPORTED)
					row.flags |= kRowImported | kRowNotCounted;

				if (!src.origin) {
					// DXVK's own resource: its kind, and the scope that caused it if any (a staging buffer for a feature's upload).
					row.source = "DXVK";
					row.flags |= kRowInternal;
					row.owner = std::string("DXVK/") + (src.kind ? src.kind : "Other");
					row.name = name.empty() ? (src.kind ? src.kind : "DXVK resource") : name;
					if (scopeOwner)
						row.name += fmt::format(" (during {})", scopeOwner);
				} else {
					row.source = "D3D11";
					const std::string kind = UsageKind(src);
					row.name = name.empty() ? fmt::format("(unnamed {} {})", kind, src.cookie) : name;
					// The developer's prefix is the most specific; then the scope it was created in.
					row.owner = OwnerFromD3DName(name);
					if (row.owner.empty())
						row.owner = OwnerFromOrgName(name);  // a D3D11 view of a render graph buffer (RenderGraphRuntime::WrapBuffer)
					if (row.owner.empty() && scopeOwner)
						row.owner = scopeOwner;
					if (row.owner.empty()) {
						row.owner = std::string("Unattributed/") + src.origin;
						row.flags |= kRowUnattributed;
						AttributeByCallSite(row, src.origin, kind);
					} else if (name.empty()) {
						// Owned but unnamed: where it was created names it.
						row.callsite = CreationSite(row.uid).text;
					}
				}
				row.category = row.source + "/" + row.type;
				out.push_back(std::move(row));
			}
			return out;
		}

		std::vector<Heap> ReadHeaps()
		{
			std::vector<Heap> heaps;
			for (const auto& report : DxvkHeapReports()) {
				Heap heap;
				heap.index = report.heapIndex;
				heap.deviceLocal = report.heapFlags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
				heap.hasBudget = report.hasVkBudget;
				heap.size = report.heapSize;
				heap.usage = report.hasVkBudget ? report.vkUsage : report.dxvkAllocated;
				heap.budget = report.hasVkBudget ? report.vkBudget : report.heapSize;
				heap.dxvkAllocated = report.dxvkAllocated;
				heap.dxvkUsed = report.dxvkUsed;
				heap.dxvkCache = report.cacheBytes;
				heap.dxvkOrgRetired = report.orgRetiredBytes;
				heaps.push_back(heap);
			}
			return heaps;
		}

		// From the driver's usage of the device-local heaps down to resources. Each total's children add up to it, with the part
		// nothing accounts for as an explicit residual.
		std::vector<Line> Reconcile(const Snapshot& a_snapshot, const OrgPart& a_org)
		{
			std::uint64_t usage = 0, allocated = 0, used = 0, cache = 0, retired = 0;
			bool anyHeap = false;
			for (const auto& heap : a_snapshot.heaps) {
				if (!heap.deviceLocal)
					continue;
				anyHeap = true;
				usage += heap.usage;
				allocated += heap.dxvkAllocated;
				used += heap.dxvkUsed;
				cache += heap.dxvkCache;
				retired += heap.dxvkOrgRetired;
			}

			std::uint64_t d3dAttributed = 0, d3dUnattributed = 0, dxvkInternal = 0, orgCounted = 0, orgAliasPools = 0;
			for (const auto& row : a_snapshot.rows) {
				if (!row.deviceLocal || (row.flags & kRowNotCounted))
					continue;
				if (row.source == "ORG") {
					orgCounted += row.bytes;
					if (row.flags & kRowInternal)
						orgAliasPools += row.bytes;
				} else if (row.flags & kRowInternal)
					dxvkInternal += row.bytes;
				else if (row.flags & kRowUnattributed)
					d3dUnattributed += row.bytes;
				else
					d3dAttributed += row.bytes;
			}

			std::vector<Line> lines;
			const std::uint64_t live = d3dAttributed + d3dUnattributed + dxvkInternal;
			const std::int64_t inFlight = Signed(used) - Signed(live) - Signed(cache) - Signed(retired);

			std::uint64_t orgAllocated = orgCounted;
			std::uint64_t orgSlack = 0, orgOutside = 0, orgUntracked = 0;
			if (a_org.allocatorValid) {
				orgSlack = Clamp(Signed(a_org.blockBytes) - Signed(a_org.allocationBytes));
				orgOutside = Clamp(Signed(orgCounted) - Signed(a_org.allocationBytes));
				orgUntracked = Clamp(Signed(a_org.allocationBytes) - Signed(orgCounted));
				orgAllocated = a_org.blockBytes + orgOutside;
			}
			// The ledger is exact: what tracked resources and the allocator's slack leave of it is untracked.
			if (a_org.hasLedger) {
				orgAllocated = a_org.ledgerBytes;
				orgOutside = 0;
				orgUntracked = Clamp(Signed(a_org.ledgerBytes) - Signed(orgCounted) - Signed(orgSlack));
			}

			if (anyHeap)
				lines.push_back({ "Device-local heap usage (driver)", usage, Kind::Total, 0,
					"VK_EXT_memory_budget's process-wide usage of the device-local heaps" });

			if (a_snapshot.hasDxvk) {
				lines.push_back({ "DXVK allocated", allocated, Kind::Total, 1, "VkDeviceMemory in DXVK's chunks and dedicated allocations" });
				lines.push_back({ "D3D11 resources, attributed", d3dAttributed, Kind::Attributed, 2, {} });
				lines.push_back({ "D3D11 resources, unattributed", d3dUnattributed, Kind::Residual, 2,
					"live resources created outside any owner scope and without a CS name; the list gives each one's call site" });
				lines.push_back({ "DXVK internal resources", dxvkInternal, Kind::Attributed, 2, "staging, descriptor heaps, scratch, ..." });
				lines.push_back({ "Allocation caches", cache, Kind::Residual, 2, "freed allocations DXVK keeps for reuse" });
				lines.push_back({ "Storages retired to ORG", retired, Kind::Residual, 2, "old storages of relocated resources ORG still holds" });
				lines.push_back({ "Renamed / in flight", Clamp(inFlight), Kind::Residual, 2,
					inFlight < 0 ? fmt::format("live resources exceed DXVK's used total by {:.1f} MB: the snapshot and the totals were sampled apart",
									   MiB(Clamp(-inFlight))) :
								   "discarded storages the GPU may still read, and local allocation caches" });
				lines.push_back({ "Free in chunks", Clamp(Signed(allocated) - Signed(used)), Kind::Residual, 2, "fragmentation and chunk headroom" });
			}

			if (a_org.present) {
				lines.push_back({ "ORG allocated", orgAllocated, Kind::Total, 1,
					a_org.hasLedger      ? "every device-local vkAllocateMemory the graph made (CS_VRAM_DEBUG's ledger)" :
					a_org.allocatorValid ? "its allocator's blocks, plus tracked resources outside them" :
										   "tracked resources (no allocator statistics)" });
				lines.push_back({ "ORG resources", orgCounted - orgAliasPools, Kind::Attributed, 2, {} });
				lines.push_back({ "ORG alias pools", orgAliasPools, Kind::Attributed, 2, "memory placed resources alias; they are listed but not counted" });
				if (a_org.allocatorValid || a_org.hasLedger) {
					if (a_org.allocatorValid)
						lines.push_back({ "Allocator slack", orgSlack, Kind::Residual, 2, "free space in the allocator's blocks" });
					lines.push_back({ "Allocations not tracked", orgUntracked, Kind::Residual, 2,
						a_org.hasLedger ? "device memory no tracked resource claims; the list's ORG/Device memory rows give each call site" :
										  "allocator allocations without a tracked resource (descriptor heaps, scratch, ...); CS_VRAM_DEBUG lists them" });
					if (orgOutside)
						lines.push_back({ "Tracked outside the allocator", orgOutside, Kind::Attributed, 2, "committed resources" });
				}
			}

			if (anyHeap) {
				const std::uint64_t accounted = (a_snapshot.hasDxvk ? allocated : 0) + orgAllocated;
				const std::int64_t opaque = Signed(usage) - Signed(accounted);
				lines.push_back({ "Not accounted by DXVK or ORG", Clamp(opaque), Kind::Opaque, 1,
					opaque < 0 ? fmt::format("DXVK and ORG account for {:.1f} MB more than the driver reports", MiB(Clamp(-opaque))) :
								 "swapchain, pipelines, descriptor and query pools, the driver's own, other APIs in the process" });
			}
			return lines;
		}

		Snapshot Build()
		{
			Snapshot snapshot;
			snapshot.heaps = ReadHeaps();

			std::vector<DxvkMemoryResourceRow> dxvkRows;
			snapshot.dxvkSerial = ReadDxvkSnapshot(dxvkRows);
			snapshot.hasDxvk = snapshot.dxvkSerial != 0;
			snapshot.rows = AttributeDxvk(dxvkRows, snapshot.heaps);

			auto org = ReadOrg();
			snapshot.hasOrg = org.present;
			snapshot.debug = Debug::Enabled();
			if (org.present)
				ReadOrgLedger(org);

			// ORG's rows carry no heap index: its device-local memory goes to the largest device-local heap.
			std::uint32_t orgHeap = ~0u;
			std::uint64_t largest = 0;
			for (const auto& heap : snapshot.heaps)
				if (heap.deviceLocal && heap.size > largest) {
					largest = heap.size;
					orgHeap = heap.index;
				}
			for (auto& row : org.rows) {
				if (row.deviceLocal)
					row.heapIndex = orgHeap;
				snapshot.rows.push_back(row);
			}

			for (const auto& row : snapshot.rows)
				if (!(row.flags & kRowNotCounted) && row.heapIndex < snapshot.heaps.size())
					snapshot.heaps[row.heapIndex].attributed += row.bytes;

			snapshot.reconciliation = Reconcile(snapshot, org);
			return snapshot;
		}

		std::mutex g_mutex;
		Snapshot g_snapshot;
		Clock::time_point g_built{};
		bool g_hasSnapshot = false;
	}

	Snapshot Current(double a_maxAgeSeconds)
	{
		std::lock_guard lock(g_mutex);
		const auto now = Clock::now();
		if (!g_hasSnapshot || std::chrono::duration<double>(now - g_built).count() >= a_maxAgeSeconds) {
			Debug::ProcessPendingTextures();
			// Targets the game creates later (the volumetric lighting volumes) are labeled here; CS's replacements keep their owners.
			TagGameRenderTargets(false);
			// DXVK's snapshot completes on its CS thread: read the last one, and ask for the next.
			g_snapshot = Build();
			RequestDxvkSnapshot();
			g_built = now;
			g_hasSnapshot = true;
		}
		return g_snapshot;
	}

	void TagGameRenderTargets(bool a_overwrite)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || !DxvkAvailable())
			return;

		// Owned "Game/<kind>/<name>"; named after the slot unless already named (a CS replacement's own name is kept). No "::"
		// in the name: it would read as a CS prefix (OwnerFromD3DName).
		auto tag = [a_overwrite](ID3D11DeviceChild* a_resource, std::string_view a_kind, std::string_view a_name, std::string_view a_suffix = {}) {
			if (!a_resource)
				return;
			SetOwner(a_resource, fmt::format("Game/{}/{}", a_kind, a_name).c_str(), a_overwrite);
			UINT size = 0;
			if (a_resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, nullptr) != S_OK || size == 0) {
				const auto name = fmt::format("{}{}", a_name, a_suffix);
				a_resource->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.c_str());
			}
		};

		for (std::size_t i = 0; i < RE::RENDER_TARGETS::kTOTAL; ++i) {
			const auto name = magic_enum::enum_name(static_cast<RE::RENDER_TARGETS::RENDER_TARGET>(i));
			const auto& target = renderer->GetRuntimeData().renderTargets[i];
			tag(target.texture, "RenderTarget", name);
			tag(target.textureCopy, "RenderTarget", name, " copy");
		}
		for (std::size_t i = 0; i < RE::RENDER_TARGETS_CUBEMAP::kTOTAL; ++i)
			tag(renderer->GetRendererData().cubemapRenderTargets[i].texture, "Cubemap",
				magic_enum::enum_name(static_cast<RE::RENDER_TARGETS_CUBEMAP::RENDER_TARGET_CUBEMAP>(i)));
		// Texture3DTargetData's first member is the texture (the game's 3D target creation, AE 0x140e453b0, writes it there).
		for (std::size_t i = 0; i < RE::RENDER_TARGETS_3D::kTOTAL; ++i)
			tag(reinterpret_cast<ID3D11Texture3D*>(renderer->GetRendererData().texture3DRenderTargets[i].unk00), "RenderTarget3D",
				magic_enum::enum_name(static_cast<RE::RENDER_TARGETS_3D::RENDER_TARGET_3D>(i)));
		for (std::size_t i = 0; i < RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL; ++i)
			tag(renderer->GetDepthStencilData().depthStencils[i].texture, "DepthStencil",
				magic_enum::enum_name(static_cast<RE::RENDER_TARGETS_DEPTHSTENCIL::RENDER_TARGET_DEPTHSTENCIL>(i)));

		if (auto* swapChain = reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain)) {
			winrt::com_ptr<ID3D11Texture2D> backBuffer;
			if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()))))
				tag(backBuffer.get(), "SwapChain", "Back buffer");
		}
	}

	std::string ReportLine(std::size_t a_owners)
	{
		const auto snapshot = Current();
		if (snapshot.rows.empty() && snapshot.heaps.empty())
			return {};

		std::unordered_map<std::string, std::uint64_t> owners;
		for (const auto& row : snapshot.rows)
			if (row.deviceLocal && !(row.flags & kRowNotCounted))
				owners[row.owner] += row.bytes;
		std::vector<std::pair<std::string, std::uint64_t>> sorted(owners.begin(), owners.end());
		std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });

		std::string largest;
		for (std::size_t i = 0; i < sorted.size() && i < a_owners; ++i)
			largest += fmt::format("{}{} {:.1f} MB", i ? ", " : "", sorted[i].first, MiB(sorted[i].second));

		std::string breakdown;
		for (const auto& line : snapshot.reconciliation)
			breakdown += fmt::format("{}{}{} {:.1f} MB", breakdown.empty() ? "" : "; ", std::string(static_cast<std::size_t>(line.depth), '>'),
				line.label, MiB(line.bytes));
		return fmt::format("VRAM attribution: {}; largest owners (device-local): {}", breakdown, largest);
	}

	std::filesystem::path WriteJson(const std::filesystem::path& a_directory)
	{
		const auto snapshot = Current(0.0);
		nlohmann::json json;
		json["dxvkSerial"] = snapshot.dxvkSerial;
		for (const auto& heap : snapshot.heaps)
			json["heaps"].push_back({ { "index", heap.index }, { "deviceLocal", heap.deviceLocal }, { "size", heap.size }, { "usage", heap.usage },
				{ "budget", heap.budget }, { "dxvkAllocated", heap.dxvkAllocated }, { "dxvkUsed", heap.dxvkUsed }, { "dxvkCache", heap.dxvkCache },
				{ "dxvkOrgRetired", heap.dxvkOrgRetired }, { "attributed", heap.attributed } });
		for (const auto& line : snapshot.reconciliation)
			json["reconciliation"].push_back({ { "label", line.label }, { "bytes", line.bytes }, { "depth", line.depth },
				{ "kind", static_cast<int>(line.kind) }, { "note", line.note } });
		for (const auto& row : snapshot.rows)
			json["rows"].push_back({ { "name", row.name }, { "owner", row.owner }, { "type", row.type }, { "source", row.source },
				{ "detail", row.detail }, { "callsite", row.callsite }, { "bytes", row.bytes }, { "uid", row.uid }, { "flags", row.flags }, { "deviceLocal", row.deviceLocal },
				{ "heap", row.heapIndex } });

		const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		auto path = a_directory / fmt::format("CommunityShaders-VRAM-{}.json", stamp);
		std::ofstream file(path, std::ios::binary);
		if (!file)
			return {};
		file << json.dump(1, '\t', false, nlohmann::json::error_handler_t::replace);
		return file ? path : std::filesystem::path();
	}
}
