#pragma once

#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <array>
#include <memory>
#include <string>

namespace DCLF::Api
{
	class SceneResourcesSetup;
}
namespace DCLF::Draws
{
	struct Resources;
	struct SceneBuffers;
}
namespace org
{
	class RenderGraph;
	class RenderPass;
}
namespace DCLF::Features::Portals
{
	/** @brief Scene-owned scratch. All query-indexed buffers grow as one unit group with their producer's rows. */
	struct Batch
	{
		std::string name;
		Draws::Versioned queries, results, ranks, indices, tasks, counts, offsets;
		std::shared_ptr<org::Buffer> dispatch;
		std::array<std::shared_ptr<const ComputeProgram>, 9> programs;  // Clear .. Evaluate, then Inline
		rhi::CommandSignaturePtr signature;
		// Inline (the default; CS_DCLF_PORTAL_EVALUATOR=batched: off): no count, scan or scatter; one Inline dispatch over the queries.
		bool inlineEvaluation = false;
		std::uint64_t reportedPrograms = 0, reportedBuckets = 0;  // render-thread capacity diagnostics only
	};
	/** @brief Create the portable count/scan/scatter/task/evaluate pipeline; no partial pipeline is published. */
	std::shared_ptr<Batch> CreateBatch(const Api::SceneResourcesSetup& a_setup, std::string a_name = "cs.dclf.portal", bool a_contextAtlas = false);
	/** @brief Register a batch under the same unique prefix used for its resource names. */
	void RegisterBatch(org::RenderGraph& a_graph, const Batch& a_batch);
	/**
	 * @brief Shared count/scan/scatter or evaluation pass over its owner's query storage; the programs are the depth latch's publication.
	 * An inline batch has no count/scan/scatter pass (null) and evaluates in one dispatch.
	 */
	std::shared_ptr<org::RenderPass> MakeBatchPass(std::shared_ptr<Draws::Resources> a_resources, std::shared_ptr<const Batch> a_batch,
		RenderGraphRuntime::Segment a_segment, bool a_evaluate);
	/** @brief A batch's owner: its batch in a scene (null: none). */
	using BatchOf = const Batch* (*)(const Draws::SceneBuffers&);
	/** @brief Install: a batch whose queries CS_DCLF_PORTAL_CAPTURE writes (Diagnostics/PortalCapture.cpp), under a_label. */
	void RegisterBatchCapture(std::string a_label, BatchOf a_batchOf);
	/** @brief Render-thread diagnostic for a main-camera snapshot that outgrew its adopted bucket scratch (evaluated unbatched). */
	void CheckMainBatchCapacity(Batch& a_batch, std::uint32_t a_frame, std::uint64_t a_capacity);
}
