#include "PublishedSceneExecutor.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace DCLF
{
	namespace
	{
		thread_local const void* currentExecutor = nullptr;
	}

	struct PublishedSceneExecutor::State
	{
		struct Job
		{
			std::chrono::steady_clock::time_point ready;
			std::shared_ptr<ExecutorScope> scope;
			Task task;
		};
		mutable std::mutex mutex;
		std::condition_variable changed;
		std::atomic_bool stopping{ false };
		std::array<std::vector<Job>, 2> queues;
		std::array<std::size_t, 2> capacities;
		Statistics stats;
		State(std::size_t coordinator, std::size_t preparation) : capacities{ coordinator, preparation }
		{
			if (!coordinator || !preparation) throw std::invalid_argument("scene executor capacity must be positive");
			queues[0].reserve(coordinator);
			queues[1].reserve(preparation);
		}
		void Run(unsigned domain);
	};

	struct PublishedSceneExecutor::ExecutorScope final : org::async::TaskScope
	{
		std::shared_ptr<State> owner;
		std::atomic_bool cancelled{ false };
		std::size_t pending = 0;  // owner mutex, includes executing work and timers
		std::exception_ptr failure;
		explicit ExecutorScope(std::shared_ptr<State> value) : owner(std::move(value)) {}
		bool StopRequested() const noexcept override { return cancelled.load() || owner->stopping.load(); }
		void Cancel() noexcept override
		{
			{
				std::lock_guard lock(owner->mutex);
				cancelled.store(true);
			}
			owner->changed.notify_all();
		}
		void Wait() const override
		{
			if (currentExecutor == owner.get()) throw std::logic_error("scene workers cannot wait on scene scopes");
			std::unique_lock lock(owner->mutex);
			owner->changed.wait(lock, [&] { return pending == 0; });
			const auto error = failure;
			lock.unlock();
			if (error) std::rethrow_exception(error);
		}
	};

	struct PublishedSceneExecutor::Threads
	{
		std::mutex shutdownMutex;
		std::vector<std::thread> workers;
	};

	void PublishedSceneExecutor::State::Run(unsigned domain)
	{
		currentExecutor = this;
		for (;;) {
			Job job;
			{
				std::unique_lock lock(mutex);
				auto& queue = queues[domain];
				for (;;) {
					if (queue.empty()) {
						if (stopping.load()) { currentExecutor = nullptr; return; }
						changed.wait(lock);
						continue;
					}
					// Cancelled timers are reclaimed immediately on their worker lane.
					auto next = std::find_if(queue.begin(), queue.end(), [](const Job& value) { return value.scope->StopRequested(); });
					if (next == queue.end()) {
						next = std::min_element(queue.begin(), queue.end(), [](const Job& a, const Job& b) { return a.ready < b.ready; });
						if (next->ready > std::chrono::steady_clock::now()) {
							const auto deadline = next->ready;
							changed.wait_until(lock, deadline);
							continue;
						}
					}
					job = std::move(*next);
					queue.erase(next);
					stats.queued[domain] = queue.size();
					++stats.active[domain];
					break;
				}
			}
			auto scope = std::move(job.scope);
			std::exception_ptr error;
			const bool cancelled = scope->StopRequested();
			if (!cancelled) {
				try { job.task({ [scope] { return scope->StopRequested(); } }); }
				catch (...) { error = std::current_exception(); }
			}
			// Release captured resources before signalling completion, outside locks,
			// including when the task never started. Cancel never destroys queued work.
			job.task = {};
			{
				std::lock_guard lock(mutex);
				--stats.active[domain];
				--scope->pending;
				if (error) { ++stats.failed; if (!scope->failure) scope->failure = error; }
				else if (cancelled) ++stats.cancelled;
				else ++stats.completed;
			}
			changed.notify_all();
		}
	}

	PublishedSceneExecutor::PublishedSceneExecutor(std::size_t coordinatorCapacity, std::size_t preparationCapacity) :
		state(std::make_shared<State>(coordinatorCapacity, preparationCapacity)), threads(std::make_unique<Threads>())
	{
		threads->workers.reserve(3);
		try {
			for (unsigned domain : { 0u, 1u, 1u })
				threads->workers.emplace_back([shared = state, domain] { shared->Run(domain); });
		} catch (...) {
			Shutdown();
			throw;
		}
	}

	PublishedSceneExecutor::~PublishedSceneExecutor() { Shutdown(); }

	org::async::Scope PublishedSceneExecutor::CreateScope(std::string_view)
	{
		return std::make_shared<ExecutorScope>(state);
	}

	bool PublishedSceneExecutor::Dispatch(const org::async::Scope& scope, org::async::TaskClass cls,
		org::async::TaskDispatch, std::string_view name, Task task, org::async::TaskTraceMetadata)
	{
		return DispatchAfter(scope, {}, cls, name, std::move(task));
	}

	bool PublishedSceneExecutor::DispatchAfter(const org::async::Scope& scope, std::chrono::steady_clock::duration delay,
		org::async::TaskClass cls, std::string_view, Task task)
	{
		const auto own = std::dynamic_pointer_cast<ExecutorScope>(scope);
		const auto now = std::chrono::steady_clock::now();
		std::unique_lock lock(state->mutex);
		if (!own || own->owner != state || own->StopRequested() || !task || !Layout.Contains(cls) ||
			delay > std::chrono::steady_clock::time_point::max() - now) {
			++state->stats.rejected;
			return false;
		}
		auto& queue = state->queues[cls.domain];
		if (queue.size() == state->capacities[cls.domain]) { ++state->stats.rejected; return false; }
		queue.push_back({ now + (std::max)(delay, std::chrono::steady_clock::duration::zero()), own, std::move(task) });
		++own->pending;
		++state->stats.accepted;
		state->stats.queued[cls.domain] = queue.size();
		state->stats.highWater[cls.domain] = (std::max)(state->stats.highWater[cls.domain], queue.size());
		lock.unlock();
		state->changed.notify_all();
		return true;
	}

	PublishedSceneExecutor::Statistics PublishedSceneExecutor::GetStatistics() const
	{
		std::lock_guard lock(state->mutex);
		return state->stats;
	}

	void PublishedSceneExecutor::Shutdown()
	{
		if (currentExecutor == state.get()) throw std::logic_error("scene executor shutdown must run on its owner");
		std::lock_guard shutdownLock(threads->shutdownMutex);
		{
			std::lock_guard lock(state->mutex);
			state->stopping.store(true);
		}
		state->changed.notify_all();
		for (auto& worker : threads->workers) if (worker.joinable()) worker.join();
	}
}
