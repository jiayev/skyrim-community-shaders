#include "BuildExecutor.h"

#include "Features/DrawcallLimitFix/Common/SceneScheduler.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

#include <ORGModuleServices/Async/SerializedTaskPump.h>
#include <tbb/concurrent_queue.h>

namespace DCLF
{
	namespace
	{
		/**
		 * @brief A width-limited pump onto the preparation pool. Lock-free: producers push and count, and take a runner slot
		 * while fewer than the width run. A runner gives its slot back only once the count reads zero, and then looks once more,
		 * so a push that found every slot taken is never left without a runner.
		 */
		struct BuildPump
		{
			using Task = std::function<void()>;

			tbb::concurrent_queue<Task> tasks;
			// Pushed and not yet popped. A pop may precede its push's count, so it can read below zero for a moment.
			std::atomic<std::int64_t> queued{ 0 };
			std::atomic<unsigned> runners{ 0 };
			const unsigned width = BuildWidth();

			bool Submit(Task a_task)
			{
				tasks.push(std::move(a_task));
				queued.fetch_add(1);
				if (TakeRunner())
					Dispatch();
				return true;
			}

			bool TakeRunner()
			{
				for (unsigned running = runners.load(); running < width;)
					if (runners.compare_exchange_weak(running, running + 1))
						return true;
				return false;
			}

			// Holds a runner slot.
			void Dispatch()
			{
				// The scope is never cancelled and the executor never shut down, so the pool takes it.
				SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), PublishedSceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "DCLF builds",
					[this](const org::async::TaskContext&) { Run(); });
			}

			void Run()
			{
				if (Task task; tasks.try_pop(task)) {
					queued.fetch_sub(1);
					// ORG's tasks report their own failures through their artifacts; nothing may stop the pump.
					try {
						task();
					} catch (...) {
					}
				}
				if (queued.load() > 0) {
					Dispatch();  // keeps the slot, behind whatever the pool queued meanwhile
					return;
				}
				runners.fetch_sub(1);
				// A push that counted after the read above and found every slot taken.
				if (queued.load() > 0 && TakeRunner())
					Dispatch();
			}
		};

		BuildPump& Pump()
		{
			// Never destroyed, as the executor it feeds: builds may still complete while the process tears down.
			static auto* pump = new BuildPump;
			return *pump;
		}
	}

	unsigned BuildWidth()
	{
		return (std::max)(1u, SceneScheduler::PreparationWorkers() / 2);
	}

	std::function<bool(std::function<void()>)> BuildSubmitter()
	{
		return [](std::function<void()> a_task) { return Pump().Submit(std::move(a_task)); };
	}

	namespace
	{
		std::atomic<void (*)()> laneDrain{ nullptr };

		org::async::SerializedTaskPump& Lane()
		{
			// Configured once, before any wake can reach it (a function-local static), and never destroyed: completions may still
			// arrive while the process tears down.
			static auto* lane = [] {
				auto* pump = new org::async::SerializedTaskPump;
				pump->Configure(
					[](org::async::SerializedTaskPump::Task a_task) {
						return SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), PublishedSceneExecutor::Coordinator, org::async::TaskDispatch::Cpu,
							"DCLF pipeline lane", [task = std::move(a_task)](const org::async::TaskContext&) { task(); });
					},
					[] {
						if (auto* drain = laneDrain.load(std::memory_order_acquire))
							drain();
					},
					[] { logger::error("[DCLF] the pipeline lane was refused by DCLF's executor; no Lighting pipeline is admitted from here on"); });
				return pump;
			}();
			return *lane;
		}
	}

	void SetPipelineLaneDrain(void (*a_drain)())
	{
		laneDrain.store(a_drain, std::memory_order_release);
		WakePipelineLane();
	}

	void WakePipelineLane()
	{
		(void)Lane().Notify();
	}
}
