#include "Features/RemoteControl/NvPerfTrace.h"

#include "Features/RemoteControl/GpuTimestamps.h"
#include "Features/RemoteControl/NvPerfBackend.h"
#include "Features/RemoteControl/TraceStatistics.h"

#include "Utils/FileSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>

// NvPerf's traces (NvPerfTrace.h): templates, the runner that drives a template's captures through RequestCapture (or, for a
// timestamp trace, samples the render graph's pass timestamps) on the render thread, and the report.
namespace NvPerf::Trace
{
	namespace
	{
		using json = nlohmann::json;

		// The metric sets the built-in templates combine. Time rides along in every set, so each capture's rows are weighted
		// and comparable; a second metric keeps a time-only set from planning a single replay pass.
		const std::vector<std::string> kTimeMetrics{ "gpu__time_duration:counter", "sm__throughput:throughput" };
		const std::vector<std::string> kUnitMetrics{ "gpu__time_duration:counter", "fe__throughput:throughput", "pda__throughput:throughput",
			"vaf__throughput:throughput", "vpc__throughput:throughput", "raster__throughput:throughput", "prop__throughput:throughput", "zrop__throughput:throughput",
			"crop__throughput:throughput", "sm__throughput:throughput", "l1tex__throughput:throughput", "lts__throughput:throughput", "dram__throughput:throughput" };
		const std::vector<std::string> kShaderMetrics{ "gpu__time_duration:counter", "smsp__inst_executed:counter", "sm__warps_active:active",
			"smsp__warp_issue_stalled_long_scoreboard_per_warp_active:ratio", "smsp__warp_issue_stalled_short_scoreboard_per_warp_active:ratio",
			"smsp__warp_issue_stalled_tex_throttle_per_warp_active:ratio", "smsp__warp_issue_stalled_math_pipe_throttle_per_warp_active:ratio",
			"smsp__warp_issue_stalled_mio_throttle_per_warp_active:ratio", "smsp__warp_issue_stalled_lg_throttle_per_warp_active:ratio",
			"smsp__warp_issue_stalled_wait_per_warp_active:ratio", "smsp__warp_issue_stalled_no_instruction_per_warp_active:ratio",
			"smsp__warp_issue_stalled_barrier_per_warp_active:ratio" };
		const std::vector<std::string> kMemoryMetrics{ "gpu__time_duration:counter", "dram__bytes_read:counter", "dram__bytes_write:counter",
			"l1tex__t_sectors_pipe_lsu_mem_local_op_ld:counter", "l1tex__t_sectors_pipe_lsu_mem_local_op_st:counter", "smsp__inst_executed_pipe_lsu:counter",
			"smsp__inst_executed_pipe_tex:counter", "prop__earlyz_output_samples:counter", "pda__input_prims:counter" };

		// The engine's outermost events beside DCLF's and Light Limit Fix's graph passes: with DCLF off, the engine ranges hold
		// the same work, so one template compares both runs.
		const std::vector<std::string> kDclfRanges{ "Depth", "World", "Directional Light Shadowmaps", "First Person View", "cs.dclf.*", "cs.llf.*" };

		// DCLF's epochs (Schedule/Frame.cpp), and the engine events that hold the same work with DCLF off.
		std::vector<ReportGroup> DclfGroups()
		{
			return {
				{ "ORG barriers", { "cs.org.barrier.*" } },
				{ "Z-prepass", { "cs.dclf.z.*", "cs.dclf.hzb", "cs.dclf.depth-phase2", "cs.dclf.build-draws-phase2", "cs.dclf.tree-lod-cull-phase2" } },
				{ "main colour", { "cs.dclf.latched-copies", "cs.dclf.build-draws", "cs.dclf.sort-*", "cs.dclf.forward-sort", "cs.dclf.main-opaque", "cs.dclf.colour*" } },
				{ "shadows", { "cs.dclf.shadow.*", "Directional Light Shadowmaps" } },
				{ "first person", { "cs.dclf.first-person.*", "First Person View" } },
				{ "reflections", { "cs.dclf.reflection.*" } },
				{ "sky occlusion", { "cs.dclf.sky.*" } },
				{ "forward", { "cs.dclf.forward", "cs.dclf.refraction" } },
				{ "light culling", { "cs.llf.*" } },
				{ "engine depth", { "Depth" } },
				{ "engine world", { "World" } },
			};
		}

		std::vector<Template> BuiltInTemplates()
		{
			const auto dclf = [](std::string a_name, std::string a_description, std::vector<MetricGroup> a_metrics, std::uint32_t a_repeats) {
				Template t;
				t.name = std::move(a_name);
				t.description = std::move(a_description);
				t.ranges = kDclfRanges;
				t.metricGroups = std::move(a_metrics);
				t.groups = DclfGroups();
				t.repeats = a_repeats;
				return t;
			};
			std::vector<Template> templates;
			{
				Template frame;
				frame.name = "frame";
				frame.description = "Every outermost range's GPU time. The engine's Player View holds most native work, so compare runs, not rows within one.";
				frame.metricGroups = { { "time", kTimeMetrics } };
				frame.repeats = 3;
				templates.push_back(std::move(frame));
			}
			templates.push_back(dclf("dclf-passes", "DCLF's graph passes and the engine events holding the same work, by epoch: GPU time. Run it with DCLF on and off (CS_DCLF=0) and compare.",
				{ { "time", kTimeMetrics } }, 3));
			{
				auto barriers = dclf("dclf-barriers", "dclf-passes plus ORG's barrier batches around every graph pass (cs.org.barrier.*): what an epoch costs between its passes.",
					{ { "time", kTimeMetrics } }, 2);
				barriers.ranges.push_back("cs.org.*");
				templates.push_back(std::move(barriers));
			}
			templates.push_back(dclf("dclf-units", "dclf-passes with every unit's throughput (front end, primitive, raster, ROP, SM, L1, L2, DRAM).", { { "units", kUnitMetrics } }, 1));
			templates.push_back(dclf("dclf-shader", "dclf-passes with instructions, occupancy and the warp stall reasons.", { { "shader", kShaderMetrics } }, 1));
			templates.push_back(dclf("dclf-memory", "dclf-passes with DRAM bytes, register spills (local memory), load/store and texture instructions, early-Z samples and input primitives.",
				{ { "memory", kMemoryMetrics } }, 1));
			templates.push_back(dclf("dclf-full", "dclf-units, dclf-shader and dclf-memory in one trace: three captures merged into one table.",
				{ { "units", kUnitMetrics }, { "shader", kShaderMetrics }, { "memory", kMemoryMetrics } }, 1));
			// The timestamp traces: every frame a sample of the render graph's pass timestamps, until each mean is within 2%.
			const auto timing = [](Template a_t, std::string a_name, std::string a_description) {
				a_t.name = std::move(a_name);
				a_t.description = std::move(a_description);
				a_t.timingSource = TimingSource::Timestamps;
				return a_t;
			};
			{
				Template frame;
				frame.metricGroups = { { "time", kTimeMetrics } };
				templates.push_back(timing(std::move(frame), "frame-timing",
					"Every render graph range's GPU time from ORG's pass timestamps (passes and barrier batches, every queue), each frame a sample, "
					"until every mean is within 2% at 95% confidence. Engine work is not timestamped."));
			}
			templates.push_back(timing(dclf("", "", { { "time", kTimeMetrics } }, 1), "dclf-timing",
				"dclf-passes from ORG's pass timestamps: every frame a sample until every row's and group's mean is within 2%, compute-queue passes "
				"included as their own rows. The engine events are not timestamped: with DCLF off only the graph's passes are measured."));
			{
				auto barriers = timing(dclf("", "", { { "time", kTimeMetrics } }, 1), "dclf-barrier-timing",
					"dclf-timing plus ORG's barrier batches around every graph pass (cs.org.barrier.*).");
				barriers.ranges.push_back("cs.org.barrier.*");
				templates.push_back(std::move(barriers));
			}
			return templates;
		}

		const char* SourceName(TimingSource a_source)
		{
			return a_source == TimingSource::Timestamps ? "timestamps" : "nvperf";
		}

		// The report group a range belongs to: the first with a filter matching it (empty: none).
		std::string GroupOf(const Template& a_t, std::string_view a_name)
		{
			for (const auto& group : a_t.groups)
				if (std::ranges::any_of(group.ranges, [&](const std::string& filter) { return detail::NameMatches(filter, a_name); }))
					return group.name;
			return {};
		}

		Statistics::Config StatisticsConfig(const Template& a_t)
		{
			return Statistics::ClampConfig({ a_t.confidence, a_t.relativeHalfWidth, a_t.absoluteHalfWidthUs, a_t.minSamples, a_t.maxSamples });
		}

		SeriesStats Summarize(const std::vector<double>& a_timesUs, const Template& a_t)
		{
			const auto summary = Statistics::Summarize(a_timesUs, StatisticsConfig(a_t));
			SeriesStats stats;
			stats.meanUs = summary.mean;
			stats.ciUs = summary.halfWidth;
			stats.medianUs = summary.median;
			stats.p05Us = summary.p05;
			stats.p95Us = summary.p95;
			stats.effectiveSamples = summary.effectiveSamples;
			stats.madOutliers = summary.madOutliers;
			stats.converged = summary.converged;
			return stats;
		}

		double RelativeHalfWidth(const SeriesStats& a_stats)
		{
			if (std::isnan(a_stats.ciUs))
				return std::numeric_limits<double>::infinity();
			return a_stats.meanUs != 0.0 ? a_stats.ciUs / std::abs(a_stats.meanUs) : (a_stats.ciUs == 0.0 ? 0.0 : std::numeric_limits<double>::infinity());
		}

		// Every row and group of a report: how many converged, and the one furthest from it (by relative half-width).
		struct Convergence
		{
			std::uint32_t converged = 0, series = 0;
			std::string worst;
			double worstRelative = 0.0;
			bool All() const { return series && converged == series; }
		};

		Convergence CheckConvergence(const Report& a_report)
		{
			Convergence out;
			const auto note = [&](const std::string& a_name, const SeriesStats& a_stats) {
				++out.series;
				if (a_stats.converged) {
					++out.converged;
					return;
				}
				const double relative = RelativeHalfWidth(a_stats);
				if (out.worst.empty() || relative > out.worstRelative) {
					out.worst = a_name;
					out.worstRelative = relative;
				}
			};
			for (const auto& row : a_report.rows)
				note(row.name, row.stats);
			for (const auto& group : a_report.groups)
				note("group " + group.name, group.stats);
			return out;
		}

		// The rows furthest from converging, for a warning.
		std::string Unconverged(const Report& a_report, std::size_t& a_count)
		{
			std::vector<std::pair<double, const Row*>> open;
			for (const auto& row : a_report.rows)
				if (!row.stats.converged)
					open.emplace_back(RelativeHalfWidth(row.stats), &row);
			std::ranges::sort(open, std::greater<>{}, &std::pair<double, const Row*>::first);
			a_count = open.size();
			std::string out;
			for (std::size_t i = 0; i < open.size() && i < 6; ++i)
				out += std::format("{}{} ({:.1f} +/- {:.1f} us)", out.empty() ? "" : ", ", open[i].second->name, open[i].second->stats.meanUs, open[i].second->stats.ciUs);
			return out;
		}

		std::vector<std::string> Strings(const json& a_value, std::string_view a_key, std::string& a_error)
		{
			std::vector<std::string> out;
			if (!a_value.is_array()) {
				a_error = std::format("'{}' must be an array of strings", a_key);
				return out;
			}
			for (const auto& entry : a_value) {
				if (!entry.is_string()) {
					a_error = std::format("'{}' must be an array of strings", a_key);
					return {};
				}
				out.push_back(entry.get<std::string>());
			}
			return out;
		}

		std::vector<ReportGroup> Groups(const json& a_value, std::string_view a_key, std::string& a_error)
		{
			std::vector<ReportGroup> out;
			if (!a_value.is_array()) {
				a_error = std::format("'{}' must be an array of {{ \"name\", \"ranges\" }}", a_key);
				return out;
			}
			for (const auto& entry : a_value) {
				if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() || !entry.contains("ranges")) {
					a_error = std::format("'{}' must be an array of {{ \"name\", \"ranges\" }}", a_key);
					return {};
				}
				ReportGroup group{ entry["name"].get<std::string>(), Strings(entry["ranges"], "ranges", a_error) };
				if (!a_error.empty())
					return {};
				out.push_back(std::move(group));
			}
			return out;
		}

		bool ParseTemplate(const json& a_value, const std::vector<Template>& a_known, Template& a_out, std::string& a_error)
		{
			if (!a_value.is_object() || !a_value.contains("name") || !a_value["name"].is_string()) {
				a_error = "a template is an object with a \"name\"";
				return false;
			}
			const auto name = a_value["name"].get<std::string>();
			if (a_value.contains("extends")) {
				const auto base = a_value["extends"].is_string() ? a_value["extends"].get<std::string>() : std::string{};
				const auto found = std::ranges::find_if(a_known, [&](const Template& t) { return t.name == base; });
				if (found == a_known.end()) {
					a_error = std::format("'{}' extends '{}', which is not defined before it", name, base);
					return false;
				}
				a_out = *found;
			} else {
				a_out = Template{};
				a_out.metricGroups = { { "time", kTimeMetrics } };
			}
			a_out.name = name;
			const auto number = [&](const char* a_key, auto& a_field) {
				if (a_value.contains(a_key)) {
					if (!a_value[a_key].is_number_unsigned())
						a_error = std::format("'{}' must be a whole number", a_key);
					else
						a_field = a_value[a_key].get<std::remove_reference_t<decltype(a_field)>>();
				}
			};
			if (a_value.contains("description") && a_value["description"].is_string())
				a_out.description = a_value["description"].get<std::string>();
			if (a_value.contains("ranges"))
				a_out.ranges = Strings(a_value["ranges"], "ranges", a_error);
			if (a_value.contains("addRanges"))
				for (auto& range : Strings(a_value["addRanges"], "addRanges", a_error))
					a_out.ranges.push_back(std::move(range));
			if (a_value.contains("metrics"))
				a_out.metricGroups = { { "metrics", Strings(a_value["metrics"], "metrics", a_error) } };
			if (a_value.contains("metricGroups")) {
				a_out.metricGroups.clear();
				if (!a_value["metricGroups"].is_array())
					a_error = "'metricGroups' must be an array of { \"name\", \"metrics\" }";
				else
					for (const auto& entry : a_value["metricGroups"]) {
						if (!entry.is_object() || !entry.contains("metrics")) {
							a_error = "'metricGroups' must be an array of { \"name\", \"metrics\" }";
							break;
						}
						const auto groupName = entry.contains("name") && entry["name"].is_string() ? entry["name"].get<std::string>() :
						                                                                              std::format("group {}", a_out.metricGroups.size() + 1);
						a_out.metricGroups.push_back({ groupName, Strings(entry["metrics"], "metrics", a_error) });
					}
			}
			if (a_value.contains("groups"))
				a_out.groups = Groups(a_value["groups"], "groups", a_error);
			if (a_value.contains("spans"))
				a_out.spans = Groups(a_value["spans"], "spans", a_error);
			if (a_value.contains("seriesFile") && a_value["seriesFile"].is_string())
				a_out.seriesFile = std::filesystem::path(a_value["seriesFile"].get<std::string>());
			if (a_value.contains("addGroups")) {
				auto added = Groups(a_value["addGroups"], "addGroups", a_error);
				a_out.groups.insert(a_out.groups.begin(), added.begin(), added.end());
			}
			const auto real = [&](const char* a_key, double& a_field) {
				if (a_value.contains(a_key)) {
					if (!a_value[a_key].is_number())
						a_error = std::format("'{}' must be a number", a_key);
					else
						a_field = a_value[a_key].get<double>();
				}
			};
			if (a_value.contains("source")) {
				const auto text = a_value["source"].is_string() ? a_value["source"].get<std::string>() : std::string{};
				if (text == "nvperf")
					a_out.timingSource = TimingSource::NvPerf;
				else if (text == "timestamps")
					a_out.timingSource = TimingSource::Timestamps;
				else
					a_error = "'source' must be \"nvperf\" or \"timestamps\"";
			}
			number("samples", a_out.samples);
			number("repeats", a_out.repeats);
			number("startFrame", a_out.startFrame);
			real("confidence", a_out.confidence);
			real("relativeHalfWidth", a_out.relativeHalfWidth);
			real("absoluteHalfWidthUs", a_out.absoluteHalfWidthUs);
			number("minSamples", a_out.minSamples);
			number("maxSamples", a_out.maxSamples);
			number("maxRepeats", a_out.maxRepeats);
			number("settlingFrames", a_out.settlingFrames);
			number("timeoutFrames", a_out.timeoutFrames);
			if (a_error.empty() && !(a_out.confidence > 0.0 && a_out.confidence < 1.0))
				a_error = "confidence must be between 0 and 1";
			if (a_error.empty() && (a_out.relativeHalfWidth < 0.0 || a_out.absoluteHalfWidthUs < 0.0))
				a_error = "half-widths cannot be negative";
			if (a_error.empty() && (a_out.minSamples < 2 || a_out.maxSamples < a_out.minSamples))
				a_error = "minSamples must be at least 2 and maxSamples at least minSamples";
			if (a_error.empty() && a_out.maxRepeats > 64)
				a_error = "maxRepeats must be at most 64";
			if (a_error.empty() && a_out.timingSource == TimingSource::NvPerf && a_out.metricGroups.empty())
				a_error = "no metrics";
			for (const auto& group : a_out.metricGroups)
				for (const auto& text : group.metrics) {
					MetricRequest metric;
					if (a_error.empty() && !ParseMetric(text, metric, a_error))
						a_error = std::format("metric '{}': {}", text, a_error);
				}
			if (a_error.empty() && (a_out.samples < 1 || a_out.samples > 64))
				a_error = "samples must be between 1 and 64";
			if (a_error.empty() && (a_out.repeats < 1 || a_out.repeats > 16))
				a_error = "repeats must be between 1 and 16";
			if (!a_error.empty())
				a_error = std::format("'{}': {}", name, a_error);
			return a_error.empty();
		}

		struct TemplateStore
		{
			std::mutex mutex;
			bool loaded = false;
			std::vector<Template> files;
			std::string errors;
		};

		TemplateStore& Store()
		{
			static TemplateStore store;
			return store;
		}

		// Caller holds the store's mutex.
		void LoadLocked(TemplateStore& a_store)
		{
			a_store.loaded = true;
			a_store.files.clear();
			a_store.errors.clear();
			std::error_code ec;
			const auto directory = TemplateDirectory();
			if (!std::filesystem::is_directory(directory, ec))
				return;
			std::vector<std::filesystem::path> paths;
			for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
				if (entry.is_regular_file() && entry.path().extension() == ".json")
					paths.push_back(entry.path());
			std::ranges::sort(paths);
			auto known = BuiltInTemplates();
			for (const auto& path : paths) {
				const auto fail = [&](const std::string& a_error) {
					a_store.errors += std::format("{}{}: {}", a_store.errors.empty() ? "" : "\n", path.filename().string(), a_error);
				};
				std::ifstream file(path);
				const json document = json::parse(file, nullptr, false, true);
				if (document.is_discarded()) {
					fail("not valid JSON");
					continue;
				}
				const auto take = [&](const json& a_value) {
					Template t;
					std::string error;
					if (!ParseTemplate(a_value, known, t, error)) {
						fail(error);
						return;
					}
					t.source = path;
					std::erase_if(known, [&](const Template& k) { return k.name == t.name; });
					known.push_back(t);
					std::erase_if(a_store.files, [&](const Template& k) { return k.name == t.name; });
					a_store.files.push_back(std::move(t));
				};
				if (document.is_array())
					for (const auto& entry : document)
						take(entry);
				else
					take(document);
			}
			if (!a_store.errors.empty())
				logger::warn("[NvPerf] trace templates: {}", a_store.errors);
		}

		std::string Env(const char* a_name)
		{
			char value[4096]{};
			const DWORD length = ::GetEnvironmentVariableA(a_name, value, sizeof(value));
			return length && length < sizeof(value) ? std::string(value, length) : std::string{};
		}

		std::string FileLabel(std::string_view a_label)
		{
			std::string label;
			for (char ch : a_label)
				label.push_back(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.' ? ch : '_');
			return label.empty() ? std::string("trace") : label;
		}

		double Median(std::vector<double> a_values)
		{
			if (a_values.empty())
				return std::nan("");
			std::ranges::sort(a_values);
			const std::size_t mid = a_values.size() / 2;
			return a_values.size() % 2 ? a_values[mid] : 0.5 * (a_values[mid - 1] + a_values[mid]);
		}

		// A metric label as a report column: the unit suffixes the metric kinds add, shortened.
		std::string ShortLabel(std::string a_label)
		{
			static const std::pair<std::string_view, std::string_view> kSuffixes[] = {
				{ ".avg.pct_of_peak_sustained_elapsed", " %" },
				{ ".avg.pct_of_peak_sustained_active", " %act" },
				{ ".avg.per_cycle_active", " /cyc" },
				{ ".sum", "" },
				{ ".ratio", "" },
			};
			for (const auto& [suffix, replacement] : kSuffixes)
				if (a_label.ends_with(suffix)) {
					a_label.resize(a_label.size() - suffix.size());
					a_label += replacement;
					break;
				}
			static const std::pair<std::string_view, std::string_view> kPrefixes[] = {
				{ "smsp__warp_issue_stalled_", "stall " },
				{ "l1tex__t_sectors_pipe_lsu_mem_local_op_", "local " },
				{ "smsp__inst_executed_pipe_", "inst " },
				{ "smsp__inst_executed", "inst" },
			};
			for (const auto& [prefix, replacement] : kPrefixes)
				if (a_label.starts_with(prefix)) {
					a_label = std::string(replacement) + a_label.substr(prefix.size());
					break;
				}
			if (const auto pos = a_label.find("_per_warp_active"); pos != std::string::npos)
				a_label.erase(pos, std::string_view("_per_warp_active").size());
			return a_label;
		}

		json Number(double a_value)
		{
			return std::isfinite(a_value) ? json(a_value) : json(nullptr);
		}

		json StatsJson(const SeriesStats& a_stats)
		{
			return { { "meanUs", Number(a_stats.meanUs) }, { "ciUs", Number(a_stats.ciUs) }, { "medianUs", Number(a_stats.medianUs) }, { "p05Us", Number(a_stats.p05Us) },
				{ "p95Us", Number(a_stats.p95Us) }, { "effectiveSamples", Number(a_stats.effectiveSamples) }, { "madOutliers", a_stats.madOutliers },
				{ "converged", a_stats.converged } };
		}

		SeriesStats StatsFrom(const json& a_entry)
		{
			SeriesStats stats;
			if (!a_entry.contains("stats") || !a_entry["stats"].is_object())
				return stats;
			const auto& value = a_entry["stats"];
			const auto number = [&](const char* a_key, double a_default) {
				return value.contains(a_key) && value[a_key].is_number() ? value[a_key].get<double>() : a_default;
			};
			stats.meanUs = number("meanUs", 0.0);
			stats.ciUs = number("ciUs", std::nan(""));
			stats.medianUs = number("medianUs", 0.0);
			stats.p05Us = number("p05Us", 0.0);
			stats.p95Us = number("p95Us", 0.0);
			stats.effectiveSamples = number("effectiveSamples", 0.0);
			stats.madOutliers = value.value("madOutliers", 0u);
			stats.converged = value.value("converged", false);
			return stats;
		}

		json ReportJson(const Report& a_report)
		{
			json rows = json::array();
			for (const auto& row : a_report.rows) {
				json metrics = json::object();
				for (const auto& [label, value] : row.metrics)
					metrics[label] = std::isnan(value) ? json(nullptr) : json(value);
				rows.push_back({ { "name", row.name }, { "group", row.group }, { "queue", row.queue }, { "timeUs", row.timeUs }, { "timeMinUs", row.timeMinUs },
					{ "timeMaxUs", row.timeMaxUs }, { "occurrencesPerFrame", row.occurrencesPerFrame }, { "samples", row.samples }, { "stats", StatsJson(row.stats) },
					{ "metrics", std::move(metrics) } });
			}
			json groups = json::array();
			for (const auto& group : a_report.groups)
				groups.push_back({ { "name", group.name }, { "timeUs", group.timeUs }, { "ranges", group.ranges }, { "stats", StatsJson(group.stats) } });
			return { { "template", a_report.templateName }, { "label", a_report.label }, { "source", SourceName(a_report.timingSource) }, { "chip", a_report.chipName },
				{ "success", a_report.success }, { "converged", a_report.converged }, { "confidence", a_report.confidence },
				{ "relativeHalfWidth", a_report.relativeHalfWidth }, { "error", a_report.error }, { "startFrame", a_report.startFrame },
				{ "endFrame", a_report.endFrame }, { "captures", a_report.captures }, { "failedCaptures", a_report.failedCaptures }, { "samples", a_report.samples },
				{ "outlierSamples", a_report.outlierSamples }, { "droppedFrames", a_report.droppedFrames }, { "metricLabels", a_report.metricLabels },
				{ "totalUs", a_report.totalUs }, { "warnings", a_report.warnings }, { "groups", std::move(groups) }, { "rows", std::move(rows) },
				{ "baseline", a_report.baseline ? a_report.baseline->label : std::string{} } };
		}

		// The capture plan of a trace: its template's metric groups, each repeats times, in that order (a repeat of every group
		// before the next repeat, so a slow drift spreads over every group alike).
		struct Planned
		{
			std::size_t group = 0;
			std::uint32_t repeat = 0;
		};

		// Set by OnGameLoaded, taken at the next present (the start a headless trace's startFrame counts from).
		std::atomic<bool> g_gameLoaded{ false };

		struct Request
		{
			Template t;
			std::string label, baseline;
			bool headless = false;
		};

		struct Runner
		{
			std::mutex mutex;  // status, latest, pending, cancel
			Status status;
			std::shared_ptr<const Report> latest;
			std::optional<Request> pending;
			bool cancel = false;

			// Render thread only.
			bool environmentHandled = false;
			bool active = false;
			Request request;
			std::vector<Planned> plan;
			std::size_t next = 0;           // the plan's capture in flight or next
			std::uint64_t captureId = 0;    // in flight: the NvPerf capture's id (0: none)
			std::uint32_t retries = 0;      // of the plan's current capture
			std::uint64_t holdUntil = 0;    // no capture is queued before this present (the profiler finishing a failed one)
			std::uint64_t gameLoadedAt = 0;  // the first present after the last game load (0: none yet)
			std::uint64_t firstDelay = 0;   // the first capture's delay
			std::vector<std::pair<std::size_t, std::shared_ptr<const CaptureResult>>> results;  // with their metric group
			std::uint32_t failed = 0;
			std::vector<std::string> warnings;
			std::uint32_t rounds = 0;  // NvPerf: rounds planned (repeats, then one per convergence check that fell short)

			// A timestamp trace's samples: one series per range (and queue), a value per frame sampled.
			struct Series
			{
				std::string name, queue, group;
				bool graphics = true;
				bool nested = false;    // a part of a pass (inside its range): out of the frame total, which its pass already counts
				bool replayed = false;  // seen in a kept recording's own queries
				std::vector<double> times;
				std::vector<std::uint16_t> occurrences;
			};
			struct TimestampRun
			{
				enum class Phase
				{
					Waiting,  // for the game, the graph's rebuild with recording timestamps
					Settling,
					Sampling,
				};
				Phase phase = Phase::Waiting;
				bool started = false;  // Timestamps::Start succeeded
				std::uint32_t settled = 0;
				std::uint64_t samplingStart = 0, lastPresent = 0;
				std::map<std::string, std::size_t> index;
				std::vector<Series> series;
				std::vector<double> totals;  // per frame sampled: the graphics queue's selected ranges
				std::uint32_t frames = 0, untimed = 0, dropped = 0, sinceCheck = 0;
			} timestamps;
		};

		Runner& Get()
		{
			static Runner runner;
			return runner;
		}

		void SetStatus(Runner& a_runner, State a_state, std::string a_error = {})
		{
			std::lock_guard lock(a_runner.mutex);
			auto& status = a_runner.status;
			status.state = a_state;
			status.templateName = a_runner.request.t.name;
			status.label = a_runner.request.label;
			status.captures = static_cast<std::uint32_t>(a_runner.plan.size());
			status.capture = static_cast<std::uint32_t>(std::min(a_runner.next + 1, a_runner.plan.size()));
			status.phase.clear();
			const auto& t = a_runner.request.t;
			if (t.timingSource == TimingSource::NvPerf && a_runner.next < a_runner.plan.size()) {
				const auto& planned = a_runner.plan[a_runner.next];
				status.phase = t.maxRepeats > t.repeats ?
				                   std::format("{}, repeat {} (at least {}, at most {})", t.metricGroups[planned.group].name, planned.repeat + 1, t.repeats, t.maxRepeats) :
				                   std::format("{}, repeat {} of {}", t.metricGroups[planned.group].name, planned.repeat + 1, t.repeats);
			}
			status.error = std::move(a_error);
		}

		void SetPhase(Runner& a_runner, std::string a_phase)
		{
			SetStatus(a_runner, State::Running);
			std::lock_guard lock(a_runner.mutex);
			a_runner.status.phase = std::move(a_phase);
		}

		void SetConvergence(Runner& a_runner, const Convergence& a_convergence, std::uint32_t a_samples)
		{
			const auto& t = a_runner.request.t;
			std::lock_guard lock(a_runner.mutex);
			auto& status = a_runner.status;
			status.convergence = true;
			status.samples = a_samples;
			status.minSamples = t.minSamples;
			status.maxSamples = t.timingSource == TimingSource::Timestamps ? t.maxSamples : 0;
			status.converged = a_convergence.converged;
			status.series = a_convergence.series;
			status.worst = a_convergence.worst;
			status.worstRelativeHalfWidth = a_convergence.worstRelative;
			status.targetRelativeHalfWidth = t.relativeHalfWidth;
		}

		/// The merged report of the trace's captures.
		Report BuildReport(const Runner& a_runner)
		{
			const auto& t = a_runner.request.t;
			Report report;
			report.templateName = t.name;
			report.label = a_runner.request.label;
			report.captures = static_cast<std::uint32_t>(a_runner.results.size());
			report.failedCaptures = a_runner.failed;
			report.warnings = a_runner.warnings;
			std::set<std::string> labels;
			for (const auto& group : t.metricGroups)
				for (const auto& text : group.metrics) {
					MetricRequest metric;
					std::string error;
					if (ParseMetric(text, metric, error) && metric.name != "gpu__time_duration" && labels.insert(MetricLabel(metric)).second)
						report.metricLabels.push_back(MetricLabel(metric));
				}

			struct Accumulated
			{
				std::vector<double> times, occurrences;
				std::map<std::string, std::vector<double>> metrics;
				std::uint32_t measured = 0;
			};
			std::map<std::string, Accumulated> byName;
			// Each group's time per sample (its rows summed; "other" the ungrouped ones), for its own statistics.
			std::map<std::string, std::vector<double>> groupTimes;
			std::set<std::string> otherQueue, unsupported;
			std::uint64_t droppedRanges = 0;
			for (const auto& [metricGroup, result] : a_runner.results) {
				if (!report.startFrame || result->startFrame < report.startFrame)
					report.startFrame = result->startFrame;
				report.endFrame = std::max(report.endFrame, result->endFrame);
				if (report.chipName.empty())
					report.chipName = result->chipName;
				otherQueue.insert(result->otherQueueRanges.begin(), result->otherQueueRanges.end());
				for (const auto& metric : result->unsupportedMetrics)
					unsupported.insert(FormatMetric(metric));
				droppedRanges += result->droppedRanges;
				const auto& metrics = result->metrics;
				std::size_t timeIndex = metrics.size();
				for (std::size_t m = 0; m < metrics.size(); ++m)
					if (metrics[m].name == "gpu__time_duration")
						timeIndex = m;
				// Per sample: each range's summed time and occurrences, its counters summed and its rates time-weighted.
				struct PerSample
				{
					double time = 0.0, occurrences = 0.0;
					std::vector<double> values, weights;
				};
				std::vector<std::map<std::string, PerSample>> samples(std::max<std::uint32_t>(result->samples, 1));
				for (const auto& range : result->rangeResults) {
					if (range.sample >= samples.size())
						continue;
					auto& entry = samples[range.sample][range.name];
					entry.values.resize(metrics.size());
					entry.weights.resize(metrics.size());
					const double time = timeIndex < range.values.size() && !std::isnan(range.values[timeIndex]) ? range.values[timeIndex] : 0.0;
					entry.time += time;
					entry.occurrences += 1.0;
					for (std::size_t m = 0; m < metrics.size() && m < range.values.size(); ++m) {
						if (m == timeIndex || std::isnan(range.values[m]))
							continue;
						const double weight = detail::IsSummed(metrics[m]) ? 1.0 : time;
						entry.values[m] += range.values[m] * weight;
						entry.weights[m] += weight;
					}
				}
				// A sample whose frame total is far from the capture's median is a hitch, not the frame being measured.
				std::vector<double> totals;
				for (const auto& sample : samples) {
					double total = 0.0;
					for (const auto& [name, entry] : sample)
						total += entry.time;
					totals.push_back(total);
				}
				const double median = Median(totals);
				std::set<std::string> names;
				for (const auto& sample : samples)
					for (const auto& [name, entry] : sample)
						names.insert(name);
				for (std::size_t s = 0; s < samples.size(); ++s) {
					if (median > 0.0 && (totals[s] > 1.5 * median || totals[s] < median / 1.5)) {
						++report.outlierSamples;
						continue;
					}
					++report.samples;
					std::map<std::string, double> groupSums;
					for (const auto& group : t.groups)
						groupSums[group.name] = 0.0;
					if (!t.groups.empty())
						groupSums["other"] = 0.0;
					for (const auto& [name, entry] : samples[s]) {
						const auto group = GroupOf(t, name);
						if (!group.empty() || !t.groups.empty())
							groupSums[group.empty() ? std::string("other") : group] += entry.time / 1000.0;
					}
					for (const auto& [group, sum] : groupSums)
						groupTimes[group].push_back(sum);
					for (const auto& name : names) {
						auto& accumulated = byName[name];
						const auto found = samples[s].find(name);
						if (found == samples[s].end()) {
							accumulated.times.push_back(0.0);
							accumulated.occurrences.push_back(0.0);
							continue;
						}
						const auto& entry = found->second;
						accumulated.times.push_back(entry.time / 1000.0);  // ns -> us
						accumulated.occurrences.push_back(entry.occurrences);
						++accumulated.measured;
						for (std::size_t m = 0; m < metrics.size(); ++m)
							if (m != timeIndex && m < entry.weights.size() && entry.weights[m] > 0.0)
								accumulated.metrics[MetricLabel(metrics[m])].push_back(
									detail::IsSummed(metrics[m]) ? entry.values[m] : entry.values[m] / entry.weights[m]);
					}
				}
			}

			for (const auto& [name, accumulated] : byName) {
				Row row;
				row.name = name;
				row.timeUs = Median(accumulated.times);
				const auto [lo, hi] = std::ranges::minmax(accumulated.times);
				row.timeMinUs = lo;
				row.timeMaxUs = hi;
				row.occurrencesPerFrame = Median(accumulated.occurrences);
				row.samples = accumulated.measured;
				for (const auto& [label, values] : accumulated.metrics)
					row.metrics[label] = Median(values);
				row.group = GroupOf(t, name);
				row.stats = Summarize(accumulated.times, t);
				report.totalUs += row.timeUs;
				report.rows.push_back(std::move(row));
			}
			std::ranges::sort(report.rows, [](const Row& a, const Row& b) { return a.timeUs > b.timeUs; });

			for (const auto& group : t.groups) {
				GroupRow entry{ group.name };
				for (const auto& row : report.rows)
					if (row.group == group.name) {
						entry.timeUs += row.timeUs;
						++entry.ranges;
					}
				entry.stats = Summarize(groupTimes[group.name], t);
				if (entry.ranges)
					report.groups.push_back(std::move(entry));
			}
			if (!t.groups.empty()) {
				GroupRow other{ "other" };
				for (const auto& row : report.rows)
					if (row.group.empty()) {
						other.timeUs += row.timeUs;
						++other.ranges;
					}
				other.stats = Summarize(groupTimes["other"], t);
				if (other.ranges)
					report.groups.push_back(std::move(other));
			}
			// The target is the report's only when the trace captured until it (maxRepeats); the intervals are there either way.
			report.timingSource = TimingSource::NvPerf;
			if (t.maxRepeats > t.repeats) {
				report.confidence = t.confidence;
				report.relativeHalfWidth = t.relativeHalfWidth;
			}
			report.converged = Statistics::Available() && CheckConvergence(report).All();

			// What the captures could not see, or saw unreliably.
			if (!otherQueue.empty()) {
				std::string names;
				for (const auto& name : otherQueue)
					names += std::format("{}{}", names.empty() ? "" : ", ", name);
				report.warnings.push_back(std::format("{} selected passes ran on the compute or copy queue and are not measured (CS_DCLF_ASYNC_COMPUTE=0 moves DCLF's to the graphics queue): {}",
					otherQueue.size(), names));
			}
			for (const auto& metric : unsupported)
				report.warnings.push_back(std::format("unavailable on this GPU: {}", metric));
			for (const auto& filter : t.ranges)
				if (!std::ranges::any_of(report.rows, [&](const Row& row) { return detail::NameMatches(filter, row.name); }))
					report.warnings.push_back(std::format("range filter '{}' matched no measured range", filter));
			if (report.outlierSamples)
				report.warnings.push_back(std::format("{} of {} samples dropped as hitches (frame total beyond 1.5x its capture's median)", report.outlierSamples,
					report.outlierSamples + report.samples));
			if (droppedRanges)
				report.warnings.push_back(std::format("the profiler dropped {} ranges", droppedRanges));
			std::string unstable;
			std::size_t unstableCount = 0;
			for (const auto& row : report.rows)
				if (row.timeUs >= 20.0 && row.timeMaxUs - row.timeMinUs > 0.5 * row.timeUs && unstableCount++ < 6)
					unstable += std::format("{}{} ({:.0f}-{:.0f} us)", unstable.empty() ? "" : ", ", row.name, row.timeMinUs, row.timeMaxUs);
			if (unstableCount)
				report.warnings.push_back(std::format("{} rows vary by more than half their median across samples: {}{}", unstableCount, unstable, unstableCount > 6 ? ", ..." : ""));
			if (t.maxRepeats > t.repeats && !report.converged && Statistics::Available()) {
				std::size_t open = 0;
				const auto rows = Unconverged(report, open);
				report.warnings.push_back(std::format("not converged after {} rounds ({} rows open, +/-{:.1f}% at {:.0f}% confidence asked): {}{}", a_runner.rounds, open,
					100.0 * t.relativeHalfWidth, 100.0 * t.confidence, rows.empty() ? "only groups" : rows, open > 6 ? ", ..." : ""));
			}
			report.success = !report.rows.empty();
			if (!report.success)
				report.error = a_runner.results.empty() ? "every capture failed" : "no range was measured";
			return report;
		}

		const char* QueueName(Timestamps::Queue a_queue)
		{
			return a_queue == Timestamps::Queue::Graphics ? "graphics" : a_queue == Timestamps::Queue::Compute ? "compute" : "copy";
		}

		/// One frame of a timestamp trace into its series: each selected range's time summed over its occurrences (0 without one).
		void AddTimestampFrame(Runner& a_runner, const Timestamps::Frame& a_frame)
		{
			const auto& t = a_runner.request.t;
			auto& run = a_runner.timestamps;
			std::vector<std::pair<double, std::uint16_t>> current(run.series.size());
			const auto addSeries = [&](const std::string& a_key, Timestamps::Queue a_queue, bool a_nested, std::string a_group) -> std::size_t {
				const auto [found, inserted] = run.index.try_emplace(a_key, run.series.size());
				if (inserted) {
					Runner::Series series;
					series.name = a_key;
					series.queue = QueueName(a_queue);
					series.graphics = a_queue == Timestamps::Queue::Graphics;
					series.nested = a_nested;
					series.group = std::move(a_group);
					series.times.assign(run.frames, 0.0);
					series.occurrences.assign(run.frames, 0);
					run.series.push_back(std::move(series));
					current.emplace_back();
				}
				return found->second;
			};
			// The spans, over every range of the frame (the template's filter does not apply to them).
			for (const auto& span : t.spans) {
				std::uint64_t first = UINT64_MAX, last = 0;
				double usPerTick = 0.0;
				for (const auto& range : a_frame.ranges)
					if (range.queue == Timestamps::Queue::Graphics && range.endTick >= range.beginTick && range.beginTick &&
						std::ranges::any_of(span.ranges, [&](const std::string& a_filter) { return detail::NameMatches(a_filter, range.name); })) {
						first = std::min(first, range.beginTick);
						last = std::max(last, range.endTick);
						usPerTick = range.usPerTick;
					}
				// A group name no group has: out of every group's sum and "other".
				const auto at = addSeries("span:" + span.name, Timestamps::Queue::Graphics, true, "(span)");
				if (first <= last) {
					current[at].first = static_cast<double>(last - first) * usPerTick;
					current[at].second = 1;
				}
			}
			for (const auto& range : a_frame.ranges) {
				if (!t.ranges.empty() && !detail::MatchingFilter(t.ranges, range.name))
					continue;
				const bool graphics = range.queue == Timestamps::Queue::Graphics;
				const auto key = graphics ? range.name : std::format("{} [{}]", range.name, QueueName(range.queue));
				// Another queue's ticks overlap the graphics queue's work: its rows stay out of the groups and the total.
				const auto at = addSeries(key, range.queue, range.nested, graphics ? GroupOf(t, range.name) : std::string{});
				auto& [time, occurrences] = current[at];
				time += range.us;
				occurrences = static_cast<std::uint16_t>(std::min<std::uint32_t>(occurrences + 1u, 0xffffu));
				run.series[at].replayed |= range.replayed;
			}
			double total = 0.0;
			for (std::size_t i = 0; i < run.series.size(); ++i) {
				auto& series = run.series[i];
				series.times.push_back(current[i].first);
				series.occurrences.push_back(current[i].second);
				if (series.graphics && !series.nested)
					total += current[i].first;
			}
			run.totals.push_back(total);
			++run.frames;
		}

		/// A timestamp trace's report: its series with hitch frames left out.
		Report BuildTimestampReport(const Runner& a_runner)
		{
			const auto& t = a_runner.request.t;
			const auto& run = a_runner.timestamps;
			Report report;
			report.templateName = t.name;
			report.label = a_runner.request.label;
			report.timingSource = TimingSource::Timestamps;
			report.confidence = t.confidence;
			report.relativeHalfWidth = t.relativeHalfWidth;
			report.startFrame = run.samplingStart;
			report.endFrame = run.lastPresent;
			report.droppedFrames = run.dropped;
			report.warnings = a_runner.warnings;

			// A frame whose total is far from the median's is a hitch (a pipeline compile, a streaming spike), as in a capture.
			const double median = Median(run.totals);
			std::vector<std::uint8_t> kept(run.frames, 1);
			for (std::uint32_t f = 0; f < run.frames; ++f)
				if (median > 0.0 && (run.totals[f] > 1.5 * median || run.totals[f] < median / 1.5)) {
					kept[f] = 0;
					++report.outlierSamples;
				}
			report.samples = run.frames - report.outlierSamples;
			const auto filtered = [&](const auto& a_values) {
				std::vector<double> out;
				out.reserve(report.samples);
				for (std::uint32_t f = 0; f < run.frames && f < a_values.size(); ++f)
					if (kept[f])
						out.push_back(static_cast<double>(a_values[f]));
				return out;
			};

			std::map<std::string, std::vector<double>> groupTimes;
			const auto addToGroup = [&](const std::string& a_group, const std::vector<double>& a_times) {
				auto& sums = groupTimes[a_group];
				sums.resize(a_times.size(), 0.0);
				for (std::size_t i = 0; i < a_times.size(); ++i)
					sums[i] += a_times[i];
			};
			std::uint32_t replayed = 0, otherQueue = 0;
			for (const auto& series : run.series) {
				Row row;
				row.name = series.name;
				row.group = series.group;
				row.queue = series.queue;
				const auto times = filtered(series.times);
				const auto occurrences = filtered(series.occurrences);
				row.stats = Summarize(times, t);
				row.timeUs = row.stats.meanUs;
				if (!times.empty()) {
					const auto [lo, hi] = std::ranges::minmax(times);
					row.timeMinUs = lo;
					row.timeMaxUs = hi;
				}
				row.occurrencesPerFrame = Median(occurrences);
				if (std::isnan(row.occurrencesPerFrame))
					row.occurrencesPerFrame = 0.0;
				row.samples = static_cast<std::uint32_t>(std::ranges::count_if(occurrences, [](double a_count) { return a_count > 0.0; }));
				replayed += series.replayed;
				if (series.graphics) {
					if (!series.nested)
						report.totalUs += row.timeUs;
					if (!t.groups.empty())
						addToGroup(row.group.empty() ? std::string("other") : row.group, times);
				} else {
					++otherQueue;
				}
				report.rows.push_back(std::move(row));
			}
			std::ranges::sort(report.rows, [](const Row& a, const Row& b) { return a.timeUs > b.timeUs; });

			if (!t.seriesFile.empty()) {
				std::ofstream csv(t.seriesFile, std::ios::trunc);
				if (!csv)
					report.warnings.push_back(std::format("series file '{}' could not be written", t.seriesFile.string()));
				csv << "frame,hitch";
				for (const auto& series : run.series)
					csv << ',' << series.name << (series.graphics ? std::string() : std::format("[{}]", series.queue));
				csv << '\n';
				for (std::uint32_t f = 0; f < run.frames; ++f) {
					csv << f << ',' << (kept[f] ? 0 : 1);
					for (const auto& series : run.series)
						csv << std::format(",{:.2f}", f < series.times.size() ? static_cast<double>(series.times[f]) : 0.0);
					csv << '\n';
				}
			}

			auto groupNames = std::vector<std::string>{};
			for (const auto& group : t.groups)
				groupNames.push_back(group.name);
			if (!t.groups.empty())
				groupNames.push_back("other");
			for (const auto& name : groupNames) {
				GroupRow entry{ name };
				for (const auto& row : report.rows)
					if (row.queue == "graphics" && (row.group == name || (name == "other" && row.group.empty()))) {
						entry.timeUs += row.timeUs;
						++entry.ranges;
					}
				if (!entry.ranges)
					continue;
				entry.stats = Summarize(groupTimes[name], t);
				report.groups.push_back(std::move(entry));
			}
			const auto convergence = CheckConvergence(report);
			report.converged = Statistics::Available() && convergence.All();

			if (otherQueue)
				report.warnings.push_back(std::format("{} ranges ran on the compute or copy queue: their own rows (\"[compute]\"), left out of the groups and the total",
					otherQueue));
			if (!replayed)
				report.warnings.push_back("no range came from a recording kept across frames: DCLF's revision epochs were not timed (expected with DCLF off)");
			for (const auto& filter : t.ranges)
				if (!std::ranges::any_of(run.series, [&](const Runner::Series& series) { return detail::NameMatches(filter, series.name); }))
					report.warnings.push_back(std::format("range filter '{}' matched no timestamped range", filter));
			if (report.outlierSamples)
				report.warnings.push_back(std::format("{} of {} frames dropped as hitches (selected total beyond 1.5x the median's)", report.outlierSamples, run.frames));
			if (run.dropped)
				report.warnings.push_back(std::format("{} frames dropped: their epochs did not all come back", run.dropped));
			if (run.untimed)
				report.warnings.push_back(std::format("{} frames carried no timestamps and were skipped", run.untimed));
			if (!report.converged && Statistics::Available()) {
				std::size_t open = 0;
				const auto rows = Unconverged(report, open);
				report.warnings.push_back(std::format("not converged after {} frames ({} rows open, +/-{:.1f}% at {:.0f}% confidence asked): {}{}", run.frames, open,
					100.0 * t.relativeHalfWidth, 100.0 * t.confidence, rows.empty() ? "only groups" : rows, open > 6 ? ", ..." : ""));
			}
			if (!Statistics::Available())
				report.warnings.push_back("built without BasicTelemetry: no confidence intervals, nothing converges (the trace ran to maxSamples)");
			report.success = !report.rows.empty();
			if (!report.success)
				report.error = run.frames ? "no selected range was timestamped" : "no frame was sampled";
			return report;
		}

		void Finish(Runner& a_runner, std::string a_error = {})
		{
			const bool timestamps = a_runner.request.t.timingSource == TimingSource::Timestamps;
			if (timestamps)
				Timestamps::Stop();
			auto report = timestamps ? BuildTimestampReport(a_runner) : BuildReport(a_runner);
			if (!a_error.empty()) {
				report.success = false;
				report.error = a_error;
			}
			if (!a_runner.request.baseline.empty()) {
				std::string error;
				report.baseline = LoadReport(a_runner.request.baseline, error);
				if (!report.baseline)
					report.warnings.push_back(std::format("baseline '{}': {}", a_runner.request.baseline, error));
			}
			std::error_code ec;
			const auto directory = ReportDirectory();
			std::filesystem::create_directories(directory, ec);
			const auto stem = FileLabel(report.label);
			report.jsonPath = directory / (stem + ".json");
			report.textPath = directory / (stem + ".txt");
			const auto text = FormatReport(report);
			if (std::ofstream file(report.jsonPath); file)
				file << ReportJson(report).dump(1, '\t');
			if (std::ofstream file(report.textPath); file)
				file << text;
			std::istringstream lines(text);
			for (std::string line; std::getline(lines, line);)
				logger::info("[NvPerf] {}", line);
			logger::info("[NvPerf] trace report: {}", report.textPath.string());

			auto published = std::make_shared<const Report>(std::move(report));
			a_runner.active = false;
			a_runner.captureId = 0;
			SetStatus(a_runner, published->success ? State::Done : State::Failed, published->error);
			std::lock_guard lock(a_runner.mutex);
			a_runner.latest = std::move(published);
		}

		// Queues the plan's next capture through the capture front end; false while it is busy with another.
		void QueueNext(Runner& a_runner, std::uint64_t a_presents)
		{
			const auto& t = a_runner.request.t;
			const auto& planned = a_runner.plan[a_runner.next];
			CaptureConfiguration configuration;
			configuration.ranges = t.ranges;
			configuration.samples = t.samples;
			for (const auto& text : t.metricGroups[planned.group].metrics) {
				MetricRequest metric;
				std::string error;
				if (ParseMetric(text, metric, error))
					configuration.metrics.push_back(std::move(metric));
			}
			if (a_runner.next == 0 && a_runner.retries == 0 && a_runner.firstDelay > a_presents)
				configuration.delayFrames = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_runner.firstDelay - a_presents, 10000));
			std::uint64_t id = 0;
			std::string error;
			if (RequestCapture(configuration, id, error)) {
				a_runner.captureId = id;
				return;
			}
			if (error.find("in progress") != std::string::npos)
				return;  // another capture (the panel's): the trace waits for it
			a_runner.warnings.push_back(std::format("capture {} ({}) could not start: {}", a_runner.next + 1, t.metricGroups[planned.group].name, error));
			++a_runner.failed;
			++a_runner.next;
		}

		void StartLocked(Runner& a_runner, Request a_request, std::uint64_t a_presents)
		{
			a_runner.request = std::move(a_request);
			const auto& t = a_runner.request.t;
			a_runner.plan.clear();
			if (t.timingSource == TimingSource::NvPerf)
				for (std::uint32_t r = 0; r < t.repeats; ++r)
					for (std::size_t g = 0; g < t.metricGroups.size(); ++g)
						a_runner.plan.push_back({ g, r });
			a_runner.rounds = t.repeats;
			a_runner.next = 0;
			a_runner.captureId = 0;
			a_runner.retries = 0;
			a_runner.holdUntil = 0;
			a_runner.results.clear();
			a_runner.failed = 0;
			a_runner.warnings.clear();
			a_runner.timestamps = {};
			a_runner.firstDelay = a_runner.request.headless ? 0 : a_presents;  // headless: set once a game loaded (OnPresentEnd)
			a_runner.active = true;
			const auto ranges = t.ranges.empty() ? std::string("every outermost") : std::to_string(t.ranges.size());
			if (t.timingSource == TimingSource::Timestamps)
				logger::info("[NvPerf] trace '{}' (template {}, timestamps): every frame a sample after {} settling frames, {}-{} samples, +/-{:.1f}% (or {:.1f} us) at {:.0f}% confidence, ranges {}",
					a_runner.request.label, t.name, t.settlingFrames, t.minSamples, t.maxSamples, 100.0 * t.relativeHalfWidth, t.absoluteHalfWidthUs,
					100.0 * t.confidence, ranges);
			else
				logger::info("[NvPerf] trace '{}' (template {}): {} captures of {} samples{}, ranges {}", a_runner.request.label, t.name, a_runner.plan.size(), t.samples,
					t.maxRepeats > t.repeats ? std::format(", up to {} rounds until +/-{:.1f}% at {:.0f}% confidence", t.maxRepeats, 100.0 * t.relativeHalfWidth, 100.0 * t.confidence) : "",
					ranges);
		}

		/// A headless trace starts startFrame presents after a game loaded (firstDelay): false while no game has.
		bool WaitForGame(Runner& a_runner)
		{
			if (!a_runner.request.headless || a_runner.firstDelay)
				return true;
			if (!a_runner.gameLoadedAt) {
				SetPhase(a_runner, "waiting for a game to load");
				return false;
			}
			a_runner.firstDelay = a_runner.gameLoadedAt + a_runner.request.t.startFrame;
			logger::info("[NvPerf] trace '{}': a game loaded at present {}; its first {} starts at present {}", a_runner.request.label, a_runner.gameLoadedAt,
				a_runner.request.t.timingSource == TimingSource::Timestamps ? "sample" : "capture", a_runner.firstDelay);
			return true;
		}

		/// NvPerf convergence (maxRepeats > repeats): once the planned captures are done, whether another round is planned, for a
		/// row or group whose time has not converged.
		bool PlanAnotherRound(Runner& a_runner)
		{
			const auto& t = a_runner.request.t;
			if (t.timingSource != TimingSource::NvPerf || t.maxRepeats <= t.repeats || a_runner.results.empty())
				return false;
			if (!Statistics::Available()) {
				a_runner.warnings.push_back("built without BasicTelemetry: no convergence, the trace captured its repeats only");
				return false;
			}
			const auto report = BuildReport(a_runner);
			const auto convergence = CheckConvergence(report);
			SetConvergence(a_runner, convergence, report.samples);
			if (convergence.All() || a_runner.rounds >= t.maxRepeats) {
				logger::info("[NvPerf] trace '{}': {} of {} rows and groups converged after {} rounds ({} samples)", a_runner.request.label, convergence.converged,
					convergence.series, a_runner.rounds, report.samples);
				return false;
			}
			for (std::size_t g = 0; g < t.metricGroups.size(); ++g)
				a_runner.plan.push_back({ g, a_runner.rounds });
			++a_runner.rounds;
			logger::info("[NvPerf] trace '{}': {} of {} rows and groups converged ({} samples; furthest {} at +/-{:.1f}%): round {} of at most {}",
				a_runner.request.label, convergence.converged, convergence.series, report.samples, convergence.worst, 100.0 * convergence.worstRelative,
				a_runner.rounds, t.maxRepeats);
			return true;
		}

		/// A timestamp trace's step, each present: start collecting, wait for the game and the rebuild, settle, then sample every
		/// frame until it converges (checked every kCheckFrames frames), maxSamples frames or timeoutFrames presents.
		void AdvanceTimestamps(Runner& a_runner, std::uint64_t a_presents)
		{
			// A check summarizes every series (sorts included): every kCheckFrames frames, or every tenth of the frames so far.
			static constexpr std::uint32_t kCheckFrames = 30;
			static constexpr std::uint64_t kNoFramesLimit = 600;  // presents sampling may see no timed frame before the trace fails
			const auto& t = a_runner.request.t;
			auto& run = a_runner.timestamps;
			run.lastPresent = a_presents;
			if (!run.started) {
				std::string error;
				if (!Timestamps::Start(error)) {
					Finish(a_runner, error);
					return;
				}
				run.started = true;
			}
			std::uint32_t dropped = 0;
			auto frames = Timestamps::TakeFrames(dropped);
			if (run.phase == Runner::TimestampRun::Phase::Waiting) {
				if (a_presents >= a_runner.firstDelay + t.timeoutFrames) {
					Finish(a_runner, "the graph was not rebuilt with recording timestamps (no build point reached?)");
					return;
				}
				if (a_presents < a_runner.firstDelay || !Timestamps::Ready()) {
					SetPhase(a_runner, a_presents < a_runner.firstDelay ? std::format("waiting for present {} (now {})", a_runner.firstDelay, a_presents) :
					                                                       std::string("waiting for the graph's rebuild with recording timestamps"));
					return;
				}
				run.phase = Runner::TimestampRun::Phase::Settling;
				logger::info("[NvPerf] trace '{}': recording timestamps are in the graph at present {}; settling {} frames", a_runner.request.label, a_presents,
					t.settlingFrames);
				return;
			}
			if (run.phase == Runner::TimestampRun::Phase::Settling) {
				run.settled += static_cast<std::uint32_t>(frames.size());
				if (run.settled < t.settlingFrames) {
					SetPhase(a_runner, std::format("settling, {} of {} frames", run.settled, t.settlingFrames));
					return;
				}
				run.phase = Runner::TimestampRun::Phase::Sampling;
				run.samplingStart = a_presents;
				logger::info("[NvPerf] trace '{}': sampling from present {}", a_runner.request.label, a_presents);
				return;
			}
			run.dropped += dropped;
			if (!run.frames && a_presents - run.samplingStart >= kNoFramesLimit) {
				Finish(a_runner, std::format("no frame with timestamps came back in {} presents (is the render graph running epochs?)", kNoFramesLimit));
				return;
			}
			for (const auto& frame : frames) {
				if (!frame.timedEpochs) {
					++run.untimed;
					continue;
				}
				AddTimestampFrame(a_runner, frame);
				++run.sinceCheck;
			}
			const bool full = run.frames >= t.maxSamples;
			const bool timedOut = a_presents - run.samplingStart >= t.timeoutFrames;
			if (full || timedOut || (run.sinceCheck >= std::max(kCheckFrames, run.frames / 10) && run.frames >= t.minSamples)) {
				run.sinceCheck = 0;
				const auto report = BuildTimestampReport(a_runner);
				const auto convergence = CheckConvergence(report);
				SetConvergence(a_runner, convergence, report.samples);
				if (report.converged || full || timedOut) {
					logger::info("[NvPerf] trace '{}': {} of {} rows and groups converged after {} frames ({} sampled){}", a_runner.request.label, convergence.converged,
						convergence.series, a_presents - run.samplingStart, run.frames, full ? ", maxSamples reached" : timedOut ? ", timeoutFrames reached" : "");
					Finish(a_runner);
					return;
				}
			}
			SetPhase(a_runner, std::format("sampling, {} frames", run.frames));
		}

		std::optional<Template> FindTemplate(std::string_view a_name)
		{
			for (auto& t : Templates())
				if (t.name == a_name)
					return t;
			return std::nullopt;
		}

		// CS_NVPERF_TEMPLATE: once the profiler can run (under DXVK, after the render graph registered its device).
		void HandleEnvironment(Runner& a_runner)
		{
			if (a_runner.environmentHandled || !EnvironmentRequested())
				return;
			const auto name = Env("CS_NVPERF_TEMPLATE");
			auto t = FindTemplate(name);
			// A timestamp trace needs no profiler: only an NvPerf one waits for Probe (under DXVK, the graph's registration).
			if (t && t->timingSource == TimingSource::NvPerf && !Probe()) {
				const auto device = GetDeviceInfo();
				if (device.probed || !device.built) {
					a_runner.environmentHandled = true;
					logger::warn("[NvPerf] CS_NVPERF_TEMPLATE: no trace: {}", device.error);
				}
				return;
			}
			a_runner.environmentHandled = true;
			if (!t) {
				logger::warn("[NvPerf] CS_NVPERF_TEMPLATE: no template '{}'", name);
				return;
			}
			const auto apply = [](const char* a_name, auto& a_field) {
				if (const auto text = Env(a_name); !text.empty())
					a_field = static_cast<std::remove_reference_t<decltype(a_field)>>(std::strtoull(text.c_str(), nullptr, 10));
			};
			apply("CS_NVPERF_START_FRAME", t->startFrame);
			apply("CS_NVPERF_SAMPLES", t->samples);
			apply("CS_NVPERF_REPEATS", t->repeats);
			apply("CS_NVPERF_MAX_REPEATS", t->maxRepeats);
			apply("CS_NVPERF_MIN_SAMPLES", t->minSamples);
			apply("CS_NVPERF_MAX_SAMPLES", t->maxSamples);
			if (const auto text = Env("CS_NVPERF_HALF_WIDTH"); !text.empty())
				t->relativeHalfWidth = std::strtod(text.c_str(), nullptr);
			t->samples = std::clamp<std::uint32_t>(t->samples, 1, 64);
			t->repeats = std::clamp<std::uint32_t>(t->repeats, 1, 16);
			t->maxRepeats = std::min<std::uint32_t>(t->maxRepeats, 64);
			const auto bounds = StatisticsConfig(*t);
			t->relativeHalfWidth = bounds.relativeHalfWidth;
			t->minSamples = bounds.minSamples;
			t->maxSamples = bounds.maxSamples;
			Request request;
			request.t = std::move(*t);
			request.label = Env("CS_NVPERF_LABEL");
			if (request.label.empty())
				request.label = request.t.name;
			request.baseline = Env("CS_NVPERF_BASELINE");
			request.headless = true;
			std::lock_guard lock(a_runner.mutex);
			a_runner.pending = std::move(request);
			a_runner.status = Status{ State::Running, a_runner.pending->t.name, a_runner.pending->label };
		}
	}

	std::vector<Template> Templates()
	{
		auto templates = BuiltInTemplates();
		auto& store = Store();
		std::lock_guard lock(store.mutex);
		if (!store.loaded)
			LoadLocked(store);
		for (const auto& file : store.files) {
			std::erase_if(templates, [&](const Template& t) { return t.name == file.name; });
			templates.push_back(file);
		}
		return templates;
	}

	std::string ReloadTemplates()
	{
		auto& store = Store();
		std::lock_guard lock(store.mutex);
		LoadLocked(store);
		return store.errors;
	}

	std::filesystem::path TemplateDirectory()
	{
		return Util::PathHelpers::GetCommunityShaderPath() / "NvPerfTemplates";  // not NvPerf: the AIO staging removes that old runtime directory
	}

	std::filesystem::path ReportDirectory()
	{
		return Util::PathHelpers::GetLogPath().parent_path() / "NvPerf";
	}

	std::vector<std::string> SavedReports()
	{
		std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(ReportDirectory(), ec))
			if (entry.is_regular_file() && entry.path().extension() == ".json")
				found.emplace_back(entry.last_write_time(ec), entry.path().stem().string());
		std::ranges::sort(found, std::greater<>{});
		std::vector<std::string> labels;
		for (auto& [time, label] : found)
			labels.push_back(std::move(label));
		return labels;
	}

	std::shared_ptr<const Report> LoadReport(std::string_view a_labelOrPath, std::string& a_error)
	{
		std::filesystem::path path(a_labelOrPath);
		if (path.extension() != ".json")
			path = ReportDirectory() / (FileLabel(a_labelOrPath) + ".json");
		std::ifstream file(path);
		if (!file) {
			a_error = std::format("no report at {}", path.string());
			return nullptr;
		}
		const json document = json::parse(file, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			a_error = std::format("{} is not a trace report", path.string());
			return nullptr;
		}
		auto report = std::make_shared<Report>();
		report->templateName = document.value("template", "");
		report->label = document.value("label", "");
		report->timingSource = document.value("source", "") == "timestamps" ? TimingSource::Timestamps : TimingSource::NvPerf;
		report->chipName = document.value("chip", "");
		report->success = document.value("success", false);
		report->converged = document.value("converged", false);
		report->confidence = document.value("confidence", 0.0);
		report->relativeHalfWidth = document.value("relativeHalfWidth", 0.0);
		report->totalUs = document.value("totalUs", 0.0);
		report->samples = document.value("samples", 0u);
		report->jsonPath = path;
		if (document.contains("metricLabels") && document["metricLabels"].is_array())
			for (const auto& label : document["metricLabels"])
				if (label.is_string())
					report->metricLabels.push_back(label.get<std::string>());
		if (document.contains("groups") && document["groups"].is_array())
			for (const auto& group : document["groups"])
				report->groups.push_back({ group.value("name", ""), group.value("timeUs", 0.0), group.value("ranges", 0u), StatsFrom(group) });
		if (document.contains("rows") && document["rows"].is_array())
			for (const auto& entry : document["rows"]) {
				Row row;
				row.name = entry.value("name", "");
				row.group = entry.value("group", "");
				row.queue = entry.value("queue", "");
				row.stats = StatsFrom(entry);
				row.timeUs = entry.value("timeUs", 0.0);
				row.timeMinUs = entry.value("timeMinUs", 0.0);
				row.timeMaxUs = entry.value("timeMaxUs", 0.0);
				row.occurrencesPerFrame = entry.value("occurrencesPerFrame", 0.0);
				row.samples = entry.value("samples", 0u);
				if (entry.contains("metrics") && entry["metrics"].is_object())
					for (const auto& [label, value] : entry["metrics"].items())
						row.metrics[label] = value.is_number() ? value.get<double>() : std::nan("");
				report->rows.push_back(std::move(row));
			}
		return report;
	}

	namespace
	{
		bool StartRequest(Template a_template, std::string_view a_label, std::string_view a_baseline, std::string& a_error);
	}

	bool Start(std::string_view a_templateName, std::string_view a_label, std::string_view a_baseline, std::string& a_error)
	{
		auto t = FindTemplate(a_templateName);
		if (!t) {
			a_error = std::format("no template '{}'", a_templateName);
			return false;
		}
		return StartRequest(std::move(*t), a_label, a_baseline, a_error);
	}

	bool StartInline(std::string_view a_templateJson, std::string_view a_label, std::string_view a_baseline, std::string& a_error)
	{
		const auto value = json::parse(a_templateJson, nullptr, false);
		if (value.is_discarded()) {
			a_error = "the template is not valid JSON";
			return false;
		}
		Template t;
		if (!ParseTemplate(value, Templates(), t, a_error))
			return false;
		return StartRequest(std::move(t), a_label, a_baseline, a_error);
	}

	std::string ReportToJson(const Report& a_report)
	{
		return ReportJson(a_report).dump();
	}

	namespace
	{
	bool StartRequest(Template a_template, std::string_view a_label, std::string_view a_baseline, std::string& a_error)
	{
		auto& runner = Get();
		std::lock_guard lock(runner.mutex);
		if (runner.pending || runner.status.state == State::Running) {
			a_error = std::format("trace '{}' is still running", runner.status.label);
			return false;
		}
		Request request;
		request.t = std::move(a_template);
		request.label = a_label.empty() ? request.t.name : std::string(a_label);
		request.baseline = std::string(a_baseline);
		runner.pending = std::move(request);
		runner.cancel = false;
		runner.status = Status{ State::Running, runner.pending->t.name, runner.pending->label };
		return true;
	}
	}

	void Cancel()
	{
		auto& runner = Get();
		std::lock_guard lock(runner.mutex);
		runner.cancel = true;
	}

	Status GetStatus()
	{
		auto& runner = Get();
		std::lock_guard lock(runner.mutex);
		return runner.status;
	}

	std::shared_ptr<const Report> Latest()
	{
		auto& runner = Get();
		std::lock_guard lock(runner.mutex);
		return runner.latest;
	}

	std::string FormatReport(const Report& a_report)
	{
		const Report* baseline = a_report.baseline.get();
		const bool timestamps = a_report.timingSource == TimingSource::Timestamps;
		std::string out = timestamps ?
		                      std::format("trace '{}' (template {}, timestamps): {} frames sampled{}{}, presents {}-{}\n", a_report.label, a_report.templateName,
								  a_report.samples, a_report.outlierSamples ? std::format(" ({} dropped as hitches)", a_report.outlierSamples) : "",
								  a_report.droppedFrames ? std::format(" ({} incomplete)", a_report.droppedFrames) : "", a_report.startFrame, a_report.endFrame) :
		                      std::format("trace '{}' (template {}, chip {}): {} captures{}, {} samples{}, frames {}-{}\n", a_report.label, a_report.templateName,
								  a_report.chipName, a_report.captures, a_report.failedCaptures ? std::format(" ({} failed)", a_report.failedCaptures) : "", a_report.samples,
								  a_report.outlierSamples ? std::format(" ({} dropped as hitches)", a_report.outlierSamples) : "", a_report.startFrame, a_report.endFrame);
		if (a_report.confidence > 0.0)
			out += std::format("{}: every row's and group's mean within +/-{:.1f}% at {:.0f}% confidence{}\n", a_report.converged ? "converged" : "NOT converged",
				100.0 * a_report.relativeHalfWidth, 100.0 * a_report.confidence, timestamps ? "" : " (gpu__time_duration per sample)");
		if (!a_report.success)
			out += std::format("FAILED: {}\n", a_report.error);
		const auto ci = [](const SeriesStats& a_stats) { return std::isfinite(a_stats.ciUs) ? std::format("{:.2f}", a_stats.ciUs) : std::string("-"); };
		// A delta, with '*' when the two means differ by more than their intervals combined.
		const auto delta = [](double a_value, double a_base, const SeriesStats* a_stats, const SeriesStats* a_baseStats) {
			const bool significant = a_stats && a_baseStats && Statistics::Significant(a_stats->meanUs, a_stats->ciUs, a_baseStats->meanUs, a_baseStats->ciUs);
			return std::format("{:+9.1f}{}", a_value - a_base, significant ? "*" : " ");
		};
		out += std::format("total {:.1f} us{}\n", a_report.totalUs,
			baseline ? std::format(" (baseline '{}' {:.1f} us, {:+.1f})", baseline->label, baseline->totalUs, a_report.totalUs - baseline->totalUs) : "");
		if (!a_report.groups.empty()) {
			out += std::format("\n{:<28} {:>9} {:>7} {:>7} {:>4}{}\n", "group", "time us", "+/-", "n_eff", "conv",
				baseline ? std::format(" {:>9} {:>10}", "baseline", "delta") : "");
			std::set<std::string> listed;
			for (const auto& group : a_report.groups) {
				listed.insert(group.name);
				std::string compared;
				if (baseline) {
					const auto found = std::ranges::find_if(baseline->groups, [&](const GroupRow& g) { return g.name == group.name; });
					const double base = found == baseline->groups.end() ? 0.0 : found->timeUs;
					compared = std::format(" {:>9.1f} {}", base, delta(group.timeUs, base, &group.stats, found == baseline->groups.end() ? nullptr : &found->stats));
				}
				out += std::format("{:<28} {:>9.1f} {:>7} {:>7.0f} {:>4}{}\n", group.name, group.timeUs, ci(group.stats), group.stats.effectiveSamples,
					group.stats.converged ? "yes" : "no", compared);
			}
			if (baseline)
				for (const auto& group : baseline->groups)
					if (!listed.contains(group.name))
						out += std::format("{:<28} {:>9.1f} {:>7} {:>7} {:>4} {:>9.1f} {}\n", group.name, 0.0, "", "", "", group.timeUs, delta(0.0, group.timeUs, nullptr, nullptr));
		}
		out += std::format("\n{:<48} {:<16} {:>9} {:>7} {:>17} {:>17} {:>6} {:>6} {:>4}{}", "range", "group", "time us", "+/-", "p05-p95", "min-max", "/frame", "n_eff",
			"conv", baseline ? std::format(" {:>10}", "delta") : "");
		for (const auto& label : a_report.metricLabels)
			out += std::format(" {:>14}", ShortLabel(label).substr(0, 14));
		out += '\n';
		std::set<std::string> listed;
		for (const auto& row : a_report.rows) {
			listed.insert(row.name);
			std::string compared;
			if (baseline) {
				const auto found = std::ranges::find_if(baseline->rows, [&](const Row& r) { return r.name == row.name; });
				compared = std::format(" {}", delta(row.timeUs, found == baseline->rows.end() ? 0.0 : found->timeUs, &row.stats,
												  found == baseline->rows.end() ? nullptr : &found->stats));
			}
			out += std::format("{:<48} {:<16} {:>9.1f} {:>7} {:>8.1f}-{:<8.1f} {:>8.1f}-{:<8.1f} {:>6.3g} {:>6.0f} {:>4}{}", row.name.substr(0, 48),
				row.group.substr(0, 16), row.timeUs, ci(row.stats), row.stats.p05Us, row.stats.p95Us, row.timeMinUs, row.timeMaxUs, row.occurrencesPerFrame,
				row.stats.effectiveSamples, row.stats.converged ? "yes" : "no", compared);
			for (const auto& label : a_report.metricLabels) {
				const auto found = row.metrics.find(label);
				out += found == row.metrics.end() || std::isnan(found->second) ? std::format(" {:>14}", "-") : std::format(" {:>14.4g}", found->second);
			}
			out += '\n';
		}
		if (baseline)
			for (const auto& row : baseline->rows)
				if (!listed.contains(row.name))
					out += std::format("{:<48} {:<16} {:>9.1f} {:>7} {:>35} {:>6} {:>6} {:>4} {}\n", row.name.substr(0, 48), row.group.substr(0, 16), 0.0, "",
						"(baseline only)", "", "", "", delta(0.0, row.timeUs, nullptr, nullptr));
		if (baseline)
			out += "\n'*': the means differ by more than their confidence intervals combined\n";
		if (!a_report.warnings.empty()) {
			out += "\nwarnings:\n";
			for (const auto& warning : a_report.warnings)
				out += std::format("  - {}\n", warning);
		}
		return out;
	}

	bool EnvironmentRequested()
	{
		static const bool requested = !Env("CS_NVPERF_TEMPLATE").empty();
		return requested;
	}

	void OnGameLoaded()
	{
		g_gameLoaded.store(true, std::memory_order_release);
	}

	void OnPresentEnd(std::uint64_t a_presents)
	{
		auto& runner = Get();
		if (g_gameLoaded.exchange(false, std::memory_order_acq_rel))
			runner.gameLoadedAt = a_presents;
		HandleEnvironment(runner);
		std::optional<Request> pending;
		bool cancel = false;
		{
			std::lock_guard lock(runner.mutex);
			pending.swap(runner.pending);
			cancel = std::exchange(runner.cancel, false);
		}
		if (pending)
			StartLocked(runner, std::move(*pending), a_presents);
		if (!runner.active)
			return;
		if (cancel) {
			if (runner.captureId)
				CancelCapture();
			Finish(runner, "cancelled");
			return;
		}
		if (runner.request.t.timingSource == TimingSource::Timestamps) {
			if (WaitForGame(runner))
				AdvanceTimestamps(runner, a_presents);
			return;
		}
		if (runner.captureId) {
			const auto status = NvPerf::GetStatus();
			if (status.captureId != runner.captureId || (status.state != CaptureState::Done && status.state != CaptureState::Failed))
				return;
			const auto result = LatestResult();
			if (!result || result->id != runner.captureId)
				return;
			const auto& planned = runner.plan[runner.next];
			const auto& groupName = runner.request.t.metricGroups[planned.group].name;
			runner.captureId = 0;
			if (result->success) {
				runner.results.emplace_back(planned.group, result);
				runner.retries = 0;
				++runner.next;
			} else if (result->error.find("still finishing") != std::string::npos) {
				// The profiler is draining the capture before (it failed or was cancelled armed): the same capture again once it has.
				runner.holdUntil = a_presents + 30;
			} else if (result->error != "cancelled" && runner.retries < 2) {
				// The profiler's failures are mostly transient (a pass the GPU reset, a frame without the ranges): again.
				++runner.retries;
				runner.holdUntil = a_presents + 30;
				runner.warnings.push_back(std::format("capture {} ({}) failed and was retried: {}", runner.next + 1, groupName, result->error));
			} else {
				runner.warnings.push_back(std::format("capture {} ({}) failed: {}", runner.next + 1, groupName, result->error));
				++runner.failed;
				runner.retries = 0;
				++runner.next;
			}
		}
		if (runner.next >= runner.plan.size() && !PlanAnotherRound(runner)) {
			Finish(runner);
			return;
		}
		if (!WaitForGame(runner))
			return;
		if (!runner.captureId && a_presents >= runner.holdUntil)
			QueueNext(runner, a_presents);
		if (runner.next >= runner.plan.size() && !PlanAnotherRound(runner)) {
			Finish(runner);
			return;
		}
		SetStatus(runner, State::Running);
	}
}
