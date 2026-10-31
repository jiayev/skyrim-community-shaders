#include "Features/RemoteControl/NvPerfBackend.h"

#ifdef NVPERF_ENABLED

#	include "Globals.h"

#	include <nvperf_d3d11_host.h>
#	include <nvperf_d3d11_target.h>
#	include <nvperf_host.h>
#	include <nvperf_target.h>

#	include <algorithm>
#	include <optional>
#	include <unordered_map>
#	include <unordered_set>

/*
 * NvPerf's D3D11 backend: NVPW's D3D11 range profiler on the game's immediate context. A session per sample; each frame
 * is one in-app replay pass, ended before Present and begun after it; the perf events' ranges are pushed on the
 * immediate context; the counters are decoded after Present once the GPU finished every pass of the sample.
 */
namespace NvPerf::detail
{
	namespace
	{
		constexpr std::size_t kMaxRangesPerPass = 2048;
		constexpr std::size_t kAvgRangeNameLength = 96;
		constexpr std::uint32_t kMaxRangeNameLength = 256;
		constexpr std::uint32_t kFramesWithoutRangeLimit = 8;
		constexpr std::uint32_t kDecodeFrameLimit = 240;

		const char* StatusName(NVPA_Status a_status)
		{
			const char* statusName = nullptr;
			const char* comment = nullptr;
			NVPW_NVPAStatusToString(a_status, &statusName, &comment);
			return statusName ? statusName : "NVPA_STATUS_UNKNOWN";
		}

		bool Failed(NVPA_Status a_status, const char* a_operation)
		{
			if (a_status == NVPA_STATUS_SUCCESS)
				return false;
			logger::warn("[NvPerf] {} failed with {}", a_operation, StatusName(a_status));
			return true;
		}

		struct SelectedMetric
		{
			MetricRequest spec;
			NVPW_MetricEvalRequest request{};
		};

		// The profiler's state of the capture in flight, beside the front end's Capture.
		struct D3D11Capture
		{
			std::vector<SelectedMetric> metrics;
			std::vector<std::uint8_t> configImage;
			std::vector<std::uint8_t> counterDataPrefix;
			std::vector<std::uint8_t> counterDataImage;
			std::vector<std::uint8_t> counterDataScratch;
			std::size_t traceBufferSize = 0;
			std::size_t traceBufferCount = 0;
			std::uint32_t decodeFrames = 0;
			std::uint32_t framesWithoutRange = 0;
			std::uint64_t rangesThisPass = 0;
			bool sessionActive = false;
			bool passActive = false;
		};

		ID3D11DeviceContext* Context()
		{
			return globals::d3d::context;
		}

		NVPW_MetricsEvaluator* CreateEvaluator(std::vector<std::uint8_t>& a_scratch, const char* a_chipName, const std::vector<std::uint8_t>* a_image)
		{
			NVPW_D3D11_MetricsEvaluator_CalculateScratchBufferSize_Params size{ NVPW_D3D11_MetricsEvaluator_CalculateScratchBufferSize_Params_STRUCT_SIZE };
			size.pChipName = a_chipName;
			if (Failed(NVPW_D3D11_MetricsEvaluator_CalculateScratchBufferSize(&size), "NVPW_D3D11_MetricsEvaluator_CalculateScratchBufferSize"))
				return nullptr;
			a_scratch.assign(size.scratchBufferSize, 0);
			NVPW_D3D11_MetricsEvaluator_Initialize_Params init{ NVPW_D3D11_MetricsEvaluator_Initialize_Params_STRUCT_SIZE };
			init.pScratchBuffer = a_scratch.data();
			init.scratchBufferSize = a_scratch.size();
			init.pChipName = a_image ? nullptr : a_chipName;
			init.pCounterDataImage = a_image ? a_image->data() : nullptr;
			init.counterDataImageSize = a_image ? a_image->size() : 0;
			if (Failed(NVPW_D3D11_MetricsEvaluator_Initialize(&init), "NVPW_D3D11_MetricsEvaluator_Initialize"))
				return nullptr;
			return init.pMetricsEvaluator;
		}

		void DestroyEvaluator(NVPW_MetricsEvaluator* a_evaluator)
		{
			NVPW_MetricsEvaluator_Destroy_Params destroy{ NVPW_MetricsEvaluator_Destroy_Params_STRUCT_SIZE };
			destroy.pMetricsEvaluator = a_evaluator;
			(void)NVPW_MetricsEvaluator_Destroy(&destroy);
		}

		void DestroyRawConfig(NVPW_RawCounterConfig* a_config)
		{
			NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
			destroy.pRawCounterConfig = a_config;
			(void)NVPW_RawCounterConfig_Destroy(&destroy);
		}

		std::optional<std::size_t> FindMetricIndex(NVPW_MetricsEvaluator* a_evaluator, const MetricRequest& a_spec)
		{
			NVPW_MetricsEvaluator_GetMetricNames_Params names{ NVPW_MetricsEvaluator_GetMetricNames_Params_STRUCT_SIZE };
			names.pMetricsEvaluator = a_evaluator;
			names.metricType = a_spec.metricType;
			if (Failed(NVPW_MetricsEvaluator_GetMetricNames(&names), "NVPW_MetricsEvaluator_GetMetricNames"))
				return std::nullopt;
			for (std::size_t i = 0; i < names.numMetrics; ++i) {
				const char* metricName = names.pMetricNames + names.pMetricNameBeginIndices[i];
				if (metricName && a_spec.name == metricName)
					return i;
			}
			return std::nullopt;
		}

		std::vector<std::uint8_t> CounterAvailability()
		{
			std::vector<std::uint8_t> image;
			NVPW_D3D11_Profiler_DeviceContext_GetCounterAvailability_Params params{ NVPW_D3D11_Profiler_DeviceContext_GetCounterAvailability_Params_STRUCT_SIZE };
			params.pDeviceContext = Context();
			if (Failed(NVPW_D3D11_Profiler_DeviceContext_GetCounterAvailability(&params), "NVPW_D3D11_Profiler_DeviceContext_GetCounterAvailability(size)"))
				return image;
			image.resize(params.counterAvailabilityImageSize);
			params.pCounterAvailabilityImage = image.data();
			if (Failed(NVPW_D3D11_Profiler_DeviceContext_GetCounterAvailability(&params), "NVPW_D3D11_Profiler_DeviceContext_GetCounterAvailability"))
				image.clear();
			return image;
		}

		// The metrics the chip has, the raw counters behind them, and the configuration and counter data prefix that
		// collect them.
		bool BuildConfig(Capture& a_capture, D3D11Capture& a_d3d, const std::string& a_chipName, std::string& a_error)
		{
			const char* chip = a_chipName.c_str();
			std::vector<std::uint8_t> scratch;
			NVPW_MetricsEvaluator* evaluator = CreateEvaluator(scratch, chip, nullptr);
			if (!evaluator) {
				a_error = "could not create the metrics evaluator";
				return false;
			}
			std::unordered_set<std::string> rawCounterNames;
			const auto requested = a_capture.configuration.metrics.empty() ? DefaultMetrics() : a_capture.configuration.metrics;
			for (const MetricRequest& spec : requested) {
				const auto index = FindMetricIndex(evaluator, spec);
				if (!index) {
					logger::warn("[NvPerf] metric '{}' is unavailable on chip '{}'", FormatMetric(spec), a_chipName);
					a_capture.result.unsupportedMetrics.push_back(spec);
					continue;
				}
				SelectedMetric metric{};
				metric.spec = spec;
				metric.request.metricIndex = *index;
				metric.request.metricType = spec.metricType;
				metric.request.rollupOp = spec.rollupOp;
				metric.request.submetric = spec.submetric;
				NVPW_MetricsEvaluator_GetMetricRawDependencies_Params deps{ NVPW_MetricsEvaluator_GetMetricRawDependencies_Params_STRUCT_SIZE };
				deps.pMetricsEvaluator = evaluator;
				deps.pMetricEvalRequests = &metric.request;
				deps.numMetricEvalRequests = 1;
				deps.metricEvalRequestStructSize = NVPW_MetricEvalRequest_STRUCT_SIZE;
				deps.metricEvalRequestStrideSize = sizeof(NVPW_MetricEvalRequest);
				if (Failed(NVPW_MetricsEvaluator_GetMetricRawDependencies(&deps), "NVPW_MetricsEvaluator_GetMetricRawDependencies(count)"))
					continue;
				std::vector<const char*> dependencies(deps.numRawDependencies);
				deps.ppRawDependencies = dependencies.data();
				deps.numRawDependencies = dependencies.size();
				if (Failed(NVPW_MetricsEvaluator_GetMetricRawDependencies(&deps), "NVPW_MetricsEvaluator_GetMetricRawDependencies"))
					continue;
				for (const char* dependency : dependencies)
					if (dependency)
						rawCounterNames.insert(dependency);
				a_d3d.metrics.push_back(metric);
			}
			DestroyEvaluator(evaluator);

			std::vector<NVPW_RawCounterRequest> rawCounters;
			rawCounters.reserve(rawCounterNames.size());
			for (const std::string& name : rawCounterNames) {
				NVPW_RawCounterRequest request{};
				request.pRawCounterName = name.c_str();
				request.domain = NVPW_RAW_COUNTER_DOMAIN_INVALID;
				request.keepInstances = false;
				rawCounters.push_back(request);
			}
			if (a_d3d.metrics.empty() || rawCounters.empty()) {
				a_error = "none of the requested metrics is available on this GPU";
				return false;
			}

			NVPW_D3D11_RawCounterConfig_Create_Params create{ NVPW_D3D11_RawCounterConfig_Create_Params_STRUCT_SIZE };
			create.pChipName = chip;
			create.activityKind = NVPA_ACTIVITY_KIND_PROFILER;
			if (Failed(NVPW_D3D11_RawCounterConfig_Create(&create), "NVPW_D3D11_RawCounterConfig_Create")) {
				a_error = "could not create the counter configuration";
				return false;
			}
			NVPW_RawCounterConfig* config = create.pRawCounterConfig;
			const auto availability = CounterAvailability();
			if (!availability.empty()) {
				NVPW_RawCounterConfig_SetCounterAvailability_Params set{ NVPW_RawCounterConfig_SetCounterAvailability_Params_STRUCT_SIZE };
				set.pRawCounterConfig = config;
				set.pCounterAvailabilityImage = availability.data();
				(void)NVPW_RawCounterConfig_SetCounterAvailability(&set);
			}
			NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains_Params domainCount{ NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains_Params_STRUCT_SIZE };
			domainCount.pRawCounterConfig = config;
			std::vector<NVPW_RawCounterDomain> domains;
			bool built = !Failed(NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains(&domainCount), "NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains(count)");
			if (built) {
				domains.resize(domainCount.numAvailableDomains);
				NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains_Params domainValues{ NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains_Params_STRUCT_SIZE };
				domainValues.pRawCounterConfig = config;
				domainValues.numAvailableDomains = domains.size();
				domainValues.pAvailableDomains = domains.data();
				built = !Failed(NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains(&domainValues), "NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains");
				domains.resize(domainValues.numAvailableDomains);
			}
			if (built) {
				NVPW_RawCounterConfig_BeginPassGroup_Params begin{ NVPW_RawCounterConfig_BeginPassGroup_Params_STRUCT_SIZE };
				begin.pRawCounterConfig = config;
				begin.numDomains = domains.size();
				begin.pDomains = domains.data();
				NVPW_RawCounterConfig_AddRawCounters_Params add{ NVPW_RawCounterConfig_AddRawCounters_Params_STRUCT_SIZE };
				add.pRawCounterConfig = config;
				add.rawCounterRequestStructSize = NVPW_RAW_COUNTER_REQUEST_STRUCT_SIZE;
				add.numRawCounterRequests = rawCounters.size();
				add.pRawCounterRequests = rawCounters.data();
				NVPW_RawCounterConfig_EndPassGroup_Params end{ NVPW_RawCounterConfig_EndPassGroup_Params_STRUCT_SIZE };
				end.pRawCounterConfig = config;
				end.numDomains = domains.size();
				end.pDomains = domains.data();
				NVPW_RawCounterConfig_GenerateConfigImage_Params generate{ NVPW_RawCounterConfig_GenerateConfigImage_Params_STRUCT_SIZE };
				generate.pRawCounterConfig = config;
				built = !Failed(NVPW_RawCounterConfig_BeginPassGroup(&begin), "NVPW_RawCounterConfig_BeginPassGroup") &&
				        !Failed(NVPW_RawCounterConfig_AddRawCounters(&add), "NVPW_RawCounterConfig_AddRawCounters") &&
				        !Failed(NVPW_RawCounterConfig_EndPassGroup(&end), "NVPW_RawCounterConfig_EndPassGroup") &&
				        !Failed(NVPW_RawCounterConfig_GenerateConfigImage(&generate), "NVPW_RawCounterConfig_GenerateConfigImage");
			}
			if (built) {
				NVPW_RawCounterConfig_GetConfigImage_Params image{ NVPW_RawCounterConfig_GetConfigImage_Params_STRUCT_SIZE };
				image.pRawCounterConfig = config;
				built = !Failed(NVPW_RawCounterConfig_GetConfigImage(&image), "NVPW_RawCounterConfig_GetConfigImage(size)");
				if (built) {
					a_d3d.configImage.resize(image.bytesCopied);
					image.bytesAllocated = a_d3d.configImage.size();
					image.pBuffer = a_d3d.configImage.data();
					built = !Failed(NVPW_RawCounterConfig_GetConfigImage(&image), "NVPW_RawCounterConfig_GetConfigImage");
				}
			}
			std::size_t passes = 0;
			if (built) {
				NVPW_RawCounterConfig_GetNumPasses_Params numPasses{ NVPW_RawCounterConfig_GetNumPasses_Params_STRUCT_SIZE };
				numPasses.pRawCounterConfig = config;
				built = !Failed(NVPW_RawCounterConfig_GetNumPasses(&numPasses), "NVPW_RawCounterConfig_GetNumPasses");
				passes = numPasses.numPasses;
			}
			DestroyRawConfig(config);
			if (!built || !passes) {
				a_error = "could not build the counter configuration";
				return false;
			}
			a_capture.passesPerSample = passes * a_capture.configuration.nestingLevels;

			NVPW_CounterDataBuilder_Create_Params builderCreate{ NVPW_CounterDataBuilder_Create_Params_STRUCT_SIZE };
			builderCreate.pChipName = chip;
			if (Failed(NVPW_CounterDataBuilder_Create(&builderCreate), "NVPW_CounterDataBuilder_Create")) {
				a_error = "could not create the counter data builder";
				return false;
			}
			NVPA_CounterDataBuilder* builder = builderCreate.pCounterDataBuilder;
			NVPW_CounterDataBuilder_AddRawCounters_Params builderAdd{ NVPW_CounterDataBuilder_AddRawCounters_Params_STRUCT_SIZE };
			builderAdd.pCounterDataBuilder = builder;
			builderAdd.rawCounterRequestStructSize = NVPW_RAW_COUNTER_REQUEST_STRUCT_SIZE;
			builderAdd.numRawCounterRequests = rawCounters.size();
			builderAdd.pRawCounterRequests = rawCounters.data();
			NVPW_CounterDataBuilder_GetCounterDataPrefix_Params prefix{ NVPW_CounterDataBuilder_GetCounterDataPrefix_Params_STRUCT_SIZE };
			prefix.pCounterDataBuilder = builder;
			bool prefixOk = !Failed(NVPW_CounterDataBuilder_AddRawCounters(&builderAdd), "NVPW_CounterDataBuilder_AddRawCounters") &&
			                !Failed(NVPW_CounterDataBuilder_GetCounterDataPrefix(&prefix), "NVPW_CounterDataBuilder_GetCounterDataPrefix(size)");
			if (prefixOk) {
				a_d3d.counterDataPrefix.resize(prefix.bytesCopied);
				prefix.bytesAllocated = a_d3d.counterDataPrefix.size();
				prefix.pBuffer = a_d3d.counterDataPrefix.data();
				prefixOk = !Failed(NVPW_CounterDataBuilder_GetCounterDataPrefix(&prefix), "NVPW_CounterDataBuilder_GetCounterDataPrefix");
			}
			NVPW_CounterDataBuilder_Destroy_Params destroy{ NVPW_CounterDataBuilder_Destroy_Params_STRUCT_SIZE };
			destroy.pCounterDataBuilder = builder;
			(void)NVPW_CounterDataBuilder_Destroy(&destroy);
			if (!prefixOk) {
				a_error = "could not build the counter data prefix";
				return false;
			}

			NVPW_D3D11_Profiler_CalcTraceBufferSize_Params traceSize{ NVPW_D3D11_Profiler_CalcTraceBufferSize_Params_STRUCT_SIZE };
			traceSize.maxRangesPerPass = kMaxRangesPerPass;
			traceSize.avgRangeNameLength = kAvgRangeNameLength;
			if (Failed(NVPW_D3D11_Profiler_CalcTraceBufferSize(&traceSize), "NVPW_D3D11_Profiler_CalcTraceBufferSize")) {
				a_error = "could not size the trace buffer";
				return false;
			}
			a_d3d.traceBufferSize = traceSize.traceBufferSize;
			// One per pass in flight: every pass is submitted before the first is decoded.
			a_d3d.traceBufferCount = std::max<std::size_t>(a_capture.passesPerSample + 2, 5);
			logger::info("[NvPerf] capture {} configured: chip '{}', {} metrics, {} raw counters, {} passes per sample", a_capture.id, a_chipName,
				a_d3d.metrics.size(), rawCounters.size(), a_capture.passesPerSample);
			return true;
		}

		bool InitializeCounterData(D3D11Capture& a_d3d)
		{
			NVPW_D3D11_Profiler_CounterDataImageOptions options{ NVPW_D3D11_Profiler_CounterDataImageOptions_STRUCT_SIZE };
			options.pCounterDataPrefix = a_d3d.counterDataPrefix.data();
			options.counterDataPrefixSize = a_d3d.counterDataPrefix.size();
			options.maxNumRanges = static_cast<std::uint32_t>(kMaxRangesPerPass);
			options.maxNumRangeTreeNodes = static_cast<std::uint32_t>(kMaxRangesPerPass);
			options.maxRangeNameLength = kMaxRangeNameLength;
			NVPW_D3D11_Profiler_CounterDataImage_CalculateSize_Params size{ NVPW_D3D11_Profiler_CounterDataImage_CalculateSize_Params_STRUCT_SIZE };
			size.counterDataImageOptionsSize = NVPW_D3D11_Profiler_CounterDataImageOptions_STRUCT_SIZE;
			size.pOptions = &options;
			if (Failed(NVPW_D3D11_Profiler_CounterDataImage_CalculateSize(&size), "NVPW_D3D11_Profiler_CounterDataImage_CalculateSize"))
				return false;
			a_d3d.counterDataImage.assign(size.counterDataImageSize, 0);
			NVPW_D3D11_Profiler_CounterDataImage_Initialize_Params init{ NVPW_D3D11_Profiler_CounterDataImage_Initialize_Params_STRUCT_SIZE };
			init.counterDataImageOptionsSize = NVPW_D3D11_Profiler_CounterDataImageOptions_STRUCT_SIZE;
			init.pOptions = &options;
			init.counterDataImageSize = a_d3d.counterDataImage.size();
			init.pCounterDataImage = a_d3d.counterDataImage.data();
			if (Failed(NVPW_D3D11_Profiler_CounterDataImage_Initialize(&init), "NVPW_D3D11_Profiler_CounterDataImage_Initialize"))
				return false;
			NVPW_D3D11_Profiler_CounterDataImage_CalculateScratchBufferSize_Params scratchSize{
				NVPW_D3D11_Profiler_CounterDataImage_CalculateScratchBufferSize_Params_STRUCT_SIZE
			};
			scratchSize.counterDataImageSize = a_d3d.counterDataImage.size();
			scratchSize.pCounterDataImage = a_d3d.counterDataImage.data();
			if (Failed(NVPW_D3D11_Profiler_CounterDataImage_CalculateScratchBufferSize(&scratchSize),
					"NVPW_D3D11_Profiler_CounterDataImage_CalculateScratchBufferSize"))
				return false;
			a_d3d.counterDataScratch.assign(scratchSize.counterDataScratchBufferSize, 0);
			NVPW_D3D11_Profiler_CounterDataImage_InitializeScratchBuffer_Params scratchInit{
				NVPW_D3D11_Profiler_CounterDataImage_InitializeScratchBuffer_Params_STRUCT_SIZE
			};
			scratchInit.counterDataImageSize = a_d3d.counterDataImage.size();
			scratchInit.pCounterDataImage = a_d3d.counterDataImage.data();
			scratchInit.counterDataScratchBufferSize = a_d3d.counterDataScratch.size();
			scratchInit.pCounterDataScratchBuffer = a_d3d.counterDataScratch.data();
			return !Failed(NVPW_D3D11_Profiler_CounterDataImage_InitializeScratchBuffer(&scratchInit),
				"NVPW_D3D11_Profiler_CounterDataImage_InitializeScratchBuffer");
		}

		// A session per sample: its counter data, then the configuration every pass of it uses.
		bool BeginSession(Capture& a_capture, D3D11Capture& a_d3d)
		{
			if (!InitializeCounterData(a_d3d))
				return false;
			NVPW_D3D11_Profiler_DeviceContext_BeginSession_Params begin{ NVPW_D3D11_Profiler_DeviceContext_BeginSession_Params_STRUCT_SIZE };
			begin.pDeviceContext = Context();
			begin.numTraceBuffers = a_d3d.traceBufferCount;
			begin.traceBufferSize = a_d3d.traceBufferSize;
			begin.maxRangesPerPass = kMaxRangesPerPass;
			begin.maxLaunchesPerPass = kMaxRangesPerPass;
			if (const auto status = NVPW_D3D11_Profiler_DeviceContext_BeginSession(&begin); status != NVPA_STATUS_SUCCESS) {
				logger::warn("[NvPerf] NVPW_D3D11_Profiler_DeviceContext_BeginSession failed with {}{}", StatusName(status),
					status == NVPA_STATUS_INSUFFICIENT_PRIVILEGE ? " (enable GPU performance counters for all users: https://developer.nvidia.com/ERR_NVGPUCTRPERM)" :
					status == NVPA_STATUS_RESOURCE_UNAVAILABLE   ? " (another profiler session is active on this GPU)" :
																   "");
				return false;
			}
			a_d3d.sessionActive = true;
			// In-app replay: the profiler steps through the configuration's passes itself, from pass 0.
			NVPW_D3D11_Profiler_DeviceContext_SetConfig_Params config{ NVPW_D3D11_Profiler_DeviceContext_SetConfig_Params_STRUCT_SIZE };
			config.pDeviceContext = Context();
			config.pConfig = a_d3d.configImage.data();
			config.configSize = a_d3d.configImage.size();
			config.minNestingLevel = 1;
			config.numNestingLevels = a_capture.configuration.nestingLevels;
			config.passIndex = 0;
			config.targetNestingLevel = 1;
			if (Failed(NVPW_D3D11_Profiler_DeviceContext_SetConfig(&config), "NVPW_D3D11_Profiler_DeviceContext_SetConfig"))
				return false;
			a_capture.passesSubmitted = 0;
			a_d3d.decodeFrames = 0;
			return true;
		}

		std::vector<std::string> RangePath(const std::vector<std::uint8_t>& a_image, std::size_t a_rangeIndex)
		{
			NVPW_Profiler_CounterData_GetRangeDescriptions_Params desc{ NVPW_Profiler_CounterData_GetRangeDescriptions_Params_STRUCT_SIZE };
			desc.pCounterDataImage = a_image.data();
			desc.rangeIndex = a_rangeIndex;
			if (Failed(NVPW_Profiler_CounterData_GetRangeDescriptions(&desc), "NVPW_Profiler_CounterData_GetRangeDescriptions(count)"))
				return {};
			std::vector<const char*> descriptions(desc.numDescriptions);
			desc.ppDescriptions = descriptions.data();
			desc.numDescriptions = descriptions.size();
			if (Failed(NVPW_Profiler_CounterData_GetRangeDescriptions(&desc), "NVPW_Profiler_CounterData_GetRangeDescriptions"))
				return {};
			std::vector<std::string> path;
			for (const char* description : descriptions)
				path.emplace_back(description ? description : "<unnamed>");
			return path;
		}

		bool EvaluateRanges(Capture& a_capture, const D3D11Capture& a_d3d, const std::string& a_chipName)
		{
			NVPW_CounterData_GetNumRanges_Params numRanges{ NVPW_CounterData_GetNumRanges_Params_STRUCT_SIZE };
			numRanges.pCounterDataImage = a_d3d.counterDataImage.data();
			if (Failed(NVPW_CounterData_GetNumRanges(&numRanges), "NVPW_CounterData_GetNumRanges"))
				return false;
			if (!numRanges.numRanges)
				return true;
			std::vector<std::uint8_t> scratch;
			NVPW_MetricsEvaluator* evaluator = CreateEvaluator(scratch, a_chipName.c_str(), &a_d3d.counterDataImage);
			if (!evaluator)
				return false;
			std::vector<NVPW_MetricEvalRequest> requests;
			for (const auto& metric : a_d3d.metrics)
				requests.push_back(metric.request);
			std::vector<double> values(requests.size());
			std::unordered_map<std::string, std::uint32_t> occurrences;
			for (std::size_t rangeIndex = 0; rangeIndex < numRanges.numRanges; ++rangeIndex) {
				std::ranges::fill(values, 0.0);
				NVPW_MetricsEvaluator_EvaluateToGpuValues_Params eval{ NVPW_MetricsEvaluator_EvaluateToGpuValues_Params_STRUCT_SIZE };
				eval.pMetricsEvaluator = evaluator;
				eval.pMetricEvalRequests = requests.data();
				eval.numMetricEvalRequests = requests.size();
				eval.metricEvalRequestStructSize = NVPW_MetricEvalRequest_STRUCT_SIZE;
				eval.metricEvalRequestStrideSize = sizeof(NVPW_MetricEvalRequest);
				eval.pCounterDataImage = a_d3d.counterDataImage.data();
				eval.counterDataImageSize = a_d3d.counterDataImage.size();
				eval.rangeIndex = rangeIndex;
				eval.pMetricValues = values.data();
				if (Failed(NVPW_MetricsEvaluator_EvaluateToGpuValues(&eval), "NVPW_MetricsEvaluator_EvaluateToGpuValues"))
					continue;
				const auto path = RangePath(a_d3d.counterDataImage, rangeIndex);
				RangeResult range;
				range.sample = a_capture.sample;
				range.name = path.empty() ? "<unnamed>" : path.back();
				for (const auto& part : path)
					range.path += range.path.empty() ? part : " / " + part;
				if (!path.empty())
					range.enclosing.assign(path.begin(), path.end() - 1);
				range.occurrence = occurrences[range.path]++;
				range.values = values;
				a_capture.result.rangeResults.push_back(std::move(range));
			}
			DestroyEvaluator(evaluator);
			return true;
		}

		Step Finished(std::string a_error)
		{
			return Step{ .finished = true, .error = std::move(a_error) };
		}

		class D3D11Backend final : public Backend
		{
		public:
			const char* Name() const noexcept override { return "D3D11"; }

			// Loads and initializes the library and identifies the device.
			bool Probe(DeviceInfo& a_device) override
			{
				auto& device = a_device;
				if (!globals::d3d::device) {
					device.error = "the D3D11 device is not created yet";
					return false;
				}
				device.probed = true;
				if (!LoadHostLibrary(device))
					return false;
				NVPW_InitializeHost_Params host{ NVPW_InitializeHost_Params_STRUCT_SIZE };
				NVPW_InitializeTarget_Params target{ NVPW_InitializeTarget_Params_STRUCT_SIZE };
				NVPW_D3D11_LoadDriver_Params load{ NVPW_D3D11_LoadDriver_Params_STRUCT_SIZE };
				if (Failed(NVPW_InitializeHost(&host), "NVPW_InitializeHost") || Failed(NVPW_InitializeTarget(&target), "NVPW_InitializeTarget") ||
					Failed(NVPW_D3D11_LoadDriver(&load), "NVPW_D3D11_LoadDriver")) {
					device.error = "the Nsight Perf library could not be initialized (non-NVIDIA GPU or old driver?)";
					return false;
				}
				device.available = true;
				NVPW_D3D11_Device_GetDeviceIndex_Params index{ NVPW_D3D11_Device_GetDeviceIndex_Params_STRUCT_SIZE };
				index.pDevice = globals::d3d::device;
				if (Failed(NVPW_D3D11_Device_GetDeviceIndex(&index), "NVPW_D3D11_Device_GetDeviceIndex")) {
					device.error = "the D3D11 device is not an NVIDIA GPU the profiler knows";
					return false;
				}
				deviceIndex = index.deviceIndex;
				NVPW_Device_GetNames_Params names{ NVPW_Device_GetNames_Params_STRUCT_SIZE };
				names.deviceIndex = index.deviceIndex;
				if (!Failed(NVPW_Device_GetNames(&names), "NVPW_Device_GetNames")) {
					device.deviceName = names.pDeviceName ? names.pDeviceName : "";
					device.chipName = names.pChipName ? names.pChipName : "";
				}
				NVPW_D3D11_Profiler_IsGpuSupported_Params supported{ NVPW_D3D11_Profiler_IsGpuSupported_Params_STRUCT_SIZE };
				supported.deviceIndex = index.deviceIndex;
				device.gpuSupported = !Failed(NVPW_D3D11_Profiler_IsGpuSupported(&supported), "NVPW_D3D11_Profiler_IsGpuSupported") && supported.isSupported &&
				                      !device.chipName.empty();
				if (!device.gpuSupported)
					device.error = "the range profiler does not support this GPU";
				logger::info("[NvPerf] device {} '{}' chip '{}', range profiler supported: {}", index.deviceIndex, device.deviceName, device.chipName,
					device.gpuSupported);
				return device.gpuSupported;
			}

			bool Begin(Capture& a_capture, const std::string& a_chipName, std::string& a_error) override
			{
				d3d.emplace();
				chipName = a_chipName;
				rangeStack.clear();
				unbalancedRanges = 0;
				return BuildConfig(a_capture, *d3d, a_chipName, a_error);
			}

			void PresentBegin(Capture* a_capture) override
			{
				if (a_capture && d3d)
					EndPass(*a_capture);
			}

			// Once per frame after Present: the sample's counters as soon as the GPU finished them, then the next pass.
			Step Advance(Capture& a_capture) override
			{
				auto& capture = *d3d;
				if (capture.sessionActive && a_capture.passesSubmitted) {
					NVPW_D3D11_Profiler_DeviceContext_DecodeCounters_Params decode{ NVPW_D3D11_Profiler_DeviceContext_DecodeCounters_Params_STRUCT_SIZE };
					decode.pDeviceContext = Context();
					decode.counterDataImageSize = capture.counterDataImage.size();
					decode.pCounterDataImage = capture.counterDataImage.data();
					decode.counterDataScratchBufferSize = capture.counterDataScratch.size();
					decode.pCounterDataScratchBuffer = capture.counterDataScratch.data();
					if (Failed(NVPW_D3D11_Profiler_DeviceContext_DecodeCounters(&decode), "NVPW_D3D11_Profiler_DeviceContext_DecodeCounters"))
						return Finished("could not decode the counters");
					a_capture.result.droppedRanges += decode.numRangesDropped;
					a_capture.result.droppedTraceBytes += decode.numTraceBytesDropped;
					if (decode.allPassesCollected) {
						EndSession(a_capture);
						if (!EvaluateRanges(a_capture, capture, chipName))
							return Finished("could not evaluate the counters");
						if (++a_capture.sample >= a_capture.configuration.samples)
							return Finished({});
					} else if (a_capture.passesSubmitted >= a_capture.passesPerSample && ++capture.decodeFrames > kDecodeFrameLimit) {
						return Finished("the GPU did not finish the profiler passes");
					}
				}
				if (capture.framesWithoutRange >= kFramesWithoutRangeLimit)
					return Finished(std::format("no selected range was recorded within {} frames", kFramesWithoutRangeLimit));
				if (!capture.sessionActive && !BeginSession(a_capture, capture))
					return Finished("could not begin a profiler session");
				if (a_capture.passesSubmitted >= a_capture.passesPerSample)
					return Step{ .state = CaptureState::Decoding };
				NVPW_D3D11_Profiler_DeviceContext_BeginPass_Params begin{ NVPW_D3D11_Profiler_DeviceContext_BeginPass_Params_STRUCT_SIZE };
				begin.pDeviceContext = Context();
				if (Failed(NVPW_D3D11_Profiler_DeviceContext_BeginPass(&begin), "NVPW_D3D11_Profiler_DeviceContext_BeginPass"))
					return Finished("could not begin a profiler pass");
				capture.passActive = true;
				SetRangesActive(true);
				return Step{ .state = CaptureState::Capturing };
			}

			void End(Capture& a_capture) override
			{
				if (!d3d)
					return;
				EndSession(a_capture);
				a_capture.result.metrics.clear();
				for (const auto& metric : d3d->metrics)
					a_capture.result.metrics.push_back(metric.spec);
				if (unbalancedRanges)
					logger::warn("[NvPerf] {} perf events were still open at Present", unbalancedRanges);
				d3d.reset();
			}

			void PushRange(Capture& a_capture, std::string_view a_name) override
			{
				if (!d3d)
					return;
				bool pushed = false;
				if (PerfEventSelected(a_capture.configuration.ranges, a_name)) {
					NVPW_D3D11_Profiler_DeviceContext_PushRange_Params push{ NVPW_D3D11_Profiler_DeviceContext_PushRange_Params_STRUCT_SIZE };
					push.pDeviceContext = Context();
					push.pRangeName = a_name.data();
					push.rangeNameLength = a_name.size();
					pushed = NVPW_D3D11_Profiler_DeviceContext_PushRange(&push) == NVPA_STATUS_SUCCESS;
					d3d->rangesThisPass += pushed;
				}
				rangeStack.push_back(pushed);
			}

			void PopRange() override
			{
				if (rangeStack.empty())
					return;
				const bool pushed = rangeStack.back();
				rangeStack.pop_back();
				if (pushed) {
					NVPW_D3D11_Profiler_DeviceContext_PopRange_Params pop{ NVPW_D3D11_Profiler_DeviceContext_PopRange_Params_STRUCT_SIZE };
					pop.pDeviceContext = Context();
					(void)NVPW_D3D11_Profiler_DeviceContext_PopRange(&pop);
				}
			}

		private:
			void EndPass(Capture& a_capture)
			{
				if (!d3d->passActive)
					return;
				SetRangesActive(false);
				// A perf event left open across Present: close what was forwarded so the pass stays balanced.
				for (auto it = rangeStack.rbegin(); it != rangeStack.rend(); ++it) {
					if (*it) {
						NVPW_D3D11_Profiler_DeviceContext_PopRange_Params pop{ NVPW_D3D11_Profiler_DeviceContext_PopRange_Params_STRUCT_SIZE };
						pop.pDeviceContext = Context();
						(void)NVPW_D3D11_Profiler_DeviceContext_PopRange(&pop);
					}
				}
				unbalancedRanges += rangeStack.size();
				rangeStack.clear();
				NVPW_D3D11_Profiler_DeviceContext_EndPass_Params end{ NVPW_D3D11_Profiler_DeviceContext_EndPass_Params_STRUCT_SIZE };
				end.pDeviceContext = Context();
				(void)Failed(NVPW_D3D11_Profiler_DeviceContext_EndPass(&end), "NVPW_D3D11_Profiler_DeviceContext_EndPass");
				d3d->passActive = false;
				++a_capture.passesSubmitted;
				d3d->framesWithoutRange = d3d->rangesThisPass ? 0 : d3d->framesWithoutRange + 1;
				d3d->rangesThisPass = 0;
			}

			void EndSession(Capture& a_capture)
			{
				EndPass(a_capture);
				if (!d3d->sessionActive)
					return;
				NVPW_D3D11_Profiler_DeviceContext_EndSession_Params end{ NVPW_D3D11_Profiler_DeviceContext_EndSession_Params_STRUCT_SIZE };
				end.pDeviceContext = Context();
				(void)Failed(NVPW_D3D11_Profiler_DeviceContext_EndSession(&end), "NVPW_D3D11_Profiler_DeviceContext_EndSession");
				d3d->sessionActive = false;
			}

			std::size_t deviceIndex = 0;
			std::string chipName;
			std::optional<D3D11Capture> d3d;
			std::vector<bool> rangeStack;  // per open perf event: whether it was forwarded to the profiler
			std::uint64_t unbalancedRanges = 0;
		};
	}

	std::unique_ptr<Backend> MakeD3D11Backend()
	{
		return std::make_unique<D3D11Backend>();
	}
}

#else

namespace NvPerf::detail
{
	std::unique_ptr<Backend> MakeD3D11Backend()
	{
		return nullptr;
	}
}

#endif
