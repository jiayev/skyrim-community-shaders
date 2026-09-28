#include "Features/DrawcallLimitFix/Published/PublishedSceneExecutor.h"

#include <atomic>
#include <cassert>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using Executor = DCLF::PublishedSceneExecutor;
using namespace std::chrono_literals;

template <class T>
T Ready(std::future<T>& future)
{
	assert(future.wait_for(5s) == std::future_status::ready);
	return future.get();
}

void TestLanesAndBounds()
{
	Executor executor(2, 2), foreign;
	const auto scope = executor.CreateScope("bounded");
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
	assert(!executor.SubmitCpu(scope, 0, 1, "full", [](const auto&) {}));
	assert(!foreign.Submit(scope, 0, 0, "foreign", [](const auto&) {}));
	assert(!executor.Submit(scope, 1, 0, "invalid class", [](const auto&) {}));
	auto waiting = std::async(std::launch::async, [&] { scope->Wait(); });
	assert(waiting.wait_for(10ms) == std::future_status::timeout);
	scope->Cancel();
	release.set_value();
	Ready(waiting);
	assert(unexpectedlyRun == 0);
	const auto stats = executor.GetStatistics();
	assert(stats.highWater[1] == 2 && stats.queued[1] == 0 && stats.active[1] == 0 && stats.cancelled == 2);
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

int main()
{
	TestLanesAndBounds();
	TestOrderingAndFailures();
	TestLifetimeAndShutdown();
}
