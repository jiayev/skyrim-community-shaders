#pragma once

#include <ORGModuleServices/Async/GraphScheduler.h>
#include <ORGModuleServices/Async/GraphSchedulingLayout.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF
{

	// DCLF's executor: one coordinator thread (the frame's builds), one scene thread (the scene lane: the coordinator's passes) and a
	// configurable number of preparation threads. Dispatch never waits and never runs work inline: a job goes into its domain's unbounded MPMC queue
	// (tbb::concurrent_queue, as BasicRenderer's scheduler) and a semaphore wakes a worker. Nothing is rejected for want of
	// room; only an invalid scope, class or task is. Scope waits and Shutdown are teardown/test APIs, not normal-frame
	// operations. The preparation domain has a second queue for frame-critical jobs (DispatchFrameCritical, ORG's
	// TaskPriority::FrameCritical): its workers take from it first, and so do a few frame-critical threads that take nothing else.
	// Those wait at the frame-critical priority, so a frame-critical job starts when the engine's threads saturate the machine and
	// the preparation workers (below normal) are not scheduled.
	class SceneExecutor final : public org::async::GraphScheduler
	{
	public:
		static constexpr org::async::TaskClass Coordinator{ 0, 0 };
		static constexpr org::async::TaskClass Preparation{ 0, 1 };
		static constexpr org::async::TaskClass Scene{ 0, 2 };
		// A frame-critical thread's class in ThreadStart alone (not a dispatch class: DispatchFrameCritical reaches them).
		static constexpr org::async::TaskClass FrameCriticalThread{ 0, 3 };
		static constexpr org::async::GraphSchedulingLayout Layout{ 1, 3, Coordinator, Coordinator };
		static constexpr std::size_t kDomains = 3;

		struct Statistics
		{
			std::uint64_t accepted = 0, completed = 0, cancelled = 0, rejected = 0, failed = 0;
			std::array<std::size_t, kDomains> queued{}, active{}, highWater{};
		};

		/**
		 * @brief Per ParallelFor name, cumulative since start (a reporter takes deltas). blockedNanoseconds is the time callers spent
		 * blocked on chunks a helper had claimed and not finished: the wait ParallelFor exists to keep small.
		 */
		struct ParallelForStatistics
		{
			std::string name;
			std::uint64_t calls = 0;                  // calls with work (a_count > 0)
			std::uint64_t chunks = 0, helperChunks = 0;  // every chunk, and those a helper ran
			std::uint64_t blocked = 0;                // calls whose caller had to wait for a helper's chunk
			std::uint64_t blockedNanoseconds = 0, maxBlockedNanoseconds = 0;  // max: since start, or since the last take
			std::uint64_t boosts = 0;                 // helpers raised to their caller's priority
			std::uint64_t idleHelpers = 0;            // helpers that started once every chunk was taken
		};

		// Called on each worker thread before it takes work (a name, a priority): its class, and its index within the class.
		using ThreadStart = std::function<void(org::async::TaskClass, unsigned)>;
		/**
		 * @brief The calling thread's scheduling priority, higher more urgent (Windows' THREAD_PRIORITY_* values): get reads it, set
		 * changes it. Given both, a helper running a ParallelFor caller's chunks runs at least at the caller's priority and is restored
		 * afterwards. Each worker reads its own once, after ThreadStart; nothing else may change a worker's priority.
		 */
		struct ThreadPriority
		{
			std::function<int()> get;
			std::function<void(int)> set;
			// A frame-critical job (DispatchFrameCritical) runs at least at this one, restored after (with get and set).
			int frameCritical = 0;
		};

		explicit SceneExecutor(unsigned preparationWorkers = 2, ThreadStart onThreadStart = {}, ThreadPriority priority = {}, unsigned frameCriticalWorkers = 0);
		~SceneExecutor() override;
		SceneExecutor(const SceneExecutor&) = delete;
		SceneExecutor& operator=(const SceneExecutor&) = delete;
		org::async::Scope CreateScope(std::string_view name) override;
		bool Dispatch(const org::async::Scope&, org::async::TaskClass, org::async::TaskDispatch,
			std::string_view, Task, org::async::TaskTraceMetadata = {}) override;
		bool DispatchAfter(const org::async::Scope&, std::chrono::steady_clock::duration,
			org::async::TaskClass, std::string_view, Task) override;
		/**
		 * @brief A preparation job the frame waits on (the CPU pipeline's culls, whose batches DXVK's CS thread records): queued ahead of
		 * every other preparation job, and run at least at ThreadPriority::frameCritical, so neither the queue nor the engine's job
		 * threads hold it back. A ParallelFor it calls has frame-critical helpers. Never runs inline; false for an invalid scope or task.
		 */
		bool DispatchFrameCritical(const org::async::Scope& a_scope, std::string_view a_name, Task a_task);
		/**
		 * @brief Runs a_body(begin, end) over [0, a_count) in chunks of a_grain, on the calling thread and on the preparation
		 * workers, and returns once every chunk ran (BasicRenderer's TaskSchedulerManager::ParallelFor). The caller takes chunks
		 * itself and waits only for chunks a worker is running, never for queued work: a helper that starts once the chunks are
		 * gone does nothing, and a helper that never starts leaves its chunks to the others. The first exception a chunk throws is
		 * rethrown here once every claimed chunk finished. For worker threads: it waits, so never on the render thread.
		 * Chunks are exactly [k * grain, min(count, (k + 1) * grain)) (callers index per-chunk results by a_begin / grain), so what
		 * the caller can wait for is one grain per helper; a helper holding one runs at least at the caller's priority (ThreadPriority),
		 * so the engine's normal-priority threads do not preempt it while an above-normal caller waits.
		 */
		void ParallelFor(std::string_view a_name, std::size_t a_count, std::size_t a_grain, const std::function<void(std::size_t, std::size_t)>& a_body);
		Statistics GetStatistics() const;
		/** @brief Every ParallelFor name seen so far, in no particular order; a_takePeaks resets each maxBlockedNanoseconds once read. */
		std::vector<ParallelForStatistics> GetParallelForStatistics(bool a_takePeaks = false) const;
		// Owner must outlive its callbacks. Calling Shutdown/Wait on its workers is an error.
		void Shutdown();

	private:
		struct State;
		struct ExecutorScope;
		struct Threads;
		std::shared_ptr<State> state;
		std::unique_ptr<Threads> threads;
		org::async::Scope parallelScope;  // ParallelFor's helpers
	};
}
