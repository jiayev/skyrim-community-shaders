#include "Features/DrawcallLimitFix/Published/PublishedSceneExecutor.h"
#include <ORGModuleServices/Async/StateGraph.h>

#include <cassert>
#include <future>
#include <thread>

using namespace std::chrono_literals;

template <class Predicate>
void Until(Predicate predicate)
{
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (!predicate()) {
		assert(std::chrono::steady_clock::now() < deadline);
		std::this_thread::yield();
	}
}

int main()
{
	using Graph = org::async::AsyncStateGraph;
	using Executor = DCLF::PublishedSceneExecutor;
	using Result = Graph::ArtifactBuildResult;
	using org::async::ArtifactReadiness;
	auto scheduler = std::make_shared<Executor>();
	org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> hooks;
	hooks.scheduling = Executor::Layout;
	hooks.producerDispatch = { org::async::TaskDispatch::Controlled, org::async::TaskDispatch::Cpu };
	Graph graph(scheduler, "DCLF isolated parity test", hooks);
	std::array<std::promise<void>, 2> started, release;
	std::array<std::future<void>, 2> startedResults{ started[0].get_future(), started[1].get_future() };
	std::array<std::shared_future<void>, 2> gates{ release[0].get_future().share(), release[1].get_future().share() };
	Graph::ArtifactProducerRegistration part;
	part.domain = Executor::Preparation.domain;
	part.producer = [&](const auto& context) {
		const auto index = context.key.primaryID;
		started[index].set_value();
		gates[index].wait();
		return Result::Ready(org::async::ArtifactPayload::Make(std::make_shared<const std::uint64_t>(context.revision)));
	};
	graph.RegisterProducer(0, part);
	Graph::ArtifactProducerRegistration complete;
	complete.domain = Executor::Coordinator.domain;
	complete.producer = [](const auto& context) {
		assert(context.dependencies.size() == 2);
		for (const auto& dependency : context.dependencies)
			assert(*dependency.payload.template Get<std::uint64_t>() == context.revision);
		return Result::Ready(org::async::ArtifactPayload::Make(std::make_shared<const std::uint64_t>(context.revision)));
	};
	graph.RegisterProducer(1, complete);
	const auto first = graph.PostRequest({{0, 0, 0}, 7}, false);
	const auto second = graph.PostRequest({{0, 1, 0}, 7}, false);
	const auto group = graph.PostRequest({{1, 1, 0}, 7, {org::async::Exact(first.Handle()), org::async::Exact(second.Handle())}}, false);
	assert(first && second && group);
	for (auto& future : startedResults) assert(future.wait_for(5s) == std::future_status::ready);
	// Both preparation lanes are occupied; the coordinator still processes work.
	const auto unrelated = graph.PostRequest({{2, 1, 0}, 1}, false);
	assert(unrelated);
	Until([&] { return graph.Snapshot(unrelated.version).readiness == ArtifactReadiness::Failed; });
	release[1].set_value();
	Until([&] { return graph.Snapshot(second.version).readiness == ArtifactReadiness::GpuReady; });
	assert(!graph.Snapshot(group.version).payload.Valid());
	release[0].set_value();
	Until([&] { return graph.Snapshot(group.version).readiness == ArtifactReadiness::GpuReady; });
	assert(*graph.Snapshot(group.version).payload.Get<std::uint64_t>() == 7);
	// Blocking cleanup is deliberate in the test, not an engine/render-thread API.
	graph.Shutdown();
	scheduler->Shutdown();
	const auto stats = scheduler->GetStatistics();
	assert(stats.active[0] == 0 && stats.active[1] == 0 && stats.queued[0] == 0 && stats.queued[1] == 0);
	assert(stats.failed == 0 && stats.rejected == 0);
}
