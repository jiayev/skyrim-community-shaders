#pragma once

// What the passes and the graph extensions share: recording helpers, registration (the ones features use too: Register and
// RecordDispatch, Core/Extensions/PassContext.h's; the draw passes' attachments, frame buffers, preprocessed and bucket draws, the latched
// copies pass, the latched dispatch and the shadow epoch's index pool copies, Passes/DrawHelpers.h's). Part of IndirectDraws'
// implementation: included by Internal.h, in its order.

#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"

namespace DCLF::Draws
{
	/**
	 * @brief Folds the camera translation into the view-projection, so that a bound's absolute world
	 * position can be projected without carrying the camera separately.
	 *
	 * The draws are camera-relative (the engine's posAdjust), and a bound is absolute: the camera is subtracted
	 * here, one matrix column, instead of from every bound, which is exact and keeps the camera out of a
	 * push-constant block at Vulkan's guaranteed 128 bytes.
	 *
	 * Row-major, as the shader's `mul(M, v)` reads it: M'(p,1) = M(p-e,1) needs only column 3 changed,
	 * by subtracting M's upper-left 3x3 applied to the camera.
	 */
	void FoldEyeIntoViewProj(const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye, float (&a_out)[16]);

	// MainPayload::objectState values besides the Skip reasons.
	constexpr std::uint8_t kObjectStateDrawable = 0xF0;
	constexpr std::uint8_t kObjectStateDecal = 0xF1;
	constexpr std::uint8_t kObjectStateAbsent = 0xFF;

	// A DXVK image the graph uses in place: imported without ownership, with simultaneous access (DXVK
	// keeps the images it hands out in GENERAL and uses them between the graph's commands).
	std::shared_ptr<org::ExternalTextureResource> ImportImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, org::TextureDescription a_desc,
		const char* a_name);

	/**
	 * @brief The index pool every plain indexed draw binds (IndexPool): the scene's (SceneBuffers::pool), made with its
	 * copy program by whichever extension sets up first. Null, logged, when the program cannot be created.
	 */
	std::shared_ptr<IndexPool> EnsureIndexPool(SceneBuffers& a_scene, rhi::Device a_device, bool a_small);

	/**
	 * @brief Brings the index pool up to the tables in the versions a_sealed (the adopted set) names (ShadowIndexPool.cpp): whichever
	 * commit draws from it first in a frame gives the ranges out and writes their copies' dispatch at a_poolOffset of its latch slot;
	 * a later commit finds nothing to copy.
	 */
	void UpdateIndexPool(const NamedVersions& a_sealed, IndexPool& a_pool, const SceneTables& a_tables, std::uint32_t a_generation, const org::LatchBlock& a_latch,
		std::uint32_t a_latchSlot, std::uint32_t a_poolOffset, CommitUploads& a_uploads);

	// The graph extensions that add DCLF's passes (Passes/MainExtension.cpp, Passes/Shadow.cpp).
	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeMainOpaqueExtension(std::shared_ptr<Resources> a_resources);
	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeShadowExtension(std::shared_ptr<ShadowResources> a_resources);

	/** @brief The scene tables, which every extension registers (the same identifiers: a second registration is an update). */
	void RegisterSceneBuffers(org::RenderGraph& a_graph, const SceneBuffers& a_scene);

	/**
	 * @brief A pass of an extension: a copy or compute pass on the graphics queue unless a_queue says otherwise, a render pass (the pass
	 * contract's: Api/Passes.h). Only the schedule's executor (Epochs/ScheduleExecutor.cpp) calls AddPass.
	 */
	using PassKind = Api::PassKind;
	using PassQueue = Api::PassQueue;
	template <class Epoch>
	void AddPass(std::vector<org::RenderGraph::ExternalPassDesc>& a_out, PassKind a_kind, std::string a_name, std::shared_ptr<org::RenderPass> a_pass, Epoch a_epoch,
		PassQueue a_queue = PassQueue::Default)
	{
		using Desc = org::RenderGraph::ExternalPassDesc;
		auto desc = a_kind == PassKind::Copy ? Desc::Copy(std::move(a_name), std::move(a_pass)) :
		            a_kind == PassKind::Compute ? Desc::Compute(std::move(a_name), std::move(a_pass)) :
		                                          Desc::Render(std::move(a_name), std::move(a_pass));
		// An external pass's queue is its description's (ORG reads no builder preference for it), so CS_DCLF_ASYNC_COMPUTE=0 is applied here.
		if (a_queue == PassQueue::Compute)
			desc.PreferQueue(AsyncComputeQueue());
		else if (a_queue == PassQueue::Default && a_kind != PassKind::Render)
			desc.PreferQueue(org::QueueKind::Graphics);
		a_out.push_back(std::move(desc.Epoch(a_epoch)));
	}

	/**
	 * @brief A forward range's sort, back to front (ForwardSortCS.hlsl), as of the shape its preparation is for: per state class with a
	 * forward sub-range (GpuLayouts.h, kClassRangeForward), its staging half's first slot and capacity (each half's) and its count word's
	 * byte offset, a dispatch each. No program: nothing to sort.
	 */
	struct ForwardSortSource
	{
		struct Range
		{
			std::uint32_t stagingBase = 0, capacity = 0, countOffset = 0;
			std::uint32_t options = 0;  // added to the range's kCountOffset (ForwardSortConstants::kRankInstance)
		};
		std::uint64_t generation = 0;
		std::shared_ptr<const ComputeProgram> program;
		std::vector<Range> ranges;
	};
	/** @brief The sort of a forward range of a_sequences (the main one, a view's), its classes' counts read from a_counts. */
	std::shared_ptr<org::RenderPass> MakeForwardSortPass(Versioned a_sequences, LatchedTarget a_counts, std::function<ForwardSortSource(const org::PassPrepareContext&)> a_source);

	/** @brief The rows' strides into BuildDraws' constants (BuildDrawsCS: RowsOf; the addresses are the latch's: PayloadLatch). */
	inline void SetRowStrides(BuildDrawsConstants& a_constants, bool a_pipelineRows = true)
	{
		a_constants.materialRowStride = kMaterialRowBytes;
		a_constants.pipelineRowStride = a_pipelineRows ? kPipelineRowBytes : 0u;
	}
	/** @brief The installed payload's main parts into a culling latch: the scene list, the geometry table, the main rows. */
	inline void PayloadLatch(const PayloadVersions& a_payload, BuildDrawsLatch& a_latch)
	{
		const std::uint64_t materialRows = a_payload[PayloadPart::MaterialRows].address, pipelineRows = a_payload[PayloadPart::PipelineRows].address;
		a_latch.inputsIndex = a_payload[PayloadPart::Inputs].srvIndex;
		a_latch.geometriesIndex = a_payload[PayloadPart::Geometries].srvIndex;
		a_latch.materialRowsLo = static_cast<std::uint32_t>(materialRows);
		a_latch.materialRowsHi = static_cast<std::uint32_t>(materialRows >> 32);
		a_latch.pipelineRowsLo = static_cast<std::uint32_t>(pipelineRows);
		a_latch.pipelineRowsHi = static_cast<std::uint32_t>(pipelineRows >> 32);
	}
	/** @brief The installed payload's shadow parts into a shadow or occlusion view's latch: the scene list, the geometry table, the shadow rows. */
	inline void ShadowPayloadLatch(const PayloadVersions& a_payload, BuildDrawsLatch& a_latch)
	{
		const std::uint64_t shadowRows = a_payload[PayloadPart::ShadowRows].address;
		a_latch.inputsIndex = a_payload[PayloadPart::Inputs].srvIndex;
		a_latch.geometriesIndex = a_payload[PayloadPart::Geometries].srvIndex;
		a_latch.materialRowsLo = static_cast<std::uint32_t>(shadowRows);
		a_latch.materialRowsHi = static_cast<std::uint32_t>(shadowRows >> 32);
		a_latch.pipelineRowsLo = a_latch.pipelineRowsHi = 0;
	}
	/**
	 * @brief A bucket map into a latch slot at a_offset: a_map, then kNoBucket for the pipeline slots past it up to a_slots (gained since
	 * the map was made: none of their draws is the frame's).
	 */
	inline void WriteBucketMap(const org::LatchBlock& a_latch, std::string_view a_what, std::uint32_t a_slot, std::uint32_t a_offset, std::span<const std::uint32_t> a_map,
		std::uint32_t a_slots)
	{
		if (!a_map.empty())
			LatchWrite(a_latch, a_what, a_slot, a_offset, std::as_bytes(a_map));
		if (a_slots <= a_map.size())
			return;
		static thread_local std::vector<std::uint32_t> none;
		none.assign(a_slots - a_map.size(), kNoBucket);
		LatchWrite(a_latch, a_what, a_slot, a_offset + static_cast<std::uint32_t>(a_map.size() * sizeof(std::uint32_t)), std::as_bytes(std::span(none)));
	}
}
