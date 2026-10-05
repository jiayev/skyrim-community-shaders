#include "SceneGraph.h"
#include "PublishedSceneExecutor.h"

#include <ORGModuleServices/Async/StateGraph.h>

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
			return hooks;
		}
	}

	SceneGraph::SceneGraph(std::shared_ptr<org::async::GraphScheduler> a_scheduler) :
		graph(std::make_unique<Graph>(std::move(a_scheduler), "DCLF scene", Hooks()))
	{}

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
