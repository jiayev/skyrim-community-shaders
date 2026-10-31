// SceneGraph: the state graph's hooks, the growth producer and the payload parts' producer (preparation pool; their outcomes on the
// coordinator lane).
#include "Features/DrawcallLimitFix/Core/Runtime/SceneGraph.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneExecutor.h"

#include <ORGModuleServices/Async/StateGraph.h>
#include <ORGModuleServices/Async/VersionGrowthArtifact.h>
#include <ORGModuleServices/Async/VersionedBufferProducer.h>

#include <array>
#include <atomic>
#include <span>
#include <vector>

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
			// Each payload part version likewise (PostPayloadPart): a part's versions are a chain, each built from the one before.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::PayloadPart)].allowCoalescing = false;
			return hooks;
		}

		constexpr auto kBufferVersion = static_cast<std::uint16_t>(SceneArtifact::BufferVersion);
		constexpr auto kPayloadPart = static_cast<std::uint16_t>(SceneArtifact::PayloadPart);
	}

	/** @brief The posting thread's payload part waits (as VersionGrowthPosts'), dropped once finished at its next post. */
	struct SceneGraph::PayloadPartPosts
	{
		struct Wait
		{
			Graph::ArtifactAwaiter awaiter;
			std::atomic<bool> finished = false;
		};
		std::vector<std::shared_ptr<Wait>> waits;
		std::uint64_t ids = 0;
	};

	SceneGraph::SceneGraph(std::shared_ptr<org::async::GraphScheduler> a_scheduler) :
		graph(std::make_unique<Graph>(std::move(a_scheduler), "DCLF scene", Hooks())),
		growths(std::make_unique<org::async::VersionGrowthPosts<org::async::DefaultStateGraphTypes>>(kBufferVersion, SceneExecutor::Coordinator.lane,
			SceneExecutor::Coordinator.domain)),
		payloadParts(std::make_unique<PayloadPartPosts>())
	{
		// Growth: the request's work on the preparation pool, ready with its uploads' completion (the graph waits for it).
		org::async::RegisterVersionGrowthProducer(*graph, kBufferVersion, SceneExecutor::Preparation.lane, SceneExecutor::Preparation.domain, "DCLF buffer version");
		// The payload parts: a version built from its predecessor on the preparation pool (BuildVersionedBuffer's predecessor copy), ready
		// with its copy's and upload's completion; its tag's `built` runs on the producer's thread before it is frozen.
		org::services::VersionedBufferProducerHooks<org::async::DefaultStateGraphTypes> hooks;
		hooks.decorate = [](org::services::PublishedBufferVersion& a_version, const org::services::VersionedBufferBuildInput& a_input) {
			if (const auto* tag = static_cast<const PayloadPartTag*>(a_input.tag.get()); tag && tag->built)
				tag->built(a_version);
		};
		static constexpr std::array<std::uint16_t, 1> kPayloadKinds{ kPayloadPart };
		org::services::RegisterVersionedBufferProducer(*graph, std::span<const std::uint16_t>(kPayloadKinds), SceneExecutor::Preparation.lane,
			SceneExecutor::Preparation.domain, "DCLF payload part", std::move(hooks));
	}

	bool SceneGraph::PostGrowth(GrowthWork a_work)
	{
		return org::async::PostVersionGrowth(*graph, *growths, std::move(a_work));
	}

	bool SceneGraph::PostPayloadPart(org::services::VersionedBufferBuildInput a_input, PayloadPartDone a_done)
	{
		auto& posts = *payloadParts;
		std::erase_if(posts.waits, [](const auto& a_wait) { return a_wait->finished.load(std::memory_order_acquire); });
		const Graph::ArtifactKey key{ kPayloadPart, ++posts.ids, 0 };
		Graph::ArtifactIntent intent;
		intent.key = key;
		intent.desiredRevision = 1;
		intent.input = org::async::ArtifactPayload::Make(std::make_shared<const org::services::VersionedBufferBuildInput>(std::move(a_input)));
		intent.requestFingerprint = key.primaryID;  // an input needs one: each version is its own
		const auto request = graph->PostRequest(std::move(intent), false);
		if (!request)
			return false;
		auto wait = std::make_shared<PayloadPartPosts::Wait>();
		// The coordinator lane runs exactly one of these, once; the address is released after (nothing else requests it).
		auto finish = [graph = graph.get(), key, done = std::move(a_done), weak = std::weak_ptr<PayloadPartPosts::Wait>(wait)](
						  std::shared_ptr<const org::services::PublishedBufferVersion> a_version, std::string a_error) {
			done(std::move(a_version), std::move(a_error));
			graph->Release(key);
			if (const auto held = weak.lock())
				held->finished.store(true, std::memory_order_release);
		};
		wait->awaiter = graph->AwaitExact(request.Handle(), org::async::ArtifactReadiness::GpuReady, SceneExecutor::Coordinator.lane,
			SceneExecutor::Coordinator.domain,
			[finish](const Graph::ArtifactSnapshot& a_snapshot) {
				auto version = a_snapshot.payload.Get<org::services::PublishedBufferVersion>();
				if (a_snapshot.gpuSubmissions && a_snapshot.gpuSubmissions->Failed())
					finish(nullptr, a_snapshot.gpuSubmissions->Failure());
				else if (!version)
					finish(nullptr, "the build made no version");
				else
					finish(std::move(version), {});
			},
			[finish](const Graph::ArtifactTermination& a_termination) {
				finish(nullptr, a_termination.error.empty() ? std::string("the build ended without a version") : a_termination.error);
			});
		posts.waits.push_back(std::move(wait));
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
