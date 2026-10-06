#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>
#include <ORGModuleServices/Async/GraphScheduler.h>
#include <ORGModuleServices/Async/StateGraphTypes.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
		BufferVersion,      // a versioned buffer's next version, made and filled as graph work (growth: PostGrowth)
		Count
	};

	/**
	 * @brief A growth as graph work (the SARP/BasicRenderer model, BuildVersionedGpuBuffer): a BufferVersion artifact whose producer
	 * runs on the preparation pool, makes the version and queues its fills on the dedicated uploader, and is ready once those copies
	 * completed (the uploader's tickets notify); `done` then runs on the coordinator lane. Type-erased, so the graph's library need
	 * not see ORG's buffers.
	 */
	struct GrowthWork
	{
		struct Produced
		{
			std::shared_ptr<const void> value;                          // what `done` is given
			std::shared_ptr<const org::async::GpuSubmissionSet> ready;  // null: ready now
		};
		std::function<Produced()> produce;  // preparation pool; throws when the version cannot be made or filled
		std::function<void(std::shared_ptr<const void> a_value, std::string a_error)> done;  // coordinator lane, once: null and why on failure
	};

	/** @brief DCLF's AsyncStateGraph on DCLF's executor. Producers come with the phases that need them; growth's is registered here. */
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

		/**
		 * @brief Posts a growth (lock-free: the request and its wait are posted to the graph's drain). One thread posts (the render
		 * thread), and it keeps the waits, dropping those that finished at its next post. False when the graph refused it (shutting
		 * down): `done` does not run.
		 */
		bool PostGrowth(GrowthWork a_work);

	private:
		std::unique_ptr<Graph> graph;
		struct GrowthWait;
		std::vector<std::shared_ptr<GrowthWait>> growthWaits;  // the posting thread's
		std::uint64_t growthIds = 0;
	};
}
