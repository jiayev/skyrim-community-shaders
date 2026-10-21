#pragma once

#include "Features/RemoteControl/NvPerf.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief NvPerf traces: a named template (ranges, metric groups, repeats, report groups) run as a sequence of captures
 * and merged into one report, so a measurement is one name instead of a hand-built capture per metric set.
 *
 * A template's metric groups are captured one after another, each repeats times, every capture with the template's
 * ranges; the report is one row per measured range (its own name, not the filter that matched it). A row's GPU time is
 * the median over every sample of every capture (a sample that lacks the range counts 0), with its min and max; a sample
 * whose frame total is far from its capture's median (a hitch: a pipeline compile, a streaming spike) is dropped and
 * counted. Each other metric is the median over the samples of the captures that measured it. Report groups sum their
 * rows' times (a row belongs to the first group with a filter matching it). Warnings name what a capture could not see:
 * selected passes that ran on another queue, filters that matched nothing, unsupported metrics, dropped and unstable rows.
 *
 * Templates: the built-in ones (BuiltInTemplates) and every *.json in TemplateDirectory(), read by ReloadTemplates. A file
 * holds one template or an array of them:
 *   { "name": "z-prepass", "extends": "dclf-barriers", "description": "...",
 *     "ranges": ["Depth", "cs.dclf.z.*"],          // replaces the base's; "addRanges" appends to them instead
 *     "metrics": ["gpu__time_duration", "sm__throughput:throughput"],   // one group; or "metricGroups":
 *     "metricGroups": [{ "name": "units", "metrics": [...] }],
 *     "samples": 4, "repeats": 3, "startFrame": 3000,
 *     "groups": [{ "name": "Z-prepass", "ranges": ["cs.dclf.z.*"] }] }  // replaces the base's; "addGroups" prepends
 * A metric is ParseMetric's form ("name[:counter|ratio|throughput|active|elapsed|percycle]").
 *
 * Every report is written to ReportDirectory() as <label>.json and <label>.txt, and logged. A report compared with a
 * baseline (an earlier report's label: a run with DCLF off, before a change) shows each row's and group's difference, marked
 * significant ('*') when the two means differ by more than their confidence intervals combined.
 *
 * Statistics (TraceStatistics.h): every row and group also carries the mean of its per-sample time with a confidence interval
 * (Newey-West effective sample count, Student t), median, p05/p95 and n_eff. A series converges once its interval's
 * half-width is within relativeHalfWidth of its mean (or absoluteHalfWidthUs) with at least minSamples samples and effective
 * samples. Template keys: "confidence" (0.95), "relativeHalfWidth" (0.02), "absoluteHalfWidthUs" (1.0), "minSamples" (30),
 * "maxSamples" (3000), "settlingFrames" (120), "timeoutFrames" (6000), "maxRepeats" (0).
 *
 * Two sources ("source"):
 * - "nvperf" (the default): the captures above. With "maxRepeats" above "repeats", repeats is a minimum: after the last planned
 *   capture, while a row or group's gpu__time_duration has not converged, another round (every metric group once) is captured,
 *   up to maxRepeats rounds.
 * - "timestamps": OpenRenderGraph's pass timestamps (GpuTimestamps.h), cheap and continuous: every frame is a sample of every
 *   graph pass and barrier batch (cs.org.barrier.<where>.<pass>) on every queue, including the async compute queue NvPerf
 *   cannot see. The trace turns on recording timestamps (a graph rebuild, so DCLF's kept recordings carry them), waits for the
 *   game (headless: startFrame presents after the load) and the rebuild, discards settlingFrames frames, then samples every
 *   frame until every row and group converges, maxSamples frames, or timeoutFrames frames; then turns them off. A row is a
 *   range's time per frame (every occurrence summed; 0 in a frame without it), its time the mean. Engine (D3D11) events are not
 *   timestamped. A range on the compute queue is its own row ("<name> [compute]", queue "compute"), left out of the groups and
 *   the total: it overlaps the graphics queue's work. A frame whose total is beyond 1.5x the median's is a hitch and dropped,
 *   as is a frame whose epochs did not all come back. "metrics", "samples" and "repeats" do not apply.
 *
 * Headless: CS_NVPERF_TEMPLATE=<name> runs a trace once a game loaded (CS_NVPERF_LABEL names its report, default the
 * template's name; CS_NVPERF_BASELINE compares it with a report; CS_NVPERF_START_FRAME, CS_NVPERF_SAMPLES,
 * CS_NVPERF_REPEATS, CS_NVPERF_MAX_REPEATS, CS_NVPERF_MIN_SAMPLES, CS_NVPERF_MAX_SAMPLES and CS_NVPERF_HALF_WIDTH (a
 * relative half-width, e.g. 0.02) override the template's). Its startFrame counts from the first present after the load: on
 * the main menu and loading screens the graph's owners do not build, so a capture there would measure lists recorded before it.
 */
namespace NvPerf::Trace
{
	struct MetricGroup
	{
		std::string name;
		std::vector<std::string> metrics;  // ParseMetric's form
	};

	struct ReportGroup
	{
		std::string name;
		std::vector<std::string> ranges;  // filters, as CaptureConfiguration::ranges
	};

	/// Where a trace's times come from ("source").
	enum class TimingSource
	{
		NvPerf,      // "nvperf": range profiler captures
		Timestamps,  // "timestamps": the render graph's pass timestamps, every frame
	};

	struct Template
	{
		std::string name;
		std::string description;
		std::vector<std::string> ranges;  // empty: every outermost range (timestamps: every range)
		std::vector<MetricGroup> metricGroups;
		std::vector<ReportGroup> groups;
		// Timestamps: a row "span:<name>" per span, each frame the graphics queue's time from the first of its ranges' beginnings to
		// the last one's end (passes and barrier batches overlap: a range begins when the queue reaches it, before the work ahead of it
		// has drained, so their sum counts some work twice; a span counts the queue's time once). Out of the total and the groups.
		std::vector<ReportGroup> spans;
		std::uint32_t samples = 4;
		std::uint32_t repeats = 1;
		std::uint64_t startFrame = 2000;  // a headless trace's first capture starts this many presents after a game loaded (DCLF compiles in the first ones)
		std::filesystem::path source;     // empty: built in
		TimingSource timingSource = TimingSource::NvPerf;
		// Convergence (TraceStatistics.h): timestamps always; NvPerf when maxRepeats > repeats.
		double confidence = 0.95;
		double relativeHalfWidth = 0.02;
		double absoluteHalfWidthUs = 1.0;
		std::uint32_t minSamples = 30;
		std::uint32_t maxSamples = 3000;      // timestamps: frames sampled at most
		std::uint32_t maxRepeats = 0;         // NvPerf: rounds captured at most (0: repeats, fixed)
		std::uint32_t settlingFrames = 120;   // timestamps: frames discarded once ready
		std::uint32_t timeoutFrames = 6000;   // timestamps: presents sampled before giving up on convergence
	};

	/// A series' statistics (TraceStatistics.h), in microseconds.
	struct SeriesStats
	{
		double meanUs = 0.0;
		double ciUs = std::numeric_limits<double>::quiet_NaN();  // the mean's confidence interval half-width
		double medianUs = 0.0, p05Us = 0.0, p95Us = 0.0;
		double effectiveSamples = 0.0;
		std::uint32_t madOutliers = 0;
		bool converged = false;
	};

	struct Row
	{
		std::string name;
		std::string group;  // its report group, or empty
		std::string queue;  // timestamps: "graphics", "compute" or "copy"; NvPerf: empty
		double occurrencesPerFrame = 0.0;
		double timeUs = 0.0;  // NvPerf: the median; timestamps: the mean
		double timeMinUs = 0.0, timeMaxUs = 0.0;
		std::uint32_t samples = 0;              // samples it was measured in
		std::map<std::string, double> metrics;  // MetricLabel -> median
		SeriesStats stats;
	};

	struct GroupRow
	{
		std::string name;
		double timeUs = 0.0;
		std::uint32_t ranges = 0;
		SeriesStats stats;  // of the group's per-sample sum
	};

	struct Report
	{
		std::string templateName, label, chipName;
		TimingSource timingSource = TimingSource::NvPerf;
		bool success = false;
		bool converged = false;  // every row and group (false when convergence was not asked for)
		double confidence = 0.0, relativeHalfWidth = 0.0;  // the target
		std::string error;
		std::uint64_t startFrame = 0, endFrame = 0;
		std::uint32_t captures = 0, failedCaptures = 0, samples = 0, outlierSamples = 0;
		std::uint32_t droppedFrames = 0;  // timestamps: frames whose epochs did not all come back
		std::vector<std::string> metricLabels;  // every metric of every row, in the template's order
		std::vector<Row> rows;                  // by time, descending
		std::vector<GroupRow> groups;           // in the template's order, then "other"
		double totalUs = 0.0;                   // every row's time
		std::vector<std::string> warnings;
		std::filesystem::path jsonPath, textPath;
		std::shared_ptr<const Report> baseline;  // the report it was compared with
	};

	enum class State
	{
		Idle,
		Running,
		Done,
		Failed,
	};

	struct Status
	{
		State state = State::Idle;
		std::string templateName, label;
		std::uint32_t capture = 0, captures = 0;  // the capture in flight (1-based) of the trace's
		std::string phase;                        // e.g. "units, repeat 2 of 3"
		std::string error;
		// Convergence, as of the last check: samples so far, rows and groups converged, and the furthest from it.
		bool convergence = false;
		std::uint32_t samples = 0, minSamples = 0, maxSamples = 0;
		std::uint32_t converged = 0, series = 0;
		std::string worst;
		double worstRelativeHalfWidth = 0.0, targetRelativeHalfWidth = 0.0;
	};

	/// The built-in templates, then the files' (a file's replaces a built-in one of the same name).
	std::vector<Template> Templates();
	/// Reads TemplateDirectory() again. @return Each file's error, joined (empty: none).
	std::string ReloadTemplates();
	std::filesystem::path TemplateDirectory();
	std::filesystem::path ReportDirectory();
	/// The labels of the reports in ReportDirectory(), newest first.
	std::vector<std::string> SavedReports();
	/// Reads a report written by a trace: a label in ReportDirectory() or a path to its .json.
	std::shared_ptr<const Report> LoadReport(std::string_view labelOrPath, std::string& error);

	/// Starts a trace (any thread). a_label names its report (empty: the template's name); a_baseline is a report to compare
	/// it with (empty: none). Fails while a trace or a capture is in progress.
	bool Start(std::string_view templateName, std::string_view label, std::string_view baseline, std::string& error);
	/// Start with a template given as JSON text, in a template file's form ("extends" a known one; e.g. a timing template with
	/// its own ranges, groups and confidence for one A/B), not added to Templates().
	bool StartInline(std::string_view templateJson, std::string_view label, std::string_view baseline, std::string& error);
	/// A report as the JSON its .json file holds.
	std::string ReportToJson(const Report& report);
	void Cancel();
	Status GetStatus();
	std::shared_ptr<const Report> Latest();
	/// A report as text: its groups, then its rows (and their difference from its baseline).
	std::string FormatReport(const Report& report);

	/// Whether CS_NVPERF_TEMPLATE requests a headless trace.
	bool EnvironmentRequested();
	/// Render thread, from NvPerf::OnPresentEnd: starts the headless trace, queues the next capture, merges finished ones.
	void OnPresentEnd(std::uint64_t presents);
	/// SKSE's kPostLoadGame (any thread): a headless trace's startFrame counts from the next present.
	void OnGameLoaded();
}
