#pragma once

#include <ORGModuleServices/Async/GraphScheduler.h>
#include <ORGModuleServices/Async/GraphSchedulingLayout.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace DCLF
{

	// Dedicated to scene publication: one coordinator and a configurable number of preparation threads.
	// Dispatch never waits and never runs work inline: a job goes into its domain's unbounded MPMC queue
	// (tbb::concurrent_queue, as BasicRenderer's scheduler) and a semaphore wakes a worker. Nothing is rejected for want of
	// room; only an invalid scope, class or task is. Scope waits and Shutdown are teardown/test APIs, not normal-frame
	// operations.
	class PublishedSceneExecutor final : public org::async::GraphScheduler
	{
	public:
		static constexpr org::async::TaskClass Coordinator{ 0, 0 };
		static constexpr org::async::TaskClass Preparation{ 0, 1 };
		static constexpr org::async::GraphSchedulingLayout Layout{ 1, 2, Coordinator, Coordinator };

		struct Statistics
		{
			std::uint64_t accepted = 0, completed = 0, cancelled = 0, rejected = 0, failed = 0;
			std::array<std::size_t, 2> queued{}, active{}, highWater{};
		};

		// Called on each worker thread before it takes work (a name, a priority): its class, and its index within the class.
		using ThreadStart = std::function<void(org::async::TaskClass, unsigned)>;

		explicit PublishedSceneExecutor(unsigned preparationWorkers = 2, ThreadStart onThreadStart = {});
		~PublishedSceneExecutor() override;
		PublishedSceneExecutor(const PublishedSceneExecutor&) = delete;
		PublishedSceneExecutor& operator=(const PublishedSceneExecutor&) = delete;
		org::async::Scope CreateScope(std::string_view name) override;
		bool Dispatch(const org::async::Scope&, org::async::TaskClass, org::async::TaskDispatch,
			std::string_view, Task, org::async::TaskTraceMetadata = {}) override;
		bool DispatchAfter(const org::async::Scope&, std::chrono::steady_clock::duration,
			org::async::TaskClass, std::string_view, Task) override;
		/**
		 * @brief Runs a_body(begin, end) over [0, a_count) in chunks of a_grain, on the calling thread and on the preparation
		 * workers, and returns once every chunk ran (BasicRenderer's TaskSchedulerManager::ParallelFor). The caller takes chunks
		 * itself and waits only for chunks a worker is running, never for queued work: a helper that starts once the chunks are
		 * gone does nothing, and a helper that never starts leaves its chunks to the others. The first exception a chunk throws is
		 * rethrown here once every claimed chunk finished. For worker threads: it waits, so never on the render thread.
		 */
		void ParallelFor(std::string_view a_name, std::size_t a_count, std::size_t a_grain, const std::function<void(std::size_t, std::size_t)>& a_body);
		Statistics GetStatistics() const;
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
