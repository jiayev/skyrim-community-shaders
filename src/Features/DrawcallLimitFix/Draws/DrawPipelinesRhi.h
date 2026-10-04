#pragma once

// BasicRHI view of DrawPipelines, for the graph pass (include after rhi_interop_vulkan.h).
#include <array>
#include <memory>
#include <vector>
#include <rhi.h>

#include "DrawPipelines.h"

namespace DCLF
{
	/** @brief What an ExecuteIndirect of DCLF's draws binds; invalid until the first pipeline is in the set. */
	struct IndirectState
	{
		rhi::PipelineLayoutHandle layout{};
		std::array<rhi::IndirectPipelineSetHandle, kVariantCount> sets{};  // by variant (DrawPipelines.h)
		std::array<rhi::CommandSignatureHandle, kVariantCount> signatures{};
		// The Z-prepass's plain indirect draws (MainOpaquePass): every published pipeline's group (the pipelines that build the
		// same pulled depth pipeline, DrawPipelines.cpp ZPipelineKey) by its index, each group's pulled depth pipeline, their
		// layout and the signature of a DrawSequence's tail as a plain draw.
		std::vector<std::uint32_t> zGroups;
		std::vector<rhi::PipelineHandle> zPipelines;
		rhi::PipelineLayoutHandle zLayout{};
		rhi::CommandSignatureHandle zDrawSignature{};
		// The set version these handles belong to (DrawPipelines.cpp, SetVersion): while any copy of this state lives, the
		// version's sets are not written, so hold it for as long as the handles are recorded with.
		std::shared_ptr<const void> version;
		bool valid = false;
	};

	IndirectState GetIndirectState();

	/** @brief The shadow views' pipelines (DrawPipelines::FindShadow), valid once one is published. */
	struct ShadowIndirectState
	{
		rhi::PipelineLayoutHandle layout{};
		// The shadow views' plain indirect draws (ShadowViewPass): every published pipeline by its index, its geometry's vertex
		// layout (ShadowPipelineKey::vertexLayout, which the pulling vertex stage decodes) and its class (DrawPipelines::
		// ShadowDiscards), and the signature of a DrawSequence's tail as a plain draw.
		std::vector<rhi::PipelineHandle> pipelines;
		std::vector<std::uint64_t> vertexLayouts;
		std::vector<std::uint8_t> discards;
		rhi::CommandSignatureHandle drawSignature{};
		std::shared_ptr<const void> version;  // as IndirectState::version
		bool valid = false;
	};

	ShadowIndirectState GetShadowIndirectState();

	/** @brief Tree LOD's pipelines and its draw's signature (DrawPipelines::FindTreeLod). */
	struct TreeLodPipelines
	{
		rhi::PipelineHandle depth{}, colour{};
		rhi::CommandSignatureHandle drawSignature{};  // one non-indexed DrawInstanced's arguments (16 bytes)
	};
}
