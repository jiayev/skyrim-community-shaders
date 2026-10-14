// SceneExecutor: per-domain job queues, worker threads, timers, scopes and ParallelFor.
#include "Features/DrawcallLimitFix/Core/Runtime/SceneExecutor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>

namespace DCLF
{
	namespace
	{
		thread_local const void* currentExecutor = nullptr;
		// A worker's current priority, read once after ThreadStart and kept by the boosts that change it (no query per chunk).
		thread_local int threadPriority = 0;

		template <class T>
		void RaiseTo(std::atomic<T>& a_max, T a_value)
		{
			for (T seen = a_max.load(std::memory_order_relaxed); a_value > seen && !a_max.compare_exchange_weak(seen, a_value, std::memory_order_relaxed);) {}
		}
	}

	struct SceneExecutor::State
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

		// One per ParallelFor name; never removed, so a helper that outlives its caller still counts into it.
		struct ParallelForEntry
		{
			std::string name;
			std::atomic<std::uint64_t> calls{ 0 }, chunks{ 0 }, helperChunks{ 0 }, blocked{ 0 }, blockedNanoseconds{ 0 }, maxBlockedNanoseconds{ 0 },
				boosts{ 0 }, idleHelpers{ 0 };
		};
		struct NameHash
		{
			using transparent_key_equal = std::equal_to<>;  // looked up by string_view: no string per call
			std::size_t operator()(std::string_view a_name) const noexcept { return std::hash<std::string_view>{}(a_name); }
		};

		std::atomic_bool stopping{ false };
		std::array<std::unique_ptr<Domain>, kDomains> domains;
		std::atomic<std::uint64_t> accepted{ 0 }, completed{ 0 }, cancelled{ 0 }, rejected{ 0 }, failed{ 0 };
		ThreadPriority priority;  // both set, or ParallelFor leaves priorities alone
		// Lock-free lookup and insertion; nodes never move.
		tbb::concurrent_unordered_map<std::string, std::unique_ptr<ParallelForEntry>, NameHash, std::equal_to<>> parallelFor;

		ParallelForEntry& Entry(std::string_view a_name)
		{
			if (const auto found = parallelFor.find(a_name); found != parallelFor.end())
				return *found->second;
			auto entry = std::make_unique<ParallelForEntry>();
			entry->name = a_name;
			return *parallelFor.emplace(std::string(a_name), std::move(entry)).first->second;  // a racing insert of the name wins
		}
		bool Boosts() const { return priority.get && priority.set; }

		explicit State(unsigned preparationWorkers, ThreadPriority a_priority) :
			priority(std::move(a_priority))
		{
			if (!preparationWorkers)
				throw std::invalid_argument("scene executor needs a preparation worker");
			for (auto& domain : domains)
				domain = std::make_unique<Domain>();
			domains[Coordinator.domain]->workers = 1;
			domains[Preparation.domain]->workers = preparationWorkers;
			domains[Scene.domain]->workers = 1;
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

	struct SceneExecutor::ExecutorScope final : org::async::TaskScope
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

	struct SceneExecutor::Threads
	{
		std::mutex shutdownMutex;
		std::vector<std::thread> workers;
	};

	void SceneExecutor::State::Execute(unsigned domain, Job& job)
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

	void SceneExecutor::State::Run(unsigned domain)
	{
		currentExecutor = this;
		if (Boosts())
			threadPriority = priority.get();  // after ThreadStart set it
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

	SceneExecutor::SceneExecutor(unsigned preparationWorkers, ThreadStart onThreadStart, ThreadPriority priority) :
		state(std::make_shared<State>(preparationWorkers, std::move(priority))), threads(std::make_unique<Threads>())
	{
		threads->workers.reserve(2 + preparationWorkers);
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
			threads->workers.emplace_back([shared = state, onThreadStart] {
				if (onThreadStart)
					onThreadStart(Scene, 0);
				shared->Run(2);
			});
		} catch (...) {
			Shutdown();
			throw;
		}
		parallelScope = CreateScope("parallel for");
	}

	SceneExecutor::~SceneExecutor() { Shutdown(); }

	org::async::Scope SceneExecutor::CreateScope(std::string_view)
	{
		return std::make_shared<ExecutorScope>(state);
	}

	bool SceneExecutor::Dispatch(const org::async::Scope& scope, org::async::TaskClass cls,
		org::async::TaskDispatch, std::string_view name, Task task, org::async::TaskTraceMetadata)
	{
		return DispatchAfter(scope, {}, cls, name, std::move(task));
	}

	bool SceneExecutor::DispatchAfter(const org::async::Scope& scope, std::chrono::steady_clock::duration delay,
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

	void SceneExecutor::ParallelFor(std::string_view a_name, std::size_t a_count, std::size_t a_grain,
		const std::function<void(std::size_t, std::size_t)>& a_body)
	{
		if (!a_count)
			return;
		auto& entry = state->Entry(a_name);
		entry.calls.fetch_add(1, std::memory_order_relaxed);
		const std::size_t grain = (std::max)(a_grain, std::size_t(1));
		const std::size_t chunks = (a_count + grain - 1) / grain;
		entry.chunks.fetch_add(chunks, std::memory_order_relaxed);
		if (chunks == 1) {
			a_body(0, a_count);
			return;
		}
		// Shared with the helpers, which may start after the caller returned: they touch the body only for a chunk they
		// claimed, and the caller returns only once every claimed chunk is done. The state and its entry outlive every worker.
		struct Shared
		{
			std::atomic<std::size_t> next{ 0 }, done{ 0 };
			std::size_t count = 0, grain = 0, chunks = 0;
			const std::function<void(std::size_t, std::size_t)>* body = nullptr;
			const ThreadPriority* priority = nullptr;  // null: priorities left alone
			int callerPriority = 0;
			State::ParallelForEntry* entry = nullptr;
			std::mutex errorMutex;
			std::exception_ptr error;
			// The chunks this thread ran.
			std::size_t Work()
			{
				std::size_t ran = 0;
				for (std::size_t chunk = next.fetch_add(1, std::memory_order_relaxed); chunk < chunks; chunk = next.fetch_add(1, std::memory_order_relaxed)) {
					try {
						(*body)(chunk * grain, (std::min)(count, (chunk + 1) * grain));
					} catch (...) {
						std::lock_guard lock(errorMutex);
						if (!error)
							error = std::current_exception();
					}
					++ran;
					if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == chunks)
						done.notify_all();
				}
				return ran;
			}
			// A preparation worker's share. Workers sit below normal priority, beside the engine's job threads; a caller above them
			// waits on any chunk a helper holds, so the helper holds it at the caller's priority (never lowered), restored after.
			void Help()
			{
				const int previous = threadPriority;
				const bool raise = priority && callerPriority > previous && next.load(std::memory_order_relaxed) < chunks;
				if (raise) {
					priority->set(callerPriority);
					threadPriority = callerPriority;  // a nested ParallelFor's helpers inherit it
					entry->boosts.fetch_add(1, std::memory_order_relaxed);
				}
				const std::size_t ran = Work();
				if (raise) {
					priority->set(previous);
					threadPriority = previous;
				}
				if (ran)
					entry->helperChunks.fetch_add(ran, std::memory_order_relaxed);
				else
					entry->idleHelpers.fetch_add(1, std::memory_order_relaxed);
			}
		};
		auto shared = std::make_shared<Shared>();
		shared->count = a_count;
		shared->grain = grain;
		shared->chunks = chunks;
		shared->body = &a_body;
		shared->entry = &entry;
		if (state->Boosts()) {
			shared->priority = &state->priority;
			// A worker's priority is cached (a helper running boosted counts as its caller's); any other thread is asked.
			shared->callerPriority = currentExecutor == state.get() ? threadPriority : state->priority.get();
		}
		const std::size_t helpers = (std::min)(chunks - 1, std::size_t(state->domains[1]->workers));
		for (std::size_t i = 0; i < helpers; ++i)
			if (!Dispatch(parallelScope, Preparation, org::async::TaskDispatch::Cpu, a_name, [shared](const org::async::TaskContext&) { shared->Help(); }))
				break;  // the caller takes what no helper does
		shared->Work();
		if (std::size_t done = shared->done.load(std::memory_order_acquire); done != chunks) {
			// Only chunks a helper claimed and still runs: the wait the statistics report.
			const auto blockedSince = std::chrono::steady_clock::now();
			for (; done != chunks; done = shared->done.load(std::memory_order_acquire))
				shared->done.wait(done, std::memory_order_acquire);
			const auto blocked = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - blockedSince).count());
			entry.blocked.fetch_add(1, std::memory_order_relaxed);
			entry.blockedNanoseconds.fetch_add(blocked, std::memory_order_relaxed);
			RaiseTo(entry.maxBlockedNanoseconds, blocked);
		}
		if (shared->error)
			std::rethrow_exception(shared->error);
	}

	SceneExecutor::Statistics SceneExecutor::GetStatistics() const
	{
		Statistics stats;
		stats.accepted = state->accepted.load();
		stats.completed = state->completed.load();
		stats.cancelled = state->cancelled.load();
		stats.rejected = state->rejected.load();
		stats.failed = state->failed.load();
		for (std::size_t d = 0; d < kDomains; ++d) {
			stats.queued[d] = state->domains[d]->ring.Size();
			stats.active[d] = state->domains[d]->active.load();
			stats.highWater[d] = state->domains[d]->highWater.load();
		}
		return stats;
	}

	std::vector<SceneExecutor::ParallelForStatistics> SceneExecutor::GetParallelForStatistics(bool a_takePeaks) const
	{
		std::vector<ParallelForStatistics> result;
		for (const auto& item : state->parallelFor) {  // safe beside concurrent inserts
			const auto& entry = item.second;
			auto& out = result.emplace_back();
			out.name = entry->name;
			out.calls = entry->calls.load(std::memory_order_relaxed);
			out.chunks = entry->chunks.load(std::memory_order_relaxed);
			out.helperChunks = entry->helperChunks.load(std::memory_order_relaxed);
			out.blocked = entry->blocked.load(std::memory_order_relaxed);
			out.blockedNanoseconds = entry->blockedNanoseconds.load(std::memory_order_relaxed);
			out.maxBlockedNanoseconds = a_takePeaks ? entry->maxBlockedNanoseconds.exchange(0, std::memory_order_relaxed) :
			                                          entry->maxBlockedNanoseconds.load(std::memory_order_relaxed);
			out.boosts = entry->boosts.load(std::memory_order_relaxed);
			out.idleHelpers = entry->idleHelpers.load(std::memory_order_relaxed);
		}
		return result;
	}

	void SceneExecutor::Shutdown()
	{
		if (currentExecutor == state.get()) throw std::logic_error("scene executor shutdown must run on its owner");
		std::lock_guard shutdownLock(threads->shutdownMutex);
		state->stopping.store(true);
		state->WakeAll();
		for (auto& worker : threads->workers) if (worker.joinable()) worker.join();
		// A dispatch that passed its check before stopping and pushed after its domain's workers left: run as cancelled here,
		// so its scope's count still reaches zero.
		for (unsigned domain = 0; domain < kDomains; ++domain)
			for (State::Job job; state->domains[domain]->ring.Pop(job);)
				state->Execute(domain, job);
	}
}
