#include "Features/DrawcallLimitFix/Runtime/SceneExecutor.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using Executor = DCLF::SceneExecutor;
using namespace std::chrono_literals;

template <class T>
T Ready(std::future<T>& future)
{
	assert(future.wait_for(5s) == std::future_status::ready);
	return future.get();
}

void TestLanesAndQueues()
{
	Executor executor(2), foreign;
	const auto scope = executor.CreateScope("unbounded");
	const auto caller = std::this_thread::get_id();
	std::promise<void> release;
	const auto gate = release.get_future().share();
	std::array<std::promise<std::thread::id>, 2> started;
	std::array<std::future<std::thread::id>, 2> futures{ started[0].get_future(), started[1].get_future() };
	for (unsigned i = 0; i != 2; ++i)
		assert(executor.SubmitCpu(scope, 0, 1, "blocked preparation", [&, i](const auto&) {
			started[i].set_value(std::this_thread::get_id());
			gate.wait();
		}));
	const auto first = Ready(futures[0]), second = Ready(futures[1]);
	assert(first != second && first != caller && second != caller);
	std::promise<std::thread::id> coordinator;
	auto coordinatorResult = coordinator.get_future();
	assert(executor.Submit(scope, 0, 0, "coordinator", [&](const auto&) { coordinator.set_value(std::this_thread::get_id()); }));
	const auto control = Ready(coordinatorResult);
	assert(control != caller && control != first && control != second);
	std::atomic_uint unexpectedlyRun{ 0 };
	for (unsigned i = 0; i != 2; ++i)
		assert(executor.ScheduleAfter(scope, 1h, 0, 1, "timer", [&](const auto&) { ++unexpectedlyRun; }));
	// No bound: queued behind the two blocked workers and the two timers.
	assert(executor.SubmitCpu(scope, 0, 1, "queued", [&](const auto&) { ++unexpectedlyRun; }));
	assert(!foreign.Submit(scope, 0, 0, "foreign", [](const auto&) {}));
	assert(!executor.Submit(scope, 1, 0, "invalid class", [](const auto&) {}));
	auto waiting = std::async(std::launch::async, [&] { scope->Wait(); });
	assert(waiting.wait_for(10ms) == std::future_status::timeout);
	scope->Cancel();
	release.set_value();
	Ready(waiting);
	assert(unexpectedlyRun == 0);
	const auto stats = executor.GetStatistics();
	assert(stats.highWater[1] == 3 && stats.queued[1] == 0 && stats.active[1] == 0 && stats.cancelled == 3 && stats.rejected == 1);
	assert(!executor.Submit(scope, 0, 0, "cancelled", [](const auto&) {}));
}

void TestOrderingAndFailures()
{
	Executor executor;
	const auto scope = executor.CreateScope("ordering");
	std::vector<unsigned> order;  // coordinator exclusively writes
	for (unsigned i = 0; i != 100; ++i)
		assert(executor.Submit(scope, 0, 0, "ordered", [&, i](const auto&) { order.push_back(i); }));
	scope->Wait();
	assert(order.size() == 100);
	for (unsigned i = 0; i != 100; ++i) assert(order[i] == i);
	const auto start = std::chrono::steady_clock::now();
	assert(executor.ScheduleAfter(scope, 20ms, 0, 0, "delay", [&](const auto&) {
		assert(std::chrono::steady_clock::now() - start >= 20ms);
	}));
	scope->Wait();
	assert(executor.Submit(scope, 0, 0, "reject worker wait", [scope](const auto&) {
		bool rejected = false;
		try { scope->Wait(); } catch (const std::logic_error&) { rejected = true; }
		assert(rejected);
	}));
	scope->Wait();
	const auto broken = executor.CreateScope("failure");
	assert(executor.SubmitCpu(broken, 0, 1, "throws", [](const auto&) { throw std::runtime_error("expected"); }));
	bool failed = false;
	try { broken->Wait(); } catch (const std::runtime_error&) { failed = true; }
	assert(failed && executor.GetStatistics().failed == 1);
	assert(executor.Submit(scope, 0, 0, "survives", [](const auto&) {}));
	scope->Wait();
}

void TestLifetimeAndShutdown()
{
	struct Retained
	{
		std::promise<std::thread::id>* destroyed;
		~Retained() { destroyed->set_value(std::this_thread::get_id()); }
	};
	std::function<bool()> savedCancellation;
	org::async::Scope retainedScope;
	std::promise<std::thread::id> destroyed;
	auto destruction = destroyed.get_future();
	const auto caller = std::this_thread::get_id();
	{
		Executor executor;
		retainedScope = executor.CreateScope("retained");
		assert(executor.Submit(retainedScope, 0, 0, "query", [&](const auto& context) { savedCancellation = context.stopRequested; }));
		retainedScope->Wait();
		assert(savedCancellation && !savedCancellation());
		auto owner = std::make_shared<Retained>();
		owner->destroyed = &destroyed;
		assert(executor.ScheduleAfter(retainedScope, 1h, 0, 0, "retained timer", [owner](const auto&) { assert(false); }));
		owner.reset();
		executor.Shutdown();
		assert(Ready(destruction) != caller);
		assert(savedCancellation());
		assert(!executor.Submit(retainedScope, 0, 0, "stopped", [](const auto&) {}));
		executor.Shutdown();
	}
	assert(savedCancellation());
	retainedScope->CancelAndWait();
}

void TestParallelFor()
{
	Executor executor(4);
	// Every index once, across several threads, from a coordinator task (the frame's producers call it there).
	const std::size_t count = 100000;
	std::vector<std::atomic_uint> hits(count);
	std::mutex threadsMutex;
	std::set<std::thread::id> threads;
	std::promise<void> finished;
	auto result = finished.get_future();
	const auto scope = executor.CreateScope("parallel for");
	assert(executor.Submit(scope, 0, 0, "coordinator", [&](const auto&) {
		executor.ParallelFor("test", count, 64, [&](std::size_t a_begin, std::size_t a_end) {
			for (std::size_t i = a_begin; i < a_end; ++i)
				hits[i].fetch_add(1);
			std::this_thread::sleep_for(std::chrono::microseconds(20));
			std::lock_guard lock(threadsMutex);
			threads.insert(std::this_thread::get_id());
		});
		finished.set_value();
	}));
	Ready(result);
	for (const auto& hit : hits)
		assert(hit.load() == 1);
	assert(threads.size() > 1);
	// Nothing, one chunk, and a throwing chunk: the rest still run, and the first error reaches the caller.
	executor.ParallelFor("empty", 0, 16, [](std::size_t, std::size_t) { assert(false); });
	std::size_t single = 0;
	executor.ParallelFor("single", 10, 16, [&](std::size_t a_begin, std::size_t a_end) { single += a_end - a_begin; });
	assert(single == 10);
	std::atomic_uint ran{ 0 };
	bool threw = false;
	try {
		executor.ParallelFor("throws", 1000, 10, [&](std::size_t a_begin, std::size_t) {
			ran.fetch_add(1);
			if (a_begin == 500)
				throw std::runtime_error("chunk");
		});
	} catch (const std::runtime_error&) {
		threw = true;
	}
	assert(threw && ran.load() == 100);
	// Helpers that start after the caller returned do nothing with the (gone) body.
	for (unsigned round = 0; round != 200; ++round) {
		std::vector<int> local(8, 0);
		executor.ParallelFor("short", local.size(), 1, [&](std::size_t a_begin, std::size_t) { local[a_begin] = 1; });
		for (int value : local)
			assert(value == 1);
	}
}

// Every size and grain: every index exactly once, in chunks [k * grain, min(count, (k + 1) * grain)) (callers index per-chunk
// results by a_begin / grain).
void TestParallelForCoverage()
{
	Executor executor(3);
	for (const std::size_t count : { 1, 2, 3, 7, 63, 64, 65, 127, 1000, 4097 })
		for (const std::size_t grain : { 0, 1, 2, 3, 16, 64, 1000, 5000 }) {
			const std::size_t step = grain ? grain : 1;
			std::vector<std::atomic_uint> hits(count);
			std::atomic_bool aligned{ true };
			executor.ParallelFor("coverage", count, grain, [&](std::size_t a_begin, std::size_t a_end) {
				if (a_begin % step || a_end != (std::min)(count, a_begin + step))
					aligned = false;
				for (std::size_t i = a_begin; i < a_end; ++i)
					hits[i].fetch_add(1);
			});
			assert(aligned.load());
			for (const auto& hit : hits)
				assert(hit.load() == 1);
		}
	const auto stats = executor.GetParallelForStatistics();
	assert(stats.size() == 1 && stats[0].name == "coverage" && stats[0].calls == 10 * 8);
}

namespace
{
	// Stands in for the OS thread priority: the executor reads and sets only the calling thread's.
	thread_local int fakePriority = 0;
	Executor::ThreadPriority FakePriority(std::atomic_uint& a_sets)
	{
		return { [] { return fakePriority; }, [&a_sets](int a_priority) { a_sets.fetch_add(1); fakePriority = a_priority; } };
	}
	Executor::ThreadStart FakeStart()
	{
		// As SceneScheduler: the coordinator and the scene lane above normal, the workers below.
		return [](org::async::TaskClass a_class, unsigned) { fakePriority = a_class.domain == Executor::Preparation.domain ? -1 : 1; };
	}
	const Executor::ParallelForStatistics& Find(const std::vector<Executor::ParallelForStatistics>& a_stats, std::string_view a_name)
	{
		for (const auto& stats : a_stats)
			if (stats.name == a_name)
				return stats;
		assert(false);
		return a_stats.front();
	}
	// Every worker of the pool reports its priority at once (each blocks until all have started, so each runs one).
	std::vector<int> WorkerPriorities(Executor& a_executor, unsigned a_workers)
	{
		const auto scope = a_executor.CreateScope("priorities");
		std::mutex seenMutex;
		std::vector<int> seen;
		std::atomic_uint started{ 0 };
		for (unsigned i = 0; i != a_workers; ++i)
			assert(a_executor.SubmitCpu(scope, 0, 1, "priority", [&](const auto&) {
				{
					std::lock_guard lock(seenMutex);
					seen.push_back(fakePriority);
				}
				started.fetch_add(1);
				for (const auto deadline = std::chrono::steady_clock::now() + 5s; started.load() != a_workers && std::chrono::steady_clock::now() < deadline;)
					std::this_thread::yield();
			}));
		scope->Wait();
		assert(seen.size() == a_workers);
		return seen;
	}
}

// A helper runs an above-normal caller's chunks at the caller's priority and goes back to its own afterwards, also when a chunk
// throws; a caller below the helpers never lowers them.
void TestParallelForPriority()
{
	constexpr unsigned kWorkers = 3;
	std::atomic_uint sets{ 0 };
	Executor executor(kWorkers, FakeStart(), FakePriority(sets));
	for (const int priority : WorkerPriorities(executor, kWorkers))
		assert(priority == -1);
	const auto scope = executor.CreateScope("boost");
	std::mutex seenMutex;
	std::set<std::thread::id> helpers;
	std::atomic_bool wrongPriority{ false }, threw{ false };
	assert(executor.Submit(scope, 0, 0, "above normal caller", [&](const auto&) {
		const auto caller = std::this_thread::get_id();
		auto body = [&](std::size_t a_begin, std::size_t) {
			if (fakePriority != 1)
				wrongPriority = true;
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			if (std::this_thread::get_id() != caller) {
				std::lock_guard lock(seenMutex);
				helpers.insert(std::this_thread::get_id());
			}
			if (a_begin == 37)
				throw std::runtime_error("chunk");
		};
		try { executor.ParallelFor("boosted", 64, 1, body); } catch (const std::runtime_error&) { threw = true; }
		assert(fakePriority == 1);
	}));
	scope->Wait();
	assert(threw.load() && !wrongPriority.load() && !helpers.empty());
	// Restored: every worker is back below normal.
	for (const int priority : WorkerPriorities(executor, kWorkers))
		assert(priority == -1);
	const auto boosted = Find(executor.GetParallelForStatistics(), "boosted");
	assert(boosted.boosts >= helpers.size() && sets.load() == 2 * boosted.boosts && boosted.chunks == 64);
	// A caller below the workers: they keep their own priority, and nothing is set.
	fakePriority = -2;
	const unsigned setsBefore = sets.load();
	std::atomic_bool lowered{ false };
	const auto self = std::this_thread::get_id();
	executor.ParallelFor("below", 64, 1, [&](std::size_t, std::size_t) {
		if (fakePriority != (std::this_thread::get_id() == self ? -2 : -1))
			lowered = true;
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	});
	assert(!lowered.load() && sets.load() == setsBefore && Find(executor.GetParallelForStatistics(), "below").boosts == 0);
	for (const int priority : WorkerPriorities(executor, kWorkers))
		assert(priority == -1);
	fakePriority = 0;
}

// The caller waits only for a chunk a helper holds, and that wait is reported under its name; helpers that start late do nothing.
void TestParallelForWaitAndLateHelpers()
{
	Executor executor(2);
	// The caller holds chunk 0 until a helper has claimed chunk 1, which the helper then holds for a while: the caller blocks.
	const auto caller = std::this_thread::get_id();
	std::atomic_bool helperStarted{ false };
	executor.ParallelFor("blocks", 2, 1, [&](std::size_t, std::size_t) {
		if (std::this_thread::get_id() != caller) {
			helperStarted = true;
			std::this_thread::sleep_for(30ms);
			return;
		}
		for (const auto deadline = std::chrono::steady_clock::now() + 5s; !helperStarted.load() && std::chrono::steady_clock::now() < deadline;)
			std::this_thread::yield();
	});
	assert(helperStarted.load());
	// A helper counts its chunks once it finished them, possibly after the caller returned.
	for (const auto deadline = std::chrono::steady_clock::now() + 5s; Find(executor.GetParallelForStatistics(), "blocks").helperChunks != 1 && std::chrono::steady_clock::now() < deadline;)
		std::this_thread::yield();
	const auto blocks = Find(executor.GetParallelForStatistics(true), "blocks");
	assert(blocks.calls == 1 && blocks.chunks == 2 && blocks.blocked == 1 && blocks.blockedNanoseconds >= 20'000'000 &&
		   blocks.maxBlockedNanoseconds == blocks.blockedNanoseconds && blocks.helperChunks == 1);
	assert(Find(executor.GetParallelForStatistics(), "blocks").maxBlockedNanoseconds == 0);  // taken
	// Every worker busy: the caller runs every chunk itself and never waits; the helpers find nothing once they start.
	const auto scope = executor.CreateScope("busy");
	std::promise<void> release;
	const auto gate = release.get_future().share();
	std::atomic_uint busy{ 0 };
	for (unsigned i = 0; i != 2; ++i)
		assert(executor.SubmitCpu(scope, 0, 1, "busy", [&](const auto&) {
			busy.fetch_add(1);
			gate.wait();
		}));
	for (const auto deadline = std::chrono::steady_clock::now() + 5s; busy.load() != 2 && std::chrono::steady_clock::now() < deadline;)
		std::this_thread::yield();
	assert(busy.load() == 2);
	std::vector<int> local(100, 0);
	executor.ParallelFor("late", local.size(), 10, [&](std::size_t a_begin, std::size_t a_end) {
		assert(std::this_thread::get_id() == caller);
		for (std::size_t i = a_begin; i < a_end; ++i)
			++local[i];
	});
	for (int value : local)
		assert(value == 1);
	release.set_value();
	scope->Wait();
	for (const auto deadline = std::chrono::steady_clock::now() + 5s; Find(executor.GetParallelForStatistics(), "late").idleHelpers != 2 && std::chrono::steady_clock::now() < deadline;)
		std::this_thread::yield();
	const auto late = Find(executor.GetParallelForStatistics(), "late");
	assert(late.idleHelpers == 2 && late.helperChunks == 0 && late.blocked == 0 && late.chunks == 10);
}

int main()
{
	TestLanesAndQueues();
	TestOrderingAndFailures();
	TestLifetimeAndShutdown();
	TestParallelFor();
	TestParallelForCoverage();
	TestParallelForPriority();
	TestParallelForWaitAndLateHelpers();
}
