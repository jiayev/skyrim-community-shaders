#pragma once

#include "Features/RemoteControl/NvPerf.h"

#include <cstdint>
#include <filesystem>
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
 * baseline (an earlier report's label: a run with DCLF off, before a change) shows each row's and group's difference.
 *
 * Headless: CS_NVPERF_TEMPLATE=<name> runs a trace once a game loaded (CS_NVPERF_LABEL names its report, default the
 * template's name; CS_NVPERF_BASELINE compares it with a report; CS_NVPERF_START_FRAME, CS_NVPERF_SAMPLES and
 * CS_NVPERF_REPEATS override the template's). Its startFrame counts from the first present after the load: on the main
 * menu and loading screens the graph's owners do not build, so a capture there would measure lists recorded before it.
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

	struct Template
	{
		std::string name;
		std::string description;
		std::vector<std::string> ranges;  // empty: every outermost range
		std::vector<MetricGroup> metricGroups;
		std::vector<ReportGroup> groups;
		std::uint32_t samples = 4;
		std::uint32_t repeats = 1;
		std::uint64_t startFrame = 2000;  // a headless trace's first capture starts this many presents after a game loaded (DCLF compiles in the first ones)
		std::filesystem::path source;     // empty: built in
	};

	struct Row
	{
		std::string name;
		std::string group;  // its report group, or empty
		double occurrencesPerFrame = 0.0;
		double timeUs = 0.0;  // median
		double timeMinUs = 0.0, timeMaxUs = 0.0;
		std::uint32_t samples = 0;              // samples it was measured in
		std::map<std::string, double> metrics;  // MetricLabel -> median
	};

	struct GroupRow
	{
		std::string name;
		double timeUs = 0.0;
		std::uint32_t ranges = 0;
	};

	struct Report
	{
		std::string templateName, label, chipName;
		bool success = false;
		std::string error;
		std::uint64_t startFrame = 0, endFrame = 0;
		std::uint32_t captures = 0, failedCaptures = 0, samples = 0, outlierSamples = 0;
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
