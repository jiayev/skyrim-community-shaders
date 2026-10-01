#pragma once

#include <ORGModuleServices/Async/GraphScheduler.h>
#include <ORGModuleServices/Async/GraphSchedulingLayout.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace DCLF
{

	// Dedicated to scene publication: one coordinator and a configurable number of preparation threads.
	// Each domain's queue (including its timers) is bounded independently. Dispatch is lock-free: a job goes into its
	// domain's bounded MPMC ring and a semaphore wakes a worker; it never executes work inline or waits for capacity
	// (a full ring rejects). Scope waits and Shutdown are teardown/test APIs, not normal-frame operations.
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

		explicit PublishedSceneExecutor(std::size_t coordinatorCapacity = 256, std::size_t preparationCapacity = 256, unsigned preparationWorkers = 2);
		~PublishedSceneExecutor() override;
		PublishedSceneExecutor(const PublishedSceneExecutor&) = delete;
		PublishedSceneExecutor& operator=(const PublishedSceneExecutor&) = delete;
		org::async::Scope CreateScope(std::string_view name) override;
		bool Dispatch(const org::async::Scope&, org::async::TaskClass, org::async::TaskDispatch,
			std::string_view, Task, org::async::TaskTraceMetadata = {}) override;
		bool DispatchAfter(const org::async::Scope&, std::chrono::steady_clock::duration,
			org::async::TaskClass, std::string_view, Task) override;
		Statistics GetStatistics() const;
		// Owner must outlive its callbacks. Calling Shutdown/Wait on its workers is an error.
		void Shutdown();

	private:
		struct State;
		struct ExecutorScope;
		struct Threads;
		std::shared_ptr<State> state;
		std::unique_ptr<Threads> threads;
	};
}
