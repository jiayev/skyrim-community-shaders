#pragma once

#include <cstdint>
#include <vector>

/**
 * @brief The statistics of NvPerf's traces (NvPerfTrace.h): a series of per-frame values summarized as BasicRenderer's
 * statistical sampler does (BasicTelemetry's StatisticalSampler): the mean with a Student-t confidence interval whose
 * standard error comes from the Newey-West long-run variance, so autocorrelated frames count as the effective sample count
 * they are worth; median, p05/p95, and the samples a median-absolute-deviation test flags (counted, never dropped).
 *
 * A series converges once it has minSamples samples, an effective sample count of at least minSamples, and a confidence
 * interval half-width within relativeHalfWidth of its mean or within absoluteHalfWidth (a range of a few microseconds
 * never reaches a relative target).
 *
 * Built with the render graph (CS_HAS_RENDER_GRAPH), which brings BasicTelemetry; without it Available() is false, a
 * summary carries the mean, median and percentiles only, and nothing converges.
 */
namespace NvPerf::Trace::Statistics
{
	struct Config
	{
		double confidence = 0.95;
		double relativeHalfWidth = 0.02;
		double absoluteHalfWidth = 1.0;  // in the series' unit
		std::uint32_t minSamples = 30;
		std::uint32_t maxSamples = 5000;
	};

	struct Summary
	{
		std::uint32_t samples = 0;
		double effectiveSamples = 0.0;
		double mean = 0.0, median = 0.0, p05 = 0.0, p95 = 0.0;
		double halfWidth = 0.0;          // of the mean's confidence interval
		double relativeHalfWidth = 0.0;  // halfWidth / |mean|
		double lag1Autocorrelation = 0.0;
		std::uint32_t madOutliers = 0;
		bool converged = false;
	};

	/// Whether confidence intervals are computed (BasicTelemetry is built in).
	bool Available();
	/// The series' summary; a_config's sample counts must satisfy 2 <= minSamples <= maxSamples (ClampConfig).
	Summary Summarize(const std::vector<double>& a_samples, const Config& a_config);
	/// a_config with its bounds made valid (confidence in (0, 1), 2 <= minSamples <= maxSamples, half-widths >= 0).
	Config ClampConfig(Config a_config);
	/// Whether two means differ by more than their confidence intervals combined (sqrt(a^2 + b^2) of the half-widths).
	bool Significant(double a_mean, double a_halfWidth, double a_baseMean, double a_baseHalfWidth);
}
