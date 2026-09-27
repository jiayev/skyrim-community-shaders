#pragma once

// BasicRHI view of DrawPipelines, for the graph pass (include after rhi_interop_vulkan.h).
#include <array>
#include <memory>
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
		// The depth pass's: the depth variant's signature, preprocessed implicitly even with CS_DCLF_DGC_PREPROCESS. It is one
		// call over sequences written just before it, which an explicit preprocess only serialises (+17 us at Riverwood).
		rhi::CommandSignatureHandle depthPassSignature{};
		// The set version these handles belong to (DrawPipelines.cpp, SetVersion): while any copy of this state lives, the
		// version's sets are not written, so hold it for as long as the handles are recorded with.
		std::shared_ptr<const void> version;
		bool valid = false;
	};

	IndirectState GetIndirectState();

	/** @brief The shadow views' pipeline set and its command signature (DrawPipelines::FindShadow). */
	struct ShadowIndirectState
	{
		rhi::PipelineLayoutHandle layout{};
		rhi::IndirectPipelineSetHandle set{};
		rhi::CommandSignatureHandle signature{};
		std::shared_ptr<const void> version;  // as IndirectState::version
		bool valid = false;
	};

	ShadowIndirectState GetShadowIndirectState();
}
