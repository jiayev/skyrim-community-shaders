#include "Features/RemoteControl/TraceStatistics.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include <BasicTelemetry/Sampling.h>
#endif

#include <algorithm>
#include <cmath>
#include <numeric>

namespace NvPerf::Trace::Statistics
{
	bool Available()
	{
#if defined(CS_HAS_RENDER_GRAPH)
		return true;
#else
		return false;
#endif
	}

	Config ClampConfig(Config a_config)
	{
		if (!(a_config.confidence > 0.0 && a_config.confidence < 1.0))
			a_config.confidence = 0.95;
		a_config.relativeHalfWidth = std::max(0.0, a_config.relativeHalfWidth);
		a_config.absoluteHalfWidth = std::max(0.0, a_config.absoluteHalfWidth);
		a_config.minSamples = std::max<std::uint32_t>(2, a_config.minSamples);
		a_config.maxSamples = std::max(a_config.minSamples, a_config.maxSamples);
		return a_config;
	}

	Summary Summarize(const std::vector<double>& a_samples, const Config& a_config)
	{
		Summary out;
		out.samples = static_cast<std::uint32_t>(a_samples.size());
		if (a_samples.empty())
			return out;
#if defined(CS_HAS_RENDER_GRAPH)
		const auto config = ClampConfig(a_config);
		basic_telemetry::SamplingConfig sampling;
		sampling.minimumSamples = config.minSamples;
		// The series is the caller's to bound: every sample counts (the sampler would keep only the first maximumSamples).
		sampling.maximumSamples = std::max<std::uint32_t>(config.minSamples, static_cast<std::uint32_t>(a_samples.size()));
		sampling.confidenceLevel = config.confidence;
		sampling.relativeHalfWidth = config.relativeHalfWidth;
		sampling.absoluteHalfWidth = config.absoluteHalfWidth;
		basic_telemetry::StatisticalSampler sampler(sampling);
		for (double value : a_samples)
			if (std::isfinite(value))
				sampler.AddSample(value);
		const auto summary = sampler.Summarize();
		out.samples = summary.rawSampleCount;
		out.effectiveSamples = summary.effectiveSampleCount;
		out.mean = summary.mean;
		out.median = summary.median;
		out.p05 = summary.p05;
		out.p95 = summary.p95;
		out.halfWidth = 0.5 * (summary.confidenceHigh - summary.confidenceLow);
		out.relativeHalfWidth = summary.relativeHalfWidth;
		out.lag1Autocorrelation = summary.lag1Autocorrelation;
		out.madOutliers = summary.madOutlierCount;
		out.converged = summary.converged;
#else
		(void)a_config;
		auto sorted = a_samples;
		std::ranges::sort(sorted);
		const auto percentile = [&](double a_p) {
			const double index = a_p * double(sorted.size() - 1);
			const auto lo = static_cast<std::size_t>(std::floor(index)), hi = static_cast<std::size_t>(std::ceil(index));
			return sorted[lo] + (sorted[hi] - sorted[lo]) * (index - double(lo));
		};
		out.effectiveSamples = double(sorted.size());
		out.mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) / double(sorted.size());
		out.median = percentile(0.5);
		out.p05 = percentile(0.05);
		out.p95 = percentile(0.95);
		out.halfWidth = out.relativeHalfWidth = std::nan("");
#endif
		return out;
	}

	bool Significant(double a_mean, double a_halfWidth, double a_baseMean, double a_baseHalfWidth)
	{
		if (!std::isfinite(a_halfWidth) || !std::isfinite(a_baseHalfWidth))
			return false;
		return std::abs(a_mean - a_baseMean) > std::sqrt(a_halfWidth * a_halfWidth + a_baseHalfWidth * a_baseHalfWidth);
	}
}
