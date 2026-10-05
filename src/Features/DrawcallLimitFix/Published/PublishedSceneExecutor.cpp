#include "PublishedSceneExecutor.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

#include <tbb/concurrent_queue.h>

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

		// Unbounded and lock-free for producers: any thread pushes, every worker of the domain pops.
		struct Queue
		{
			tbb::concurrent_queue<Job> jobs;
			std::atomic<std::size_t> size{ 0 };  // pushed and not yet popped: the statistics' queued count
			void Push(Job&& a_job)
			{
				size.fetch_add(1, std::memory_order_relaxed);
				jobs.push(std::move(a_job));
			}
			bool Pop(Job& a_job)
			{
				if (!jobs.try_pop(a_job))
					return false;
				size.fetch_sub(1, std::memory_order_relaxed);
				return true;
			}
			std::size_t Size() const { return size.load(std::memory_order_relaxed); }
		};

		struct Domain
		{
			Queue ring;
			std::counting_semaphore<> available{ 0 };  // one release per pushed job, and per wake-up (Cancel, Shutdown)
			unsigned workers = 0;
			std::atomic<std::size_t> active{ 0 }, highWater{ 0 };
		};

		std::atomic_bool stopping{ false };
		std::array<std::unique_ptr<Domain>, 2> domains;
		std::atomic<std::uint64_t> accepted{ 0 }, completed{ 0 }, cancelled{ 0 }, rejected{ 0 }, failed{ 0 };

		explicit State(unsigned preparationWorkers)
		{
			if (!preparationWorkers)
				throw std::invalid_argument("scene executor needs a preparation worker");
			domains[0] = std::make_unique<Domain>();
			domains[1] = std::make_unique<Domain>();
			domains[0]->workers = 1;
			domains[1]->workers = preparationWorkers;
		}
		// Every worker of every domain looks again (a cancelled scope's timers, shutdown).
		void WakeAll()
		{
			for (auto& domain : domains)
				domain->available.release(domain->workers);
		}
		void Run(unsigned domain);
		void Execute(unsigned domain, Job& job);
	};

	struct PublishedSceneExecutor::ExecutorScope final : org::async::TaskScope
	{
		std::shared_ptr<State> owner;
		std::atomic_bool cancelled{ false };
		std::atomic<std::size_t> pending{ 0 };  // includes executing work and timers
		// The first failure: written when a task throws, read by Wait (both off the frame path).
		mutable std::mutex failureMutex;
		std::exception_ptr failure;
		explicit ExecutorScope(std::shared_ptr<State> value) : owner(std::move(value)) {}
		bool StopRequested() const noexcept override { return cancelled.load() || owner->stopping.load(); }
		void Cancel() noexcept override
		{
			cancelled.store(true);
			owner->WakeAll();  // its timers are reclaimed now, not at their deadline
		}
		void Wait() const override
		{
			if (currentExecutor == owner.get()) throw std::logic_error("scene workers cannot wait on scene scopes");
			for (auto value = pending.load(); value != 0; value = pending.load())
				pending.wait(value);
			std::exception_ptr error;
			{
				std::lock_guard lock(failureMutex);
				error = failure;
			}
			if (error) std::rethrow_exception(error);
		}
	};

	struct PublishedSceneExecutor::Threads
	{
		std::mutex shutdownMutex;
		std::vector<std::thread> workers;
	};

	void PublishedSceneExecutor::State::Execute(unsigned domain, Job& job)
	{
		auto& d = *domains[domain];
		d.active.fetch_add(1, std::memory_order_relaxed);
		auto scope = std::move(job.scope);
		std::exception_ptr error;
		const bool wasCancelled = scope->StopRequested();
		if (!wasCancelled) {
			try { job.task({ [scope] { return scope->StopRequested(); } }); }
			catch (...) { error = std::current_exception(); }
		}
		// Release captured resources before signalling completion, including when the task never started. Cancel never
		// destroys queued work.
		job.task = {};
		d.active.fetch_sub(1, std::memory_order_relaxed);
		if (error) {
			failed.fetch_add(1, std::memory_order_relaxed);
			std::lock_guard lock(scope->failureMutex);
			if (!scope->failure) scope->failure = error;
		} else if (wasCancelled) {
			cancelled.fetch_add(1, std::memory_order_relaxed);
		} else {
			completed.fetch_add(1, std::memory_order_relaxed);
		}
		if (scope->pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
			scope->pending.notify_all();
	}

	void PublishedSceneExecutor::State::Run(unsigned domain)
	{
		currentExecutor = this;
		auto& d = *domains[domain];
		// The delayed jobs this worker took from the ring before their time: its own, earliest first.
		std::vector<Job> timers;
		auto later = [](const Job& a, const Job& b) { return a.ready > b.ready; };
		for (;;) {
			// Due timers and cancelled ones first (a cancelled scope's timers are reclaimed now, not at their deadline).
			const auto now = std::chrono::steady_clock::now();
			if (std::any_of(timers.begin(), timers.end(), [&](const Job& a_job) { return a_job.ready <= now || a_job.scope->StopRequested(); })) {
				std::vector<Job> kept;
				for (auto& timer : timers) {
					if (timer.ready <= now || timer.scope->StopRequested())
						Execute(domain, timer);
					else
						kept.push_back(std::move(timer));
				}
				timers = std::move(kept);
				std::make_heap(timers.begin(), timers.end(), later);
			}
			if (stopping.load() && timers.empty() && d.ring.Size() == 0) {
				currentExecutor = nullptr;
				return;
			}
			// One token per pushed job, and per wake-up (Cancel, Shutdown): acquired before the pop, so tokens never pile up.
			if (stopping.load())
				(void)d.available.try_acquire_for(std::chrono::milliseconds(1));  // draining: never block on a spent token
			else if (timers.empty())
				d.available.acquire();
			else if (!d.available.try_acquire_until(timers.front().ready))
				continue;  // the earliest timer is due
			Job job;
			if (!d.ring.Pop(job))
				continue;  // a wake-up, or a job another worker took with its token
			if (job.ready > std::chrono::steady_clock::now() && !job.scope->StopRequested()) {
				timers.push_back(std::move(job));
				std::push_heap(timers.begin(), timers.end(), later);
			} else {
				Execute(domain, job);
			}
		}
	}

	PublishedSceneExecutor::PublishedSceneExecutor(unsigned preparationWorkers, ThreadStart onThreadStart) :
		state(std::make_shared<State>(preparationWorkers)), threads(std::make_unique<Threads>())
	{
		threads->workers.reserve(1 + preparationWorkers);
		try {
			threads->workers.emplace_back([shared = state, onThreadStart] {
				if (onThreadStart)
					onThreadStart(Coordinator, 0);
				shared->Run(0);
			});
			for (unsigned i = 0; i < preparationWorkers; ++i)
				threads->workers.emplace_back([shared = state, onThreadStart, i] {
					if (onThreadStart)
						onThreadStart(Preparation, i);
					shared->Run(1);
				});
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
		if (!own || own->owner != state || own->StopRequested() || !task || !Layout.Contains(cls) ||
			delay > std::chrono::steady_clock::time_point::max() - now) {
			state->rejected.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		auto& domain = *state->domains[cls.domain];
		// Counted before the push: a worker may finish the job before this returns.
		own->pending.fetch_add(1, std::memory_order_acq_rel);
		domain.ring.Push({ now + (std::max)(delay, std::chrono::steady_clock::duration::zero()), own, std::move(task) });
		state->accepted.fetch_add(1, std::memory_order_relaxed);
		const std::size_t queued = domain.ring.Size();
		for (std::size_t high = domain.highWater.load(std::memory_order_relaxed); queued > high &&
			 !domain.highWater.compare_exchange_weak(high, queued, std::memory_order_relaxed);) {}
		domain.available.release();
		return true;
	}

	PublishedSceneExecutor::Statistics PublishedSceneExecutor::GetStatistics() const
	{
		Statistics stats;
		stats.accepted = state->accepted.load();
		stats.completed = state->completed.load();
		stats.cancelled = state->cancelled.load();
		stats.rejected = state->rejected.load();
		stats.failed = state->failed.load();
		for (std::size_t d = 0; d < 2; ++d) {
			stats.queued[d] = state->domains[d]->ring.Size();
			stats.active[d] = state->domains[d]->active.load();
			stats.highWater[d] = state->domains[d]->highWater.load();
		}
		return stats;
	}

	void PublishedSceneExecutor::Shutdown()
	{
		if (currentExecutor == state.get()) throw std::logic_error("scene executor shutdown must run on its owner");
		std::lock_guard shutdownLock(threads->shutdownMutex);
		state->stopping.store(true);
		state->WakeAll();
		for (auto& worker : threads->workers) if (worker.joinable()) worker.join();
		// A dispatch that passed its check before stopping and pushed after its domain's workers left: run as cancelled here,
		// so its scope's count still reaches zero.
		for (unsigned domain = 0; domain < 2; ++domain)
			for (State::Job job; state->domains[domain]->ring.Pop(job);)
				state->Execute(domain, job);
	}
}
