#pragma once

#include "Features/RemoteControl/GpuCountersBackend.h"

#include <gpu_performance_api/gpu_perf_api.h>

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Internal to the GPUPerfAPI backends (GpaD3D11.cpp, GpaVulkan.cpp): the library, the counters a capture measures
 * and how its passes' samples become RangeResults.
 *
 * GPUPerfAPI ("GPA") is AMD's counter library. It is loaded and initialized before the device exists
 * (GpaBeforeDeviceCreation), its context is opened on the device, and each pass of a sample is one GPA session with one
 * group of counters enabled: a group is what the hardware counts in a single pass, so a session never needs the same work
 * replayed, and a frame is a pass. The groups are formed when a capture begins. GPUTime, the weight the front end averages
 * percentages by, is always measured (on D3D11 it takes a pass of its own).
 */
namespace GpuCounters::detail::gpa
{
	/// The library's entry points, or null: not requested at startup, missing, or failed to initialize (Unavailable()).
	const GpaFunctionTable* Table();
	const std::string& Unavailable();
	/// Logs a failed call: false on failure.
	bool Check(GpaStatus a_status, const char* a_operation);

	/// The default metrics on GPUPerfAPI.
	std::vector<MetricRequest> DefaultMetrics();
	/// The device's name and generation, for DeviceInfo, once its context is open.
	void Describe(GpaContextId a_context, DeviceInfo& a_device);

	/// The metrics a capture measures and the groups its passes enable.
	struct Schedule
	{
		std::vector<MetricRequest> metrics;            // resolved to kPlainSummed / kPlainAveraged; the result's metrics
		std::vector<MetricRequest> unsupported;        // requested, but not a counter here or not countable in one pass
		std::vector<GpaUInt32> counters;               // per metric: its counter index
		std::vector<GpaDataType> types;                // per metric
		std::vector<std::vector<std::size_t>> groups;  // per pass: the metrics it enables
		std::size_t timeMetric = 0;
	};
	/// Resolves a_requested on a_context and groups it into single-pass groups. False with a_error.
	bool BuildSchedule(GpaContextId a_context, const std::vector<MetricRequest>& a_requested, Schedule& a_schedule, std::string& a_error);
	/// A session counting a_schedule's group a_group, begun. Null with a_error.
	GpaSessionId BeginPassSession(GpaContextId a_context, const Schedule& a_schedule, std::size_t a_group, std::string& a_error);
	/// Ends (if begun) and deletes a session.
	void DeleteSession(GpaSessionId a_session, bool a_end);

	/// A sample's passes merged: each range's values per metric, matched across passes by name and occurrence (the n-th
	/// sample of that name in its pass). A range a pass did not see keeps NaN for that pass's metrics.
	class Merger
	{
	public:
		void Reset(std::size_t a_metricCount);
		/// Reads a complete session's samples, given in the order they began with their range names. False with a_error.
		bool AddPass(GpaSessionId a_session, const Schedule& a_schedule, std::size_t a_group, const std::vector<std::pair<GpaUInt32, std::string>>& a_samples,
			std::string& a_error);
		/// The merged ranges, as sample a_sample of a_result. Clears them.
		void Emit(std::uint32_t a_sample, CaptureResult& a_result);

	private:
		struct Row
		{
			std::string name;
			std::uint32_t occurrence = 0;
			std::vector<double> values;
		};
		std::size_t metricCount = 0;
		std::vector<Row> rows;  // first-seen order
		std::map<std::pair<std::string, std::uint32_t>, std::size_t> index;
	};
}
