#pragma once

#include <array>
#include <atomic>
#include <d3d11.h>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <winrt/base.h>

/**
 * @brief GPU and CPU profiler using D3D11 timestamp queries.
 *
 * Maintains a ring buffer of frames with paired begin/end timestamp queries
 * and rolling statistics (average, p95, p99) per named pass.
 */
class Profiler
{
public:
	static constexpr uint32_t kMaxTimers = 128;
	// Frames whose queries can be in flight at once. The render thread may run several frames ahead of the GPU
	// (nothing in the frame waits for it), and a frame's queries are read only once all of them are done.
	static constexpr uint32_t kFrameRing = 8;
	static constexpr uint32_t kHistorySize = 300;
	// Must exceed the longest legitimate gap between samples of a still-running pass;
	// the DynamicCubemaps state machine spreads its passes over 6 frames.
	static constexpr uint64_t kTimerRetireFrames = 60;

	using PerfEventCallback = std::function<void(std::string_view)>;

	/** @brief Circular buffer tracking per-timer timing samples with statistics. */
	struct RollingHistory
	{
		float history[kHistorySize]{};
		uint32_t head = 0;
		uint32_t count = 0;
		float lastMs = 0.0f;

		/** @brief Appends a timing sample, overwriting the oldest if full. */
		void PushSample(float ms)
		{
			history[head] = ms;
			head = (head + 1) % kHistorySize;
			if (count < kHistorySize)
				count++;
			lastMs = ms;
			sortedDirty = true;
		}

		/** @brief Gets the arithmetic mean of all buffered samples. */
		float GetAverage() const;

		/**
		 * @brief Gets an interpolated percentile from the buffered samples.
		 * @param p Percentile in [0, 100].
		 */
		float GetPercentile(float p) const;

	private:
		// Reuse one sorted snapshot for every percentile until the next sample arrives.
		mutable std::array<float, kHistorySize> sorted{};
		mutable bool sortedDirty = true;
	};

	/** @brief Snapshot of GPU/CPU timing data for a single named pass. */
	struct TimerResult
	{
		std::string name;
		float gpuTimeMs = 0.0f;
		// Rolling statistics, filled in only by GetStatistics.
		float avgMs = 0.0f;
		float p95Ms = 0.0f;
		float p99Ms = 0.0f;
		float cpuTimeMs = 0.0f;
		float cpuAvgMs = 0.0f;
		float cpuP95Ms = 0.0f;
		float cpuP99Ms = 0.0f;
		bool valid = false;
		// Share of recent samples whose begin and end timestamps landed in different DXVK submissions.
		// Such a span also counts any time the GPU sat idle waiting for the later submission, so it
		// overstates the pass; see Profiler.cpp.
		float splitFraction = 0.0f;

		const float* historyBuffer = nullptr;
		uint32_t historyHead = 0;
		uint32_t historyCount = 0;

		/**
		 * @brief Gets a history sample by age-ordered index (0 = oldest).
		 * @param index Zero-based index into the history ring buffer.
		 */
		float GetHistorySample(uint32_t index) const
		{
			if (!historyBuffer || index >= historyCount)
				return 0.0f;
			return historyBuffer[(historyHead - historyCount + index + kHistorySize) % kHistorySize];
		}
	};

	/** @brief Rolling wall-clock statistics of the interval between EndFrame calls. */
	struct FrameStats
	{
		float lastMs = 0.0f;
		float avgMs = 0.0f;
		float p95Ms = 0.0f;
		float p99Ms = 0.0f;
		uint32_t count = 0;
	};

	/**
	 * @brief Creates timestamp query objects and prepares the frame ring buffer.
	 * @param device D3D11 device used to create query objects.
	 * @param context Device context used for issuing and collecting queries.
	 */
	void Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

	/** @brief Releases all D3D11 query objects and resets state. */
	void Release();

	/**
	 * @brief Registers optional callbacks invoked at pass begin/end (e.g. for RenderDoc markers).
	 * @param beginCb Called with the pass name when a pass begins.
	 * @param endCb Called when a pass ends.
	 */
	void SetPerfEventCallbacks(PerfEventCallback beginCb, PerfEventCallback endCb)
	{
		beginPerfEvent = std::move(beginCb);
		endPerfEvent = std::move(endCb);
	}

	/** @brief Begins a new profiling frame; collects results from the oldest in-flight frame. */
	void BeginFrame();

	/**
	 * @brief Begins a named GPU/CPU timing pass within the current frame.
	 * @param name Pass identifier (e.g. "Feature::PassName"). Implicitly calls BeginFrame if needed.
	 */
	void BeginPass(std::string_view name);

	/** @brief Ends the current timing pass and records the CPU/GPU timestamps. */
	void EndPass();

	/** @brief Ends the current profiling frame and advances the ring buffer write cursor. */
	void EndFrame();

	/**
	 * @brief Keeps GPU/CPU pass timing running for the next few frames; call it each frame a consumer reads the results.
	 * @param frames Frames to stay active for. Timing stops once no consumer has asked within that window.
	 * A shorter request never cuts a longer one short, so the overlay cannot end a DevBench window.
	 */
	void KeepActive(uint32_t frames = 2)
	{
		const uint64_t until = frameCounter.load(std::memory_order_relaxed) + frames;
		uint64_t current = activeUntilFrame.load(std::memory_order_relaxed);
		while (current < until && !activeUntilFrame.compare_exchange_weak(current, until, std::memory_order_relaxed)) {
		}
	}
	/** @brief True while a consumer asked for timings within its KeepActive window. */
	bool IsActive() const { return frameCounter.load(std::memory_order_relaxed) <= activeUntilFrame.load(std::memory_order_relaxed); }

	/**
	 * @brief Adds GPU time measured outside the D3D11 queries: the render graph's pass timestamps, and the
	 * Streamline interop buffers' own timestamps. Work submitted outside DXVK's command lists must not also be
	 * bracketed by a D3D11 timer, whose span would straddle the submission (see TimerResult::splitFraction).
	 *
	 * Samples accumulate until the next collected frame and are averaged over the frames that passed since the
	 * last one, so a source that reports late or in bursts still yields a per-frame figure.
	 */
	void AddExternalSample(std::string_view a_name, float a_gpuMs);

	/** @brief Gets the live timings and history from the last collected frame. Rolling statistics are left at zero; see GetStatistics. */
	const std::vector<TimerResult>& GetResults() const { return results; }

	/**
	 * @brief Computes the rolling CPU and GPU statistics of every tracked pass.
	 * Re-sorts every history that gained samples since the last call, so callers should throttle it.
	 * Not synchronized: call it only on the thread that calls EndFrame, like GetResults.
	 * @param timerPrefix Includes only passes starting with this prefix, or all passes when empty.
	 */
	std::vector<TimerResult> GetStatistics(std::string_view timerPrefix = {}) const;

	/**
	 * @brief Gets the present-cadence frame interval statistics, tracked even while GPU timing is off.
	 * Re-sorts the history if it gained a sample, so callers should throttle it.
	 * Not synchronized: call it only on the thread that calls EndFrame, like GetResults.
	 */
	FrameStats GetFrameStats() const;

	/** @brief Gets the total GPU time in milliseconds for the last collected frame. */
	float GetTotalTimeMs() const { return totalTimeMs; }

	/** @brief Gets the total CPU time in milliseconds for the last collected frame. */
	float GetCpuTotalTimeMs() const { return cpuTotalTimeMs; }

	/** @brief Resets all timer history and results. */
	void ClearTimers()
	{
		results.clear();
		knownTimers.clear();
		knownTimerIndex.clear();
		collectedFrames = 0;
		lastLoggedFrame = 0;
		totalTimeMs = 0.0f;
		cpuTotalTimeMs = 0.0f;
	}

	/**
	 * @brief Removes all timers whose names start with the given feature prefix.
	 * @param featureName Feature name; timers matching "featureName::*" are removed.
	 */
	void ClearTimersForFeature(const std::string& featureName)
	{
		std::string prefix = featureName + "::";
		std::erase_if(knownTimers, [&prefix](const KnownTimer& kt) {
			return kt.name.starts_with(prefix);
		});
		RebuildTimerIndex();
	}

private:
	struct FrameQueries
	{
		winrt::com_ptr<ID3D11Query> disjoint;
		struct TimerPair
		{
			winrt::com_ptr<ID3D11Query> begin;
			winrt::com_ptr<ID3D11Query> end;
			std::string name;
			LARGE_INTEGER cpuBegin{};
			float cpuMs = 0.0f;
			uint64_t submissionAtBegin = 0;
			bool split = false;
		};
		std::vector<TimerPair> timers;
		uint32_t activeCount = 0;
		bool inFlight = false;
	};

	ID3D11DeviceContext* context = nullptr;

	FrameQueries frames[kFrameRing];
	uint32_t writeFrame = 0;
	uint32_t readFrame = 0;  // the oldest frame not yet collected
	bool initialized = false;
	bool frameActive = false;
	bool passOpen = false;
	// The current frame is not timed: every slot of the ring still holds a frame the GPU has not finished.
	// Its passes still emit their perf events.
	bool frameSkipped = false;
	static double QpcTicksToMs();

	uint64_t lastLoggedFrame = 0;
	double cpuTicksToMs = QpcTicksToMs();
	std::atomic<uint64_t> frameCounter{ 0 };
	std::atomic<uint64_t> activeUntilFrame{ 0 };
	RollingHistory frameHistory;
	int64_t lastFrameTicks = 0;

	PerfEventCallback beginPerfEvent;
	PerfEventCallback endPerfEvent;

	std::vector<TimerResult> results;

	struct KnownTimer
	{
		std::string name;
		RollingHistory gpu;
		RollingHistory cpu;
		RollingHistory split;
		uint64_t lastSampleFrame = 0;
	};
	std::unordered_map<std::string, float> pendingExternal;
	uint32_t framesSinceExternalMerge = 0;
	std::vector<KnownTimer> knownTimers;
	std::unordered_map<std::string, size_t> knownTimerIndex;
	uint64_t collectedFrames = 0;
	// DXVK's submission counter (dxvkGetSubmissionCounter), or null on native D3D11 or an older DXVK.
	const volatile uint64_t* submissionCounter = nullptr;
	float totalTimeMs = 0.0f;
	float cpuTotalTimeMs = 0.0f;

	/** @brief Collects every frame the GPU has finished, oldest first, then rebuilds the results. */
	void CollectResults();
	/** @brief Reads one finished frame's queries into the timers' histories; false while the GPU is not done with it. */
	bool CollectFrame(FrameQueries& frame, std::unordered_map<std::string, std::pair<float, float>>& activeTimers);

	/** @brief Drops timers that have not been sampled for kTimerRetireFrames, so disabled passes stop reporting stale values. */
	void RetireStaleTimers();
	void LogResultsIfRequested();

	/** @brief Repoints knownTimerIndex at the current knownTimers positions after an erase. */
	void RebuildTimerIndex();
};
