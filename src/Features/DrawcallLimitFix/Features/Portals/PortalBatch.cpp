#include "Features/DrawcallLimitFix/Features/Portals/PortalBatch.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalContext.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

namespace DCLF::Features::Portals
{
	void CheckMainBatchCapacity(Batch& a_batch, std::uint32_t a_frame, std::uint64_t a_capacity)
	{
		if (a_batch.inlineEvaluation)
			return;  // no buckets: every query is evaluated against its own program
		const auto& programs = PortalViews::Get().Encoded();
		const auto count = programs.size() > kPortalSetBytes / 4 ? programs[kPortalSetBytes / 4] : 0u;
		if (count > a_capacity) {
			if (a_batch.reportedPrograms != count || a_batch.reportedBuckets != a_capacity)
				logger::warn("[DCLF] {} programs exceed publication buckets until growth is adopted; the excess is evaluated unbatched: programs={}, capacity={}, frame={}", a_batch.name, count, a_capacity, a_frame);
			a_batch.reportedPrograms = count;
			a_batch.reportedBuckets = a_capacity;
		} else if (a_batch.reportedPrograms) {
			logger::info("[DCLF] {} publication bucket gap resolved at frame {}", a_batch.name, a_frame);
			a_batch.reportedPrograms = a_batch.reportedBuckets = 0;
		}
	}

	void RegisterBatch(org::RenderGraph& a_graph, const Batch& a_batch)
	{
		const auto add = [&](const char* suffix, const Draws::Versioned& buffer) {
			Draws::Register(a_graph, org::ResourceIdentifier(a_batch.name + suffix), buffer);
		};
		add("-queries", a_batch.queries);
		add("-results", a_batch.results);
		add("-ranks", a_batch.ranks);
		add("-indices", a_batch.indices);
		add("-tasks", a_batch.tasks);
		add("-counts", a_batch.counts);
		add("-offsets", a_batch.offsets);
		Draws::Register(a_graph, org::ResourceIdentifier(a_batch.name + "-dispatch"), a_batch.dispatch);
	}

	std::shared_ptr<Batch> CreateBatch(const Api::SceneResourcesSetup& a_setup, std::string a_name, bool a_contextAtlas)
	{
		auto batch = std::make_shared<Batch>();
		batch->name = std::move(a_name);
		const wchar_t* entries[]{ L"Clear", L"Count", L"TaskCounts", L"Scan", L"AddOffsets", L"Scatter", L"MakeTasks", L"Evaluate", L"Inline" };
		// Inline by default: the portal bench (tests/DclfPortalBench) measured the batch's count/scan/scatter dispatches at ~25 us a batch
		// over the evaluation itself, on every captured and synthetic corpus (dclf-portal-query-batches.md, "Portal culling bench").
		const auto& evaluator = RegisteredSwitchValue("CS_DCLF_PORTAL_EVALUATOR");
		batch->inlineEvaluation = evaluator != "batched";
		if (!evaluator.empty() && evaluator != "inline" && evaluator != "batched")
			logger::warn("[DCLF] CS_DCLF_PORTAL_EVALUATOR '{}' is neither batched nor inline; {} is inline", evaluator, batch->name);
		for (std::size_t i = 0; i < batch->programs.size(); ++i) {
			ComputeProgram::Desc desc{ .source = "DrawcallLimitFix/PortalBatchCS.hlsl", .entry = entries[i], .constantWords = 20 };
			// Count evaluates past-capacity queries itself, so it addresses programs as Evaluate does.
			if ((i == 1 || i == 7 || i == 8) && a_contextAtlas)
				desc.defines.emplace_back(L"DCLF_PORTAL_CONTEXT_ATLAS", L"1");
			if ((i == 7 || i == 8) && RegisteredSwitchValue("CS_DCLF_PORTAL_UNIFORM_SCAN") == "1")
				desc.defines.emplace_back(L"DCLF_PORTAL_UNIFORM_SCAN", L"1");
			batch->programs[i] = ComputeProgram::Load(a_setup.device, desc);
			if (!batch->programs[i])
				return nullptr;
		}
		batch->signature = Draws::CreateDispatchSignature(a_setup.device, batch->programs[7]->layout->GetHandle());
		if (!batch->signature)
			return nullptr;
		const auto buffer = [&](std::uint32_t stride, const char* suffix, std::uint32_t count) {
			return Draws::MakeVersioned(a_setup.StructuredBuffer(count, stride, (batch->name + suffix).c_str(), true), { .extent = { 0, stride, stride } });
		};
		const auto roots = a_setup.smallStart ? 4u : 4096u;
		batch->queries = buffer(sizeof(DCLF::Portals::Query), "-queries", roots);
		batch->results = buffer(sizeof(DCLF::Portals::Result), "-results", roots);
		batch->ranks = buffer(4, "-ranks", roots);
		batch->indices = buffer(4, "-indices", roots);
		batch->tasks = buffer(sizeof(DCLF::Portals::Task), "-tasks", roots);
		batch->counts = buffer(8, "-counts", 8);
		batch->offsets = buffer(8, "-offsets", 8);
		batch->dispatch = a_setup.StructuredBuffer(4, 4, (batch->name + "-dispatch").c_str(), true);
		return batch;
	}
}
