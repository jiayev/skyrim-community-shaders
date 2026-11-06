// SceneGraph: the state graph's hooks, the growth producer and the payload parts' producer (preparation pool; their outcomes on the
// coordinator lane).
#include <mutex>
#include <vector>
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
			hooks.producerDispatch = { org::async::TaskDispatch::Controlled, org::async::TaskDispatch::Cpu, org::async::TaskDispatch::Controlled, org::async::TaskDispatch::Cpu };  // per domain: coordinator, preparation, scene, streaming
			static_assert(static_cast<std::size_t>(SceneArtifact::Count) <= org::async::DefaultStateGraphTypes::kArtifactKindCount);
			// The scene's revisions are its events, in order: never one replaced by a later one.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::SceneState)].allowCoalescing = false;
			// Each growth is its own address (PostGrowth): exact, never replaced by another.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::BufferVersion)].allowCoalescing = false;
			// Each payload part version likewise (PostPayloadPart): a part's versions are a chain, each built from the one before.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::PayloadPart)].allowCoalescing = false;
			// Each gate is its own request's.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::TextureGate)].allowCoalescing = false;
			return hooks;
		}

		constexpr auto kBufferVersion = static_cast<std::uint16_t>(SceneArtifact::BufferVersion);
		constexpr auto kPayloadPart = static_cast<std::uint16_t>(SceneArtifact::PayloadPart);
		constexpr auto kTextureWindow = static_cast<std::uint16_t>(SceneArtifact::TextureWindow);
		constexpr auto kTextureGate = static_cast<std::uint16_t>(SceneArtifact::TextureGate);

		struct TextureWindowInput
		{
			std::function<void()> build;
		};
		struct TextureWindowBuilt
		{
		};
		struct TextureGateInput
		{
			std::function<void()> run;
		};
		struct TextureGateRan
		{
			std::uint64_t id = 0;
		};
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

	/** @brief The gates that ran, whose addresses go at the next post. */
	struct SceneGraph::TextureGates
	{
		std::atomic<std::uint64_t> ids{ 0 };
		std::mutex mutex;
		std::vector<Graph::ArtifactKey> ran;
	};

	SceneGraph::SceneGraph(std::shared_ptr<org::async::GraphScheduler> a_scheduler) :
		graph(std::make_unique<Graph>(std::move(a_scheduler), "DCLF scene", Hooks())),
		growths(std::make_unique<org::async::VersionGrowthPosts<org::async::DefaultStateGraphTypes>>(kBufferVersion, SceneExecutor::Coordinator.lane,
			SceneExecutor::Coordinator.domain)),
		payloadParts(std::make_unique<PayloadPartPosts>()),
		textureGates(std::make_unique<TextureGates>())
	{
		// A window: the file's read and the texture's creation, which block, on the streaming domain. A failed read still completes: its
		// gate opens on a texture that reads zero rather than wait for ever.
		graph->RegisterTypedProducer<TextureWindowInput, TextureWindowBuilt>(kTextureWindow, SceneExecutor::Streaming.lane, SceneExecutor::Streaming.domain, "DCLF texture window",
			[](const Graph::ArtifactBuildContext&, std::shared_ptr<const TextureWindowInput> a_input) {
				a_input->build();
				return Graph::ArtifactBuildResult::Ready(org::async::ArtifactPayload::Make(std::make_shared<const TextureWindowBuilt>()));
			});
		// A gate runs its request when its requirement, the base window, is ready: the wake is the graph's.
		graph->RegisterTypedProducer<TextureGateInput, TextureGateRan>(kTextureGate, SceneExecutor::Preparation.lane, SceneExecutor::Preparation.domain, "DCLF texture gate",
			[gates = textureGates.get()](const Graph::ArtifactBuildContext& a_context, std::shared_ptr<const TextureGateInput> a_input) {
				a_input->run();
				{
					std::scoped_lock lock(gates->mutex);
					gates->ran.push_back(a_context.key);
				}
				return Graph::ArtifactBuildResult::Ready(org::async::ArtifactPayload::Make(std::make_shared<const TextureGateRan>()));
			});
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

	bool SceneGraph::PostTextureWindow(std::uint64_t a_texture, std::uint64_t a_revision, std::function<void()> a_build)
	{
		Graph::ArtifactIntent intent;
		intent.key = Graph::ArtifactKey{ kTextureWindow, a_texture, 0 };
		intent.desiredRevision = a_revision;
		intent.input = org::async::ArtifactPayload::Make(std::make_shared<const TextureWindowInput>(TextureWindowInput{ std::move(a_build) }));
		intent.requestFingerprint = a_revision;
		return static_cast<bool>(graph->PostRequest(std::move(intent), true));
	}

	void SceneGraph::RetireTextureWindow(std::uint64_t a_texture)
	{
		graph->Release(Graph::ArtifactKey{ kTextureWindow, a_texture, 0 });
	}

	bool SceneGraph::PostTextureGate(std::uint64_t a_texture, std::uint64_t a_revision, std::function<void()> a_run)
	{
		std::vector<Graph::ArtifactKey> ran;
		{
			std::scoped_lock lock(textureGates->mutex);
			ran.swap(textureGates->ran);
		}
		if (!ran.empty())
			graph->ReleaseBatch(ran);
		Graph::ArtifactIntent intent;
		intent.key = Graph::ArtifactKey{ kTextureGate, textureGates->ids.fetch_add(1, std::memory_order_relaxed) + 1, 0 };
		intent.desiredRevision = 1;
		intent.requirements.push_back(org::async::ReadyGateAtLeast(Graph::ArtifactKey{ kTextureWindow, a_texture, 0 }, a_revision));
		intent.input = org::async::ArtifactPayload::Make(std::make_shared<const TextureGateInput>(TextureGateInput{ std::move(a_run) }));
		intent.requestFingerprint = intent.key.primaryID;
		return static_cast<bool>(graph->PostRequest(std::move(intent), false));
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
