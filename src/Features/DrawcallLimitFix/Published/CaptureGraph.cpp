#include "CaptureGraph.h"
#include "PublishedSceneExecutor.h"

#include <ORGModuleServices/Async/StateGraph.h>
#include <stdexcept>

namespace DCLF::Published
{
	namespace
	{
		using Graph = org::async::AsyncStateGraph;
		struct GraphBackend
		{
			static org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> Hooks()
			{
				org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> hooks;
				hooks.scheduling = PublishedSceneExecutor::Layout;
				hooks.producerDispatch = { org::async::TaskDispatch::Controlled, org::async::TaskDispatch::Cpu };
				return hooks;
			}
			Graph graph;
			Graph::ArtifactAwaiter waiter;
			GraphBackend(std::shared_ptr<org::async::GraphScheduler> scheduler, CaptureService::Builder builder) :
				graph(std::move(scheduler), "DCLF capture preparation", Hooks())
			{
				graph.RegisterTypedProducer<CaptureBuildRequest, CaptureBuildRequest>(0, 0, 0, "DCLF captured root",
					[](const auto&, auto input) {
						return Graph::ArtifactBuildResult::Ready(org::async::ArtifactPayload::Make(std::move(input)));
					});
				Graph::ArtifactProducerRegistration prepare;
				prepare.domain = PublishedSceneExecutor::Preparation.domain;
				prepare.taskName = "DCLF prepare exact capture";
				prepare.producer = [builder = std::move(builder)](const auto& context) {
					if (context.dependencies.size() != 1) return Graph::ArtifactBuildResult::Failure("missing exact captured root");
					const auto input = context.dependencies[0].payload.template Get<CaptureBuildRequest>();
					if (!input || input->ticket != context.revision) return Graph::ArtifactBuildResult::Failure("capture ticket mismatch");
					auto output = builder(*input);
					if (!output.artifact || output.bytes > input->reservedOutputBytes)
						return Graph::ArtifactBuildResult::Failure("invalid prepared capture output");
					return Graph::ArtifactBuildResult::Ready(org::async::ArtifactPayload::Make(
						std::make_shared<const CaptureService::Output>(std::move(output))));
				};
				graph.RegisterProducer(1, std::move(prepare));
			}
			bool Start(const CaptureBuildRequest& request, CaptureService::Completion done)
			{
				waiter.Reset();
				const auto root = graph.PostRequest({ { 0, 0, 0 }, request.ticket, {},
					org::async::ArtifactPayload::Make(std::make_shared<const CaptureBuildRequest>(request)), request.ticket }, false);
				if (!root) return false;
				const auto prepared = graph.PostRequest({ { 1, 0, 0 }, request.ticket, { org::async::Exact(root.Handle()) } }, false);
				if (!prepared) return false;
				// Both success and terminal failure return through the coordinator lane.
				waiter = graph.AwaitExact(prepared.Handle(), org::async::ArtifactReadiness::CpuReady, 0, 0,
					[this, done](const auto& snapshot) {
						const auto output = snapshot.payload.template Get<CaptureService::Output>();
						graph.Release({ 1, 0, 0 });
						graph.Release({ 0, 0, 0 });
						done(output ? *output : CaptureService::Output{});
					}, [this, done](const auto&) {
						graph.Release({ 1, 0, 0 });
						graph.Release({ 0, 0, 0 });
						done({});
					});
				return true;
			}
		};
	}

	CaptureService::BackendFactory MakeGraphCaptureBackend(CaptureService::Builder builder)
	{
		if (!builder) throw std::invalid_argument("graph capture backend needs a builder");
		return [builder = std::move(builder)](std::shared_ptr<org::async::GraphScheduler> scheduler) {
			auto backend = std::make_shared<GraphBackend>(std::move(scheduler), builder);
			return CaptureService::Backend{
				[backend](const auto& request, auto done) { return backend->Start(request, std::move(done)); },
				[backend] { backend->graph.Shutdown(); }
			};
		};
	}
}
