// SceneGraph: the state graph's hooks and the growth producer (preparation pool; `done` on the coordinator lane).
#include "Features/DrawcallLimitFix/Runtime/SceneGraph.h"
#include "Features/DrawcallLimitFix/Runtime/SceneExecutor.h"

#include <ORGModuleServices/Async/StateGraph.h>
#include <ORGModuleServices/Async/VersionGrowthArtifact.h>

namespace DCLF::Runtime
{
	namespace
	{
		org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> Hooks()
		{
			org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> hooks;
			hooks.scheduling = SceneExecutor::Layout;
			hooks.producerDispatch = { org::async::TaskDispatch::Controlled, org::async::TaskDispatch::Cpu, org::async::TaskDispatch::Controlled };  // per domain: coordinator, preparation, scene
			static_assert(static_cast<std::size_t>(SceneArtifact::Count) <= org::async::DefaultStateGraphTypes::kArtifactKindCount);
			// The scene's revisions are its events, in order: never one replaced by a later one.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::SceneState)].allowCoalescing = false;
			// Each growth is its own address (PostGrowth): exact, never replaced by another.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::BufferVersion)].allowCoalescing = false;
			return hooks;
		}

		constexpr auto kBufferVersion = static_cast<std::uint16_t>(SceneArtifact::BufferVersion);
	}

	SceneGraph::SceneGraph(std::shared_ptr<org::async::GraphScheduler> a_scheduler) :
		graph(std::make_unique<Graph>(std::move(a_scheduler), "DCLF scene", Hooks())),
		growths(std::make_unique<org::async::VersionGrowthPosts<org::async::DefaultStateGraphTypes>>(kBufferVersion, SceneExecutor::Coordinator.lane,
			SceneExecutor::Coordinator.domain))
	{
		// Growth: the request's work on the preparation pool, ready with its uploads' completion (the graph waits for it).
		org::async::RegisterVersionGrowthProducer(*graph, kBufferVersion, SceneExecutor::Preparation.lane, SceneExecutor::Preparation.domain, "DCLF buffer version");
	}

	bool SceneGraph::PostGrowth(GrowthWork a_work)
	{
		return org::async::PostVersionGrowth(*graph, *growths, std::move(a_work));
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
