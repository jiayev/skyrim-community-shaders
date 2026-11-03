// A test run's phases as BasicTelemetry sessions. CS links BasicTelemetry for the graph's scopes, but nothing else starts a
// session, so those scopes record nothing without this. Each phase writes three files to <log directory>/dclf-telemetry:
// <label>.frames.csv, one row per Present; <label>.scopes.csv, each scope's totals; <label>.stalls.csv, scopes off the CPU for 2 ms
// or more. The log gets the phase's frame time summary from BasicTelemetry's sampler.

#include "TelemetryProfile.h"

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <BasicTelemetry/Telemetry.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace DCLF::Diagnostics
{
	namespace
	{
		std::unique_ptr<basic_telemetry::Session> session;
		std::optional<basic_telemetry::Frame> frame;
		std::string phase;

		std::uint64_t Percentile(std::vector<std::uint64_t> a_samples, double a_p)
		{
			if (a_samples.empty())
				return 0;
			const auto at = a_samples.begin() + static_cast<std::ptrdiff_t>(a_p * static_cast<double>(a_samples.size() - 1));
			std::nth_element(a_samples.begin(), at, a_samples.end());
			return *at;
		}

		void WriteFrames(const basic_telemetry::SessionSnapshot& a_snapshot, const std::filesystem::path& a_dir)
		{
			std::ofstream out(a_dir / (phase + ".frames.csv"));
			out << "frame,start_ns,duration_ns,loading\n";
			auto frames = a_snapshot.frames;
			std::ranges::sort(frames, {}, &basic_telemetry::FrameSnapshot::id);
			basic_telemetry::SamplingConfig config;
			config.maximumSamples = std::max<std::uint32_t>(config.minimumSamples, static_cast<std::uint32_t>(frames.size()));
			config.absoluteHalfWidth = 0.1;
			basic_telemetry::StatisticalSampler sampler(config);
			double longest = 0.0;
			for (const auto& entry : frames) {
				const auto loading = entry.dimensions.find("loading");
				const bool isLoading = loading != entry.dimensions.end() && loading->second != 0;
				out << entry.id << ',' << entry.startNs << ',' << entry.durationNs << ',' << (isLoading ? 1 : 0) << '\n';
				if (isLoading)
					continue;
				const double ms = static_cast<double>(entry.durationNs) / 1e6;
				sampler.AddSample(ms);
				longest = std::max(longest, ms);
			}
			const auto summary = sampler.Summarize();
			logger::info("[DCLF] telemetry {}: {} frames, mean {:.3f} ms [{:.3f}, {:.3f}], median {:.3f}, p05 {:.3f}, p95 {:.3f}, longest {:.3f}, cv {:.3f}, lag-1 {:.3f}, effective {:.0f}, outliers {}, converged {}",
				phase, summary.rawSampleCount, summary.mean, summary.confidenceLow, summary.confidenceHigh, summary.median, summary.p05, summary.p95, longest,
				summary.coefficientOfVariation, summary.lag1Autocorrelation, summary.effectiveSampleCount, summary.madOutlierCount, summary.converged);
		}

		void WriteScopes(const basic_telemetry::SessionSnapshot& a_snapshot, const std::filesystem::path& a_dir)
		{
			std::unordered_map<basic_telemetry::StableId, std::string> names;
			for (const auto& definition : a_snapshot.scopeDefinitions)
				names[definition.id] = definition.name;
			std::ofstream scopes(a_dir / (phase + ".scopes.csv"));
			scopes << "scope,count,inclusive_total_ns,inclusive_p50_ns,inclusive_p99_ns,inclusive_max_ns,self_total_ns,cpu_total_ns,off_cpu_total_ns\n";
			for (const auto& scope : a_snapshot.scopes)
				scopes << '"' << names[scope.scopeId] << "\"," << scope.inclusiveNs.count << ',' << scope.inclusiveNs.total << ','
					   << Percentile(scope.inclusiveNs.retainedSamples, 0.5) << ',' << Percentile(scope.inclusiveNs.retainedSamples, 0.99) << ','
					   << scope.inclusiveNs.maximum << ',' << scope.selfNs.total << ',' << scope.threadCpuNs.total << ',' << scope.nonRunningNs.total << '\n';
			std::ofstream stalls(a_dir / (phase + ".stalls.csv"));
			stalls << "frame,thread,scope,start_ns,inclusive_ns,cpu_ns,off_cpu_ns\n";
			for (const auto& event : a_snapshot.events)
				stalls << event.frameId << ',' << event.threadId << ",\"" << names[event.scopeId] << "\"," << event.startNs << ','
					   << event.inclusiveNs << ',' << event.threadCpuNs << ',' << event.nonRunningNs << '\n';
			if (a_snapshot.droppedEvents || a_snapshot.droppedContendedScopeSamples)
				logger::warn("[DCLF] telemetry {}: dropped {} stall events and {} contended scope samples", phase, a_snapshot.droppedEvents, a_snapshot.droppedContendedScopeSamples);
		}

		void EndPhase()
		{
			if (!session)
				return;
			frame.reset();
			const auto snapshot = session->Snapshot();
			session.reset();
			const auto dir = logger::log_directory().value_or(std::filesystem::path{}) / "dclf-telemetry";
			std::error_code error;
			std::filesystem::create_directories(dir, error);
			WriteFrames(snapshot, dir);
			WriteScopes(snapshot, dir);
		}
	}

	void TelemetryPhase(std::string_view a_label)
	{
		EndPhase();
		if (a_label == "off")
			return;
		phase = a_label;
		basic_telemetry::SessionConfig config;
		config.mode = basic_telemetry::CaptureMode::Summary;
		config.measureThreadCpuTime = true;
		config.retainStallEvents = true;
		config.stallEventThresholdNs = 2'000'000;
		session = std::make_unique<basic_telemetry::Session>(std::move(config));
	}

	void TelemetryFrame()
	{
		if (!session)
			return;
		frame.reset();
		frame.emplace("CSPresent");
		frame->SetDimension("loading", Scene::FrameView::IsLoadingScreenUp() ? 1 : 0);
	}
}
