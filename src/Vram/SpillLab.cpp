#include "Vram/SpillLab.h"

#include "Features/Upscaling/VkDriverTrace.h"
#include "Globals.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "Vram/DxvkMemory.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace Vram::SpillLab
{
	namespace
	{
		enum class Pending
		{
			None,
			Demote,
			Spill,
			Clear,
			Check
		};
		Pending pending = Pending::None;
		std::uint64_t pendingAfter = 0;
		std::string filter;
		std::vector<std::uint64_t> held;

		std::string windowLabel = "-";
		std::vector<float> frameMs;
		std::chrono::steady_clock::time_point lastPresent{};

		std::string Lower(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			return a_text;
		}

		bool ClassMatches(std::string_view a_class, const DxvkMemoryResourceRow& a_row)
		{
			constexpr std::uint32_t kImageWritten = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			if (a_row.isImage) {
				if (a_class == "sampled")
					return !(a_row.usage & kImageWritten);
				if (a_class == "rt")
					return a_row.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
				if (a_class == "ds")
					return a_row.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
				if (a_class == "storage")
					return a_row.usage & VK_IMAGE_USAGE_STORAGE_BIT;
				return false;
			}
			if (a_class == "buffer")
				return true;
			if (a_class == "vb")
				return a_row.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
			if (a_class == "ib")
				return a_row.usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
			if (a_class == "ubo")
				return a_row.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			if (a_class == "ssbo")
				return a_row.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			if (a_class == "indirect")
				return a_row.usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
			return false;
		}

		bool Matches(const std::string& a_filter, const DxvkMemoryResourceRow& a_row)
		{
			const auto text = Lower(std::format("{}/{}", a_row.owner ? a_row.owner : "", a_row.name));
			for (const auto alternative : std::views::split(a_filter, '|')) {
				bool all = true;
				for (const auto termRange : std::views::split(alternative, '&')) {
					const auto term = Lower(std::string(termRange.begin(), termRange.end()));
					if (term.rfind("class:", 0) == 0)
						all = all && ClassMatches(std::string_view(term).substr(6), a_row);
					else
						all = all && text.find(term) != std::string::npos;
				}
				if (all)
					return true;
			}
			return false;
		}

		void Place(const std::vector<std::uint64_t>& a_cookies, bool a_toSystem, std::uint32_t& a_queued)
		{
			a_queued = 0;
			static const auto force = reinterpret_cast<PFN_dxvkForceResourcePlacement>(reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"dxvk_d3d11.dll"), "dxvkForceResourcePlacement")));
			if (force && globals::d3d::device && !a_cookies.empty())
				force(globals::d3d::device, static_cast<std::uint32_t>(a_cookies.size()), a_cookies.data(), a_toSystem ? TRUE : FALSE, &a_queued);
		}

		void RunPending(const std::vector<DxvkMemoryResourceRow>& a_rows)
		{
			if (pending == Pending::Demote) {
				std::vector<std::uint64_t> cookies;
				std::uint64_t bytes = 0;
				for (const auto& row : a_rows)
					if (row.heapIndex == 0 && row.isImage && !row.isSparse && Matches(filter, row)) {
						cookies.push_back(row.cookie);
						bytes += row.size;
					}
				static const auto mark = reinterpret_cast<PFN_dxvkSetResourcesSpillable>(reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"dxvk_d3d11.dll"), "dxvkSetResourcesSpillable")));
				std::uint32_t queued = 0;
				if (mark && globals::d3d::device && !cookies.empty())
					mark(globals::d3d::device, static_cast<std::uint32_t>(cookies.size()), cookies.data(), TRUE, &queued);
				logger::info("[SpillLab] demote '{}': {} images, {} MiB, {} can move", filter, cookies.size(), bytes >> 20, queued);
			} else if (pending == Pending::Spill) {
				std::vector<std::uint64_t> cookies;
				std::uint64_t bytes = 0;
				for (const auto& row : a_rows)
					if (row.heapIndex == 0 && !row.isSparse && Matches(filter, row)) {
						cookies.push_back(row.cookie);
						bytes += row.size;
					}
				std::uint32_t queued = 0;
				Place(cookies, true, queued);
				held.insert(held.end(), cookies.begin(), cookies.end());
				logger::info("[SpillLab] '{}': {} resources, {} MiB, {} can move", filter, cookies.size(), bytes >> 20, queued);
			} else if (pending == Pending::Clear) {
				std::uint32_t queued = 0;
				Place(held, false, queued);
				logger::info("[SpillLab] brought back {} of {}", queued, held.size());
				held.clear();
			} else if (pending == Pending::Check) {
				std::uint32_t moved = 0, stayed = 0;
				std::uint64_t movedBytes = 0, stayedBytes = 0;
				for (const auto& row : a_rows)
					if (std::ranges::find(held, row.cookie) != held.end()) {
						if (row.heapIndex == 0) {
							++stayed;
							stayedBytes += row.size;
						} else {
							++moved;
							movedBytes += row.size;
						}
					}
				logger::info("[SpillLab] held: {} moved ({} MiB), {} still in video memory ({} MiB)", moved, movedBytes >> 20, stayed, stayedBytes >> 20);
			}
			pending = Pending::None;
		}

		void Ask(Pending a_what)
		{
			std::vector<DxvkMemoryResourceRow> rows;
			pendingAfter = ReadDxvkSnapshot(rows);
			pending = a_what;
			RequestDxvkSnapshot();
		}
	}

	bool Command(const std::string& a_command)
	{
		if (a_command.rfind("dclf:spill=", 0) == 0) {
			filter = a_command.substr(11);
			Ask(Pending::Spill);
			return true;
		}
		if (a_command.rfind("dclf:demote=", 0) == 0) {
			filter = a_command.substr(12);
			Ask(Pending::Demote);
			return true;
		}
		if (a_command == "dclf:spill-clear") {
			Ask(Pending::Clear);
			return true;
		}
		if (a_command == "dclf:spill-check") {
			Ask(Pending::Check);
			return true;
		}
		if (a_command.rfind("dclf:frame-window=", 0) == 0) {
			if (windowLabel != "-" && !frameMs.empty()) {
				std::ranges::sort(frameMs);
				double total = 0.0;
				for (const float ms : frameMs)
					total += ms;
				logger::info("[SpillLab] window '{}': {} frames, mean {:.2f} ms, p50 {:.2f}, p95 {:.2f}, max {:.2f}", windowLabel, frameMs.size(), total / static_cast<double>(frameMs.size()),
					frameMs[frameMs.size() / 2], frameMs[frameMs.size() * 95 / 100], frameMs.back());
			}
			windowLabel = a_command.substr(18);
			frameMs.clear();
			return true;
		}
		if (a_command.rfind("dclf:ballast=", 0) == 0) {
			float priority = 1.0f;
			const auto mib = static_cast<std::uint32_t>(std::strtoul(a_command.c_str() + 13, nullptr, 10));
			if (const auto colon = a_command.find(':', 13); colon != std::string::npos)
				priority = std::strtof(a_command.c_str() + colon + 1, nullptr);
			logger::info("[SpillLab] ballast: holds {} MiB at priority {}", VkDriverTrace::SetBallast(mib, priority), priority);
			return true;
		}
		return false;
	}

	void OnPresent()
	{
		const auto now = std::chrono::steady_clock::now();
		if (windowLabel != "-" && lastPresent.time_since_epoch().count())
			frameMs.push_back(std::chrono::duration<float, std::milli>(now - lastPresent).count());
		lastPresent = now;
		if (pending != Pending::None) {
			std::vector<DxvkMemoryResourceRow> rows;
			if (ReadDxvkSnapshot(rows) > pendingAfter)
				RunPending(rows);
			else
				RequestDxvkSnapshot();
		}
	}
}
