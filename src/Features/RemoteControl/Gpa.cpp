#include "Features/RemoteControl/Gpa.h"

#include "DxvkLoader.h"
#include "Features/RemoteControl/GpuCounters.h"
#include "RenderGraph/DxvkOrgInterop.h"

#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace GpuCounters::detail
{
	namespace gpa
	{
		namespace
		{
			constexpr std::uint32_t kVendorAmd = 0x1002;
			constexpr const char* kTimeCounter = "GPUTime";

			struct Library
			{
				HMODULE module = nullptr;
				GpaFunctionTable table;
				bool ready = false;
				std::string unavailable = "GPUPerfAPI was not requested at startup (CS_GPA=1, or a CS_GPU_COUNTERS_RANGES capture)";
			};

			Library& Lib()
			{
				static Library library;
				return library;
			}

			std::string Env(const char* a_name)
			{
				char value[1024]{};
				const DWORD length = ::GetEnvironmentVariableA(a_name, value, sizeof(value));
				return length && length < sizeof(value) ? std::string(value, length) : std::string{};
			}

			// Whether any adapter is AMD's, through the system DXGI (DXVK's is dxvk_dxgi.dll): GPUPerfAPI is AMD's alone.
			bool AmdAdapterPresent()
			{
				const HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
				if (!dxgi)
					return false;
				using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
				const auto create = reinterpret_cast<CreateFactory>(::GetProcAddress(dxgi, "CreateDXGIFactory1"));
				winrt::com_ptr<IDXGIFactory1> factory;
				if (!create || FAILED(create(__uuidof(IDXGIFactory1), factory.put_void())))
					return false;
				for (UINT i = 0;; ++i) {
					winrt::com_ptr<IDXGIAdapter1> adapter;
					if (factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND)
						return false;
					DXGI_ADAPTER_DESC1 desc{};
					if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == kVendorAmd)
						return true;
				}
			}

			void LogMessage(GpaLoggingType a_type, const char* a_message)
			{
				if (a_type & kGpaLoggingError)
					logger::warn("[GPA] {}", a_message ? a_message : "");
				else
					logger::info("[GPA] {}", a_message ? a_message : "");
			}

			bool Requested()
			{
				const auto value = Env("CS_GPA");
				if (value == "0")
					return false;
				return value == "1" || GpuCounters::EnvironmentRequested();
			}

			double ToDouble(GpaUInt64 a_bits, GpaDataType a_type)
			{
				if (a_type == kGpaDataTypeFloat64) {
					double value = 0.0;
					std::memcpy(&value, &a_bits, sizeof(value));
					return value;
				}
				return static_cast<double>(a_bits);
			}

			bool Summed(GpaUsageType a_usage)
			{
				return a_usage != kGpaUsageTypeRatio && a_usage != kGpaUsageTypePercentage;
			}
		}

		const GpaFunctionTable* Table()
		{
			auto& lib = Lib();
			return lib.ready ? &lib.table : nullptr;
		}

		const std::string& Unavailable()
		{
			return Lib().unavailable;
		}

		bool Check(GpaStatus a_status, const char* a_operation)
		{
			if (a_status == kGpaStatusOk)
				return true;
			const char* text = Table() && Table()->GpaGetStatusAsStr ? Table()->GpaGetStatusAsStr(a_status) : nullptr;
			logger::warn("[GPA] {} failed: {} ({})", a_operation, text ? text : "unknown status", static_cast<int>(a_status));
			return false;
		}

		std::vector<MetricRequest> DefaultMetrics()
		{
			// Time; how busy each stage and unit was; vertex work and its culling; the vertex stage's instruction mix (the shadow
			// and depth passes are vertex-bound); memory and cache behaviour; depth and colour output.
			constexpr std::array kNames{ "GPUTime", "GPUBusy", "VsGsBusy", "VsGsTime", "PSBusy", "PSTime", "CSBusy", "PrimitiveAssemblyBusy",
				"PAStalledOnRasterizer", "PrimitivesIn", "CulledPrims", "ClippedPrims", "VsGsVALUInstCount", "VsGsSALUInstCount", "PSPixelsOut",
				"PSExportStalls", "TexUnitBusy", "MemUnitBusy", "MemUnitStalled", "WriteUnitStalled", "FetchSize", "WriteSize", "L0CacheHit",
				"L1CacheHit", "L2CacheHit", "ScalarCacheHit", "InstCacheHit", "DepthStencilTestBusy", "HiZTilesAccepted", "PreZTilesDetailCulled",
				"ZUnitStalled", "DBMemRead", "DBMemWritten", "CBMemRead", "CBMemWritten" };
			std::vector<MetricRequest> metrics;
			for (const char* name : kNames) {
				MetricRequest metric;
				metric.name = name;
				metric.metricType = kCounter;
				metric.rollupOp = kRollupSum;
				metrics.push_back(std::move(metric));
			}
			return metrics;
		}

		void Describe(GpaContextId a_context, DeviceInfo& a_device)
		{
			const auto* table = Table();
			const char* name = nullptr;
			if (table->GpaGetDeviceName(a_context, &name) == kGpaStatusOk && name)
				a_device.deviceName = name;
			GpaHwGeneration generation = kGpaHwGenerationNone;
			(void)table->GpaGetDeviceGeneration(a_context, &generation);
			switch (generation) {
			case kGpaHwGenerationGfx10:
				a_device.chipName = "gfx10";
				break;
			case kGpaHwGenerationGfx103:
				a_device.chipName = "gfx103";
				break;
			case kGpaHwGenerationGfx11:
				a_device.chipName = "gfx11";
				break;
			default:
				a_device.chipName = std::format("generation {}", static_cast<int>(generation));
				break;
			}
		}

		bool BuildSchedule(GpaContextId a_context, const std::vector<MetricRequest>& a_requested, Schedule& a_schedule, std::string& a_error)
		{
			const auto* table = Table();
			a_schedule = {};
			GpaSessionId probe = nullptr;
			if (!Check(table->GpaCreateSession(a_context, kGpaSessionSampleTypeDiscreteCounter, &probe), "GpaCreateSession")) {
				a_error = "could not create a GPUPerfAPI session";
				return false;
			}
			// The time counter first: it weights the averaged metrics. It is a metric like the others: on D3D11 it takes a pass of its
			// own, so it cannot join every group.
			std::vector<MetricRequest> requested;
			MetricRequest time;
			time.name = kTimeCounter;
			requested.push_back(time);
			for (const auto& metric : a_requested)
				if (metric.name != kTimeCounter)
					requested.push_back(metric);

			const auto passes = [&](const std::vector<std::size_t>& a_metrics) {
				(void)table->GpaDisableAllCounters(probe);
				for (const std::size_t m : a_metrics)
					if (table->GpaEnableCounter(probe, a_schedule.counters[m]) != kGpaStatusOk)
						return std::numeric_limits<GpaUInt32>::max();
				GpaUInt32 count = 0;
				return table->GpaGetPassCount(probe, &count) == kGpaStatusOk ? count : std::numeric_limits<GpaUInt32>::max();
			};

			for (const auto& metric : requested) {
				GpaUInt32 index = 0;
				GpaUsageType usage = kGpaUsageTypeLast;
				GpaDataType type = kGpaDataTypeLast;
				if (table->GpaGetCounterIndex(probe, metric.name.c_str(), &index) != kGpaStatusOk ||
					table->GpaGetCounterUsageType(probe, index, &usage) != kGpaStatusOk || table->GpaGetCounterDataType(probe, index, &type) != kGpaStatusOk) {
					if (metric.name == kTimeCounter) {
						a_error = "this GPU has no GPUTime counter";
						DeleteSession(probe, false);
						return false;
					}
					a_schedule.unsupported.push_back(metric);
					continue;
				}
				MetricRequest resolved;
				resolved.name = metric.name;
				resolved.metricType = Summed(usage) ? kPlainSummed : kPlainAveraged;
				const std::size_t m = a_schedule.metrics.size();
				a_schedule.metrics.push_back(std::move(resolved));
				a_schedule.counters.push_back(index);
				a_schedule.types.push_back(type);
				if (m == 0)
					a_schedule.timeMetric = 0;
				// Greedy: the metric joins the open group while that stays one pass; else it opens a group of its own, and a metric
				// that needs more than one pass even alone is not measured.
				if (!a_schedule.groups.empty()) {
					auto candidate = a_schedule.groups.back();
					candidate.push_back(m);
					if (passes(candidate) == 1) {
						a_schedule.groups.back() = std::move(candidate);
						continue;
					}
				}
				std::vector<std::size_t> alone{ m };
				if (passes(alone) == 1) {
					a_schedule.groups.push_back(std::move(alone));
					continue;
				}
				a_schedule.unsupported.push_back(metric);
				a_schedule.metrics.pop_back();
				a_schedule.counters.pop_back();
				a_schedule.types.pop_back();
			}
			DeleteSession(probe, false);
			return true;
		}

		GpaSessionId BeginPassSession(GpaContextId a_context, const Schedule& a_schedule, std::size_t a_group, std::string& a_error)
		{
			const auto* table = Table();
			GpaSessionId session = nullptr;
			if (!Check(table->GpaCreateSession(a_context, kGpaSessionSampleTypeDiscreteCounter, &session), "GpaCreateSession")) {
				a_error = "could not create a GPUPerfAPI session";
				return nullptr;
			}
			for (const std::size_t m : a_schedule.groups[a_group]) {
				if (!Check(table->GpaEnableCounter(session, a_schedule.counters[m]), "GpaEnableCounter")) {
					a_error = std::format("could not enable {}", a_schedule.metrics[m].name);
					DeleteSession(session, false);
					return nullptr;
				}
			}
			if (!Check(table->GpaBeginSession(session), "GpaBeginSession")) {
				a_error = "could not begin a GPUPerfAPI session";
				DeleteSession(session, false);
				return nullptr;
			}
			return session;
		}

		void DeleteSession(GpaSessionId a_session, bool a_end)
		{
			if (!a_session)
				return;
			const auto* table = Table();
			if (a_end)
				(void)Check(table->GpaEndSession(a_session), "GpaEndSession");
			(void)Check(table->GpaDeleteSession(a_session), "GpaDeleteSession");
		}

		void Merger::Reset(std::size_t a_metricCount)
		{
			metricCount = a_metricCount;
			rows.clear();
			index.clear();
		}

		bool Merger::AddPass(GpaSessionId a_session, const Schedule& a_schedule, std::size_t a_group, const std::vector<std::pair<GpaUInt32, std::string>>& a_samples,
			std::string& a_error)
		{
			const auto* table = Table();
			// The result's values come in enabled order: each one's metric.
			GpaUInt32 enabled = 0;
			if (!Check(table->GpaGetNumEnabledCounters(a_session, &enabled), "GpaGetNumEnabledCounters")) {
				a_error = "could not read the session's counters";
				return false;
			}
			std::vector<std::size_t> metricOf(enabled, metricCount);
			for (GpaUInt32 e = 0; e < enabled; ++e) {
				GpaUInt32 counter = 0;
				if (table->GpaGetEnabledIndex(a_session, e, &counter) != kGpaStatusOk)
					continue;
				for (const std::size_t m : a_schedule.groups[a_group])
					if (a_schedule.counters[m] == counter)
						metricOf[e] = m;
			}
			std::map<std::string, std::uint32_t> seen;
			std::vector<GpaUInt64> values;
			for (const auto& [id, name] : a_samples) {
				std::size_t size = 0;
				if (table->GpaGetSampleResultSize(a_session, id, &size) != kGpaStatusOk || size < enabled * sizeof(GpaUInt64))
					continue;
				values.assign(size / sizeof(GpaUInt64), 0);
				if (!Check(table->GpaGetSampleResult(a_session, id, size, values.data()), "GpaGetSampleResult"))
					continue;
				const std::uint32_t occurrence = seen[name]++;
				auto [at, inserted] = index.try_emplace({ name, occurrence }, rows.size());
				if (inserted)
					rows.push_back({ name, occurrence, std::vector<double>(metricCount, std::nan("")) });
				auto& row = rows[at->second];
				for (GpaUInt32 e = 0; e < enabled; ++e)
					if (const std::size_t m = metricOf[e]; m < metricCount)
						row.values[m] = ToDouble(values[e], a_schedule.types[m]);
			}
			return true;
		}

		void Merger::Emit(std::uint32_t a_sample, CaptureResult& a_result)
		{
			for (auto& row : rows) {
				RangeResult range;
				range.sample = a_sample;
				range.name = row.name;
				range.path = row.name;
				range.occurrence = row.occurrence;
				range.values = std::move(row.values);
				a_result.rangeResults.push_back(std::move(range));
			}
			rows.clear();
			index.clear();
		}
	}

	void GpaBeforeDeviceCreation(void* a_dxvkModule)
	{
		auto& lib = gpa::Lib();
		if (!gpa::Requested())
			return;
		if (!gpa::AmdAdapterPresent()) {
			lib.unavailable = "no AMD GPU";
			return;
		}
		const wchar_t* file = a_dxvkModule ? L"GPUPerfAPIVK-x64.dll" : L"GPUPerfAPIDX11-x64.dll";
		const auto dir = gpa::Env("CS_GPA_DIR");
		const std::filesystem::path path = (dir.empty() ? DxvkLoader::GetRuntimeDir() : std::filesystem::path(dir)) / file;
		lib.module = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!lib.module) {
			lib.unavailable = std::format("could not load {} (error {})", Util::PathHelpers::ToUtf8(path), ::GetLastError());
			logger::warn("[GPA] {}", lib.unavailable);
			return;
		}
		const auto getTable = reinterpret_cast<GpaGetFuncTablePtrType>(::GetProcAddress(lib.module, "GpaGetFuncTable"));
		if (!getTable || getTable(&lib.table) != kGpaStatusOk) {
			lib.unavailable = "GpaGetFuncTable failed (a GPUPerfAPI older or newer than its headers?)";
			logger::warn("[GPA] {}", lib.unavailable);
			return;
		}
		(void)lib.table.GpaRegisterLoggingCallback(kGpaLoggingError, &gpa::LogMessage);
		if (const GpaStatus status = lib.table.GpaInitialize(kGpaInitializeDefaultBit); status != kGpaStatusOk) {
			const char* text = lib.table.GpaGetStatusAsStr ? lib.table.GpaGetStatusAsStr(status) : nullptr;
			lib.unavailable = std::format("GpaInitialize failed: {}", text ? text : "unknown status");
			logger::warn("[GPA] {}", lib.unavailable);
			return;
		}
		lib.ready = true;
		lib.unavailable.clear();
		// DXVK's device enables the driver's counter interface (it knows nothing of these extensions; DXVK's export adds them
		// where the device has them).
		if (a_dxvkModule) {
			const auto request = reinterpret_cast<PFN_dxvkRequestDeviceExtensions>(::GetProcAddress(static_cast<HMODULE>(a_dxvkModule), "dxvkRequestDeviceExtensions"));
			const char* const extensions[] = { "VK_AMD_gpa_interface", "VK_AMD_shader_core_properties", "VK_AMD_shader_core_properties2" };
			if (!request || FAILED(request(static_cast<uint32_t>(std::size(extensions)), extensions))) {
				lib.ready = false;
				lib.unavailable = "this DXVK build cannot enable GPUPerfAPI's device extensions (dxvkRequestDeviceExtensions)";
				logger::warn("[GPA] {}", lib.unavailable);
				return;
			}
		}
		logger::info("[GPA] {} loaded and initialized before device creation", Util::PathHelpers::ToUtf8(path));
	}
}
