#pragma once

// The shapes a revision's recording of an epoch is prepared for, as its passes read them.
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	struct ShadowFrame;
	struct ViewFrame;
	/**
	 * @brief The shapes a scene revision's recording of an epoch is prepared for (Revision/Assemble.cpp): what its passes read
	 * from the preparation's host data (PassPrepareContext::preparationData). A
	 * preparation without one draws nothing: every DCLF epoch is submitted from a revision.
	 */
	struct RevisionShapes
	{
		std::array<std::shared_ptr<const PassFrame>, 2> main;  // kDepthShape, kColourShape
		std::shared_ptr<const ShadowFrame> shadow, occlusion;
		// The views' (ViewKind; first person's two epochs record against its one shape).
		std::array<std::shared_ptr<const ViewFrame>, kViews> views;
	};
	inline const RevisionShapes* RevisionShapesOf(const org::PassPrepareContext& a_preparation)
	{
		return a_preparation.preparationData ? a_preparation.preparationData->Get<RevisionShapes>() : nullptr;
	}
	/**
	 * @brief The serial of the version of a_buffer a_preparation's revision names (0: none): what a pass's invocation revision
	 * takes for each versioned buffer it binds.
	 */
	inline std::uint64_t NamedSerial(const org::PassPrepareContext& a_preparation, const Versioned& a_buffer)
	{
		const auto* named = a_preparation.preparationData ? a_preparation.preparationData->Get<NamedVersions>() : nullptr;
		return named && a_buffer && named->Names(*a_buffer) ? named->Of(*a_buffer).serial : 0;
	}
	/**
	 * @brief The units of a_buffer's version a_preparation's revision names (0: none): what its passes size their dispatches and
	 * lists by, for the versions it binds.
	 */
	inline std::uint64_t NamedUnitsOf(const org::PassPrepareContext& a_preparation, const Versioned& a_buffer)
	{
		const auto* named = a_preparation.preparationData ? a_preparation.preparationData->Get<NamedVersions>() : nullptr;
		return named ? NamedUnits(*named, a_buffer) : 0;
	}
	/** @brief The shape a_preparation prepares the segment's passes for: its revision's, null without one or for a segment that does not draw. */
	inline std::shared_ptr<const PassFrame> CurrentFrame(const org::PassPrepareContext& a_preparation, RenderGraphRuntime::Segment a_segment)
	{
		if (const auto* shapes = RevisionShapesOf(a_preparation)) {
			if (a_segment == RenderGraphRuntime::Segment::ZPrepass)
				return shapes->main[kDepthShape];
			if (a_segment == RenderGraphRuntime::Segment::MainOpaque)
				return shapes->main[kColourShape];
		}
		return nullptr;
	}

	static_assert(DrawPipelines::kMaxPipelines == 4096, "BuildDrawsCS.hlsl's kSortKeys");

	/** @brief Whether BuildDraws runs in the segment (BuildDrawsPass::Prepare's conditions), which is when a sort follows it. */
	inline bool BuildsDraws(const org::PassPrepareContext& a_preparation, const Resources& a_resources, RenderGraphRuntime::Segment a_segment)
	{
		const auto frame = a_resources.buildDraws && a_resources.dispatchSignature ? CurrentFrame(a_preparation, a_segment) : nullptr;
		return frame && frame->latch;
	}
}
