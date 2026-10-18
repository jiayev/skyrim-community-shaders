#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>
#include <ORGModuleServices/Async/GraphScheduler.h>
#include <ORGModuleServices/Async/StateGraphTypes.h>
#include <ORGModuleServices/Async/VersionGrowthArtifact.h>

#include <cstdint>
#include <memory>

namespace DCLF::Runtime
{
	/**
	 * @brief The artifact kinds of DCLF's state graph (dclf-architecture.md, "The design"). Only BufferVersion has a producer
	 * (growth, registered here).
	 *
	 * SceneState and BufferVersion requests are exact and never coalesced; the other kinds are replaceable: a newer request
	 * supersedes an unstarted older one.
	 */
	enum class SceneArtifact : std::uint32_t
	{
		SceneState,         // the scene tables after a frame's events
		Lookups,            // material, pipeline and texture lookups
		FrameValues,        // a frame's placements, palettes, shading samples and visibility
		ShadowPayload,      // the per-segment payloads
		ZPrepassPayload,
		ColourPayload,
		ReflectionPayload,
		OcclusionPayload,
		Publication,        // what the render thread accepts: claims, filters and every segment's payload
		BufferVersion,      // a versioned buffer's next version, made and filled as graph work (growth: PostGrowth)
		Count
	};

	/** @brief A growth as graph work (org::async::VersionGrowthWork): a BufferVersion artifact made on the preparation pool, `done` on the coordinator lane. */
	using GrowthWork = org::async::VersionGrowthWork;

	/** @brief DCLF's state graph on DCLF's executor, with the growth producer. */
	class SceneGraph
	{
	public:
		using Graph = org::async::StateGraph<org::async::DefaultStateGraphTypes>;

		/** @brief a_scheduler must be the executor whose Layout the graph is built for (SceneExecutor). */
		explicit SceneGraph(std::shared_ptr<org::async::GraphScheduler> a_scheduler);
		~SceneGraph();
		SceneGraph(const SceneGraph&) = delete;
		SceneGraph& operator=(const SceneGraph&) = delete;

		Graph& Get();

		/**
		 * @brief Posts a growth (lock-free: the request and its wait are posted to the graph's drain). One thread posts (the render
		 * thread), and it keeps the waits, dropping those that finished at its next post. False when the graph refused it (shutting
		 * down): `done` does not run.
		 */
		bool PostGrowth(GrowthWork a_work);

	private:
		std::unique_ptr<Graph> graph;
		std::unique_ptr<org::async::VersionGrowthPosts<org::async::DefaultStateGraphTypes>> growths;  // the posting thread's
	};
}
