#pragma once

#include <ORGModuleServices/Async/GraphScheduler.h>
#include <ORGModuleServices/Async/StateGraphTypes.h>

#include <cstdint>
#include <memory>

namespace org::async
{
	template <class Binding>
	class StateGraph;
}

namespace DCLF::Published
{
	/**
	 * @brief The DCLF scene's artifacts (dclf-async-publication.md, "The design"): the kinds of DCLF's AsyncStateGraph.
	 *
	 * SceneState is the coordinator's scene, revised in event order: its requests are exact and never coalesced, so no
	 * revision's events are skipped. The rest are derived and replaceable: a newer request supersedes an unstarted older one.
	 */
	enum class SceneArtifact : std::uint32_t
	{
		SceneState,         // the scene tables after a frame's events (phase 3)
		Lookups,            // material, pipeline and texture lookups (phase 5)
		FrameValues,        // a frame's placements, palettes, shading samples and visibility (phase 4)
		ShadowPayload,      // the per-segment payloads (phase 6)
		ZPrepassPayload,
		ColourPayload,
		ReflectionPayload,
		OcclusionPayload,
		Publication,        // what the render thread accepts: claims, filters and every segment's payload (phase 3 on)
		Count
	};

	/** @brief DCLF's AsyncStateGraph on DCLF's executor. No producer is registered yet: each phase registers its own. */
	class SceneGraph
	{
	public:
		using Graph = org::async::StateGraph<org::async::DefaultStateGraphTypes>;

		/** @brief a_scheduler must be the executor whose Layout the graph is built for (PublishedSceneExecutor). */
		explicit SceneGraph(std::shared_ptr<org::async::GraphScheduler> a_scheduler);
		~SceneGraph();
		SceneGraph(const SceneGraph&) = delete;
		SceneGraph& operator=(const SceneGraph&) = delete;

		Graph& Get();

	private:
		std::unique_ptr<Graph> graph;
	};
}
