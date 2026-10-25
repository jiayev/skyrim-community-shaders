#pragma once

// The CPU-driven pipeline's shadow site (Sites.h, SiteFrame.h): the shadow views (the sun's cascades, the spot and point lights' views)
// and the occlusion maps (the sky occluder, the precipitation occluder), drawn at shadow-maps.after from the shadow epoch's commit
// (IndirectDraws::ExecuteShadowFrame, run in Direct::ExecuteEpoch), which latched them as in the GPU path. Each latched view is culled on
// DCLF's pool from its latch (ShadowCullView, OcclusionCullView) and drawn into its map's slice and viewport with direct draws, after the
// site's pre-work (the shape's latched copies and the index pool's copies, which the frame's other sites' draws read too).

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"

#include <cstdint>
#include <memory>
#include <span>

namespace org::services
{
	class NamedVersions;
}

namespace DCLF::Draws
{
	struct ShadowFrame;
	struct ShadowResources;
}

namespace DCLF::Direct
{
	/** @brief One view the shadow commit latched (LatchShadowViews, LatchOcclusionViews): its list, its slot, its view in the shape, its latch. */
	struct ShadowSiteView
	{
		ViewType type = ViewType::Sun;
		std::uint32_t slot = 0;   // its view slot (the CullView's viewIndex)
		std::uint32_t index = 0;  // into ShadowFrame::views (its target, slice, viewport, push words and map row)
		Draws::BuildDrawsLatch latch{};
	};

	/** @brief The shadow commit's inputs to its site (ExecuteShadowFrame's body, where the GPU path submits its recording). */
	struct ShadowCommitInputs
	{
		std::shared_ptr<const Draws::ShadowFrame> shape;  // the revision's shadow shape the commit wrote against (its full variant)
		std::shared_ptr<Draws::ShadowResources> resources;
		const org::services::NamedVersions* adopted = nullptr;  // the frame's adopted set (held by the execution's owner)
		std::span<const ShadowSiteView> views;
		std::span<const Draws::SunEntryProcess> sunEntry;  // the sun's full-frustum processes the commit latched (clamped)
		std::uint32_t latchSlot = 0;                       // the latch regions it wrote (the external frame slot)
	};

	/**
	 * @brief Render thread, inside the shadow epoch's commit (CPU mode): the shadow site: its pre-work, a view of its list per latched view
	 * whose map the shadow resources import, culled once the frame's cull frame is in; its callback queued after the commit (ExecuteEpoch).
	 */
	void ShadowCommitted(const ShadowCommitInputs& a_inputs);

	/**
	 * @brief Render thread: whether this CPU frame's shadow site ran (its commit latched the views and its pre-work copies their blocks), so
	 * the frame's local shadow records name views DCLF drew (LocalShadowMasks.cpp's CPU-mode resolve).
	 */
	bool ShadowSiteRan();
}
