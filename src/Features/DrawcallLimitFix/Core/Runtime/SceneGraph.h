#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>
#include <ORGModuleServices/Async/GraphScheduler.h>
#include <ORGModuleServices/Async/StateGraphTypes.h>
#include <ORGModuleServices/Async/VersionGrowthArtifact.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace DCLF::Runtime
{
	/**
	 * @brief The artifact kinds of DCLF's state graph (dclf-architecture.md, "The design"). BufferVersion (growth) and PayloadPart have
	 * producers, registered here.
	 *
	 * SceneState, BufferVersion and PayloadPart requests are exact and never coalesced; the other kinds are replaceable: a newer
	 * request supersedes an unstarted older one.
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
		PayloadPart,        // a scene payload part's next version, built from its predecessor (PostPayloadPart)
		Count
	};

	/** @brief A growth as graph work (org::async::VersionGrowthWork): a BufferVersion artifact made on the preparation pool, `done` on the coordinator lane. */
	using GrowthWork = org::async::VersionGrowthWork;

	/**
	 * @brief A payload part version's tag (VersionedBufferBuildInput::tag of a PostPayloadPart input): `built` runs on the producer's
	 * thread once the version is made, before it is frozen (the part's pool's acquirer: its journal floor is read there). Must not throw.
	 */
	struct PayloadPartTag
	{
		std::function<void(const org::services::PublishedBufferVersion& a_version)> built;
	};
	/** @brief A payload part's outcome (PostPayloadPart), on the coordinator lane: the version once GPU ready, or null and why. */
	using PayloadPartDone = std::function<void(std::shared_ptr<const org::services::PublishedBufferVersion> a_version, std::string a_error)>;

	/** @brief DCLF's state graph on DCLF's executor, with the growth and payload part producers. */
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
		/**
		 * @brief Posts a payload part's next version (lock-free, as PostGrowth): a_input built on the preparation pool
		 * (org::services::RegisterVersionedBufferProducer: an exhausted pool suspends it until a backing frees), each at an address of its
		 * own, released once a_done ran on the coordinator lane, at the version's GPU readiness (its copy and upload complete) or its end.
		 * One thread posts at a time (the snapshot builder). False when the graph refused it (shutting down): a_done does not run.
		 */
		bool PostPayloadPart(org::services::VersionedBufferBuildInput a_input, PayloadPartDone a_done);

	private:
		struct PayloadPartPosts;
		std::unique_ptr<Graph> graph;
		std::unique_ptr<org::async::VersionGrowthPosts<org::async::DefaultStateGraphTypes>> growths;  // the posting thread's
		std::unique_ptr<PayloadPartPosts> payloadParts;                                             // the snapshot builder's
	};
}
