#include "Vram/DxvkMemory.h"

#include "Globals.h"
#include "RenderGraph/DxvkOrgInterop.h"

namespace Vram
{
	namespace
	{
		struct Exports
		{
			PFN_dxvkPushResourceOwner push = nullptr;
			PFN_dxvkPopResourceOwner pop = nullptr;
			PFN_dxvkSetResourceOwner setOwner = nullptr;
			PFN_dxvkRequestMemorySnapshot requestSnapshot = nullptr;
			PFN_dxvkReadMemorySnapshot readSnapshot = nullptr;
			PFN_dxvkGetMemoryHeapReports heapReports = nullptr;

			bool Complete() const { return push && pop && setOwner && requestSnapshot && readSnapshot && heapReports; }
		};

		template <class T>
		T Resolve(HMODULE a_module, const char* a_name)
		{
			return a_module ? reinterpret_cast<T>(reinterpret_cast<void*>(::GetProcAddress(a_module, a_name))) : nullptr;
		}

		// Resolved once dxvk_d3d11.dll is loaded (feature callbacks can run before DxvkLoader loads it); all or nothing, so a
		// DXVK build with only some of them is treated as one without.
		const Exports& Get()
		{
			static Exports exports;
			static std::atomic<bool> resolved = false;
			static std::mutex mutex;
			if (resolved.load(std::memory_order_acquire))
				return exports;

			static const Exports none;
			HMODULE d3d11 = ::GetModuleHandleW(L"dxvk_d3d11.dll");
			if (!d3d11)
				return none;

			std::lock_guard lock(mutex);
			if (!resolved.load(std::memory_order_relaxed)) {
				Exports e;
				e.push = Resolve<PFN_dxvkPushResourceOwner>(d3d11, "dxvkPushResourceOwner");
				e.pop = Resolve<PFN_dxvkPopResourceOwner>(d3d11, "dxvkPopResourceOwner");
				e.setOwner = Resolve<PFN_dxvkSetResourceOwner>(d3d11, "dxvkSetResourceOwner");
				e.requestSnapshot = Resolve<PFN_dxvkRequestMemorySnapshot>(d3d11, "dxvkRequestMemorySnapshot");
				e.readSnapshot = Resolve<PFN_dxvkReadMemorySnapshot>(d3d11, "dxvkReadMemorySnapshot");
				e.heapReports = Resolve<PFN_dxvkGetMemoryHeapReports>(d3d11, "dxvkGetMemoryHeapReports");
				if (!e.Complete()) {
					logger::info("[VRAM] dxvk_d3d11.dll has no VRAM attribution exports; D3D11 memory is not attributed");
					e = {};
				}
				exports = e;
				resolved.store(true, std::memory_order_release);
			}
			return exports;
		}
	}

	bool DxvkAvailable()
	{
		return Get().Complete();
	}

	OwnerScope::OwnerScope(const char* a_owner)
	{
		if (const auto& e = Get(); e.push && a_owner) {
			e.push(a_owner);
			m_pushed = true;
		}
	}

	OwnerScope::~OwnerScope()
	{
		if (m_pushed)
			Get().pop();
	}

	void SetOwner(IUnknown* a_resource, const char* a_owner, bool a_overwrite)
	{
		if (const auto& e = Get(); e.setOwner && a_resource && globals::d3d::device)
			e.setOwner(globals::d3d::device, a_resource, a_owner, a_overwrite ? TRUE : FALSE);
	}

	void RequestDxvkSnapshot()
	{
		if (const auto& e = Get(); e.requestSnapshot && globals::d3d::device)
			e.requestSnapshot(globals::d3d::device);
	}

	std::uint64_t ReadDxvkSnapshot(std::vector<DxvkMemoryResourceRow>& a_rows)
	{
		a_rows.clear();
		const auto& e = Get();
		if (!e.readSnapshot || !globals::d3d::device)
			return 0;

		// The count can grow between the two reads; retry until the copy fits.
		std::uint32_t count = 0;
		std::uint64_t serial = 0;
		if (FAILED(e.readSnapshot(globals::d3d::device, nullptr, 0, &count, &serial)))
			return 0;
		for (int attempt = 0; attempt < 4; ++attempt) {
			a_rows.resize(count + count / 8 + 64);
			std::uint32_t total = 0;
			const HRESULT hr = e.readSnapshot(globals::d3d::device, a_rows.data(), static_cast<std::uint32_t>(a_rows.size()), &total, &serial);
			if (FAILED(hr))
				break;
			if (hr == S_OK) {
				a_rows.resize(total);
				return serial;
			}
			count = total;
		}
		a_rows.clear();
		return 0;
	}

	std::vector<DxvkMemoryHeapReport> DxvkHeapReports()
	{
		std::vector<DxvkMemoryHeapReport> reports;
		const auto& e = Get();
		if (!e.heapReports || !globals::d3d::device)
			return reports;

		reports.resize(VK_MAX_MEMORY_HEAPS);
		for (auto& report : reports) {
			report = {};
			report.version = DXVK_MEMORY_REPORT_VERSION;
		}
		std::uint32_t count = 0;
		if (FAILED(e.heapReports(globals::d3d::device, reports.data(), static_cast<std::uint32_t>(reports.size()), &count)))
			count = 0;
		reports.resize(std::min<std::size_t>(count, reports.size()));
		return reports;
	}
}
