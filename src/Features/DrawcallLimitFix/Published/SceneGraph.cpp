#include "SceneGraph.h"
#include "PublishedSceneExecutor.h"

#include <ORGModuleServices/Async/StateGraph.h>

#include <exception>

namespace DCLF::Published
{
	namespace
	{
		org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> Hooks()
		{
			org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> hooks;
			hooks.scheduling = PublishedSceneExecutor::Layout;
			hooks.producerDispatch = { org::async::TaskDispatch::Controlled, org::async::TaskDispatch::Cpu };
			static_assert(static_cast<std::size_t>(SceneArtifact::Count) <= org::async::DefaultStateGraphTypes::kArtifactKindCount);
			// The scene's revisions are its events, in order: never one replaced by a later one.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::SceneState)].allowCoalescing = false;
			// Each growth is its own address (PostGrowth): exact, never replaced by another.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::BufferVersion)].allowCoalescing = false;
			return hooks;
		}

		constexpr auto kBufferVersion = static_cast<std::uint32_t>(SceneArtifact::BufferVersion);
	}

	struct SceneGraph::GrowthWait
	{
		Graph::ArtifactAwaiter awaiter;
		std::atomic<bool> finished = false;
	};

	SceneGraph::SceneGraph(std::shared_ptr<org::async::GraphScheduler> a_scheduler) :
		graph(std::make_unique<Graph>(std::move(a_scheduler), "DCLF scene", Hooks()))
	{
		// Growth: the request's work on the preparation pool, ready with its uploads' completion (the graph waits for it).
		Graph::ArtifactProducerRegistration growth;
		growth.lane = PublishedSceneExecutor::Preparation.lane;
		growth.domain = PublishedSceneExecutor::Preparation.domain;
		growth.taskName = "DCLF buffer version";
		growth.producer = [](const Graph::ArtifactBuildContext& a_context) {
			const auto work = a_context.input.template Get<GrowthWork>();
			if (!work || !work->produce)
				return Graph::ArtifactBuildResult::Failure("a growth without its work");
			try {
				auto produced = work->produce();
				if (!produced.value)
					return Graph::ArtifactBuildResult::Failure("a growth produced nothing");
				auto ready = produced.ready;
				return Graph::ArtifactBuildResult::Ready(org::async::ArtifactPayload::Make(std::make_shared<const GrowthWork::Produced>(std::move(produced))),
					std::move(ready));
			} catch (const std::exception& error) {
				return Graph::ArtifactBuildResult::Failure(error.what());
			}
		};
		graph->RegisterProducer(kBufferVersion, std::move(growth));
	}

	bool SceneGraph::PostGrowth(GrowthWork a_work)
	{
		std::erase_if(growthWaits, [](const auto& a_wait) { return a_wait->finished.load(std::memory_order_acquire); });
		auto done = std::move(a_work.done);
		const Graph::ArtifactKey key{ kBufferVersion, ++growthIds, 0 };
		Graph::ArtifactIntent intent;
		intent.key = key;
		intent.desiredRevision = 1;
		intent.input = org::async::ArtifactPayload::Make(std::make_shared<const GrowthWork>(std::move(a_work)));
		intent.requestFingerprint = key.primaryID;  // an input needs one: each growth is its own
		const auto request = graph->PostRequest(std::move(intent), false);
		if (!request)
			return false;
		auto wait = std::make_shared<GrowthWait>();
		// The coordinator lane runs exactly one of these, once; the address is released after (nothing else requests it).
		auto finish = [this, key, done, weak = std::weak_ptr<GrowthWait>(wait)](std::shared_ptr<const void> a_value, std::string a_error) {
			done(std::move(a_value), std::move(a_error));
			graph->Release(key);
			if (const auto held = weak.lock())
				held->finished.store(true, std::memory_order_release);
		};
		wait->awaiter = graph->AwaitExact(request.Handle(), org::async::ArtifactReadiness::GpuReady, PublishedSceneExecutor::Coordinator.lane,
			PublishedSceneExecutor::Coordinator.domain,
			[finish](const Graph::ArtifactSnapshot& a_snapshot) {
				const auto produced = a_snapshot.payload.template Get<GrowthWork::Produced>();
				if (produced && produced->ready && produced->ready->Failed())
					finish(nullptr, produced->ready->Failure());
				else
					finish(produced ? produced->value : nullptr, produced ? std::string() : std::string("no version"));
			},
			[finish](const Graph::ArtifactTermination& a_termination) {
				finish(nullptr, a_termination.error.empty() ? std::string("the growth ended without a version") : a_termination.error);
			});
		growthWaits.push_back(std::move(wait));
		return true;
	}

	SceneGraph::~SceneGraph()
	{
		if (graph)
			graph->Shutdown();
	}

	SceneGraph::Graph& SceneGraph::Get()
	{
		return *graph;
	}
}
