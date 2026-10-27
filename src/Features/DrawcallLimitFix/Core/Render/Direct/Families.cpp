// The CPU-driven pipeline's draw families (SiteFrame.h): tree LOD's and grass's draws at the depth and colour sites.
//
// The GPU path draws a family in the main pass's executions (MainOpaquePass: its depth draw after the Z-prepass's buckets, its colour
// draw after the main colour draws, before the second pass), with its main.declare's arguments and draw row. Here the depth run resolves
// them once a frame (the family's main.direct, from the depth commit's adopted set) with the slot's pipelines, and the
// colour run draws what the depth run resolved: its colour tests EQUAL against that depth draw, and its arguments are what the depth
// site's epoch's passes wrote (the families' single-phase culls and grass's arguments, before its draws: GraphConditions::cpuPipeline).

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteFrame.h"

#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <array>
#include <memory>

namespace DCLF::Direct::detail
{
	namespace
	{
		/** @brief A frame's family draws (FrameSlot::familyState): by family, what the depth and colour runs' bindings name. */
		struct FamilyFrame
		{
			std::array<FamilyDraw, Extensions::kDrawFamilies> draws{};
		};

		/**
		 * @brief Render thread, the depth run's: each family DCLF draws this frame (IndirectDraws::DrawFamilyOwned, set up, its pipelines
		 * published) with its row and arguments resolved; the arguments buffer held by the slot. Null when none is drawn.
		 */
		std::shared_ptr<FamilyFrame> ResolveFamilies(FrameSlot& a_slot, const MainCommitInputs& a_inputs)
		{
			if (!a_inputs.adopted || !a_inputs.resources || !a_inputs.resources->scene)
				return nullptr;
			const auto& scene = *a_inputs.resources->scene;
			const auto& draws = IndirectDraws::Get();
			std::shared_ptr<FamilyFrame> frame;
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f) {
				const auto family = static_cast<Api::DrawFamily>(f);
				const auto resolve = Extensions::DrawFamilies()[f].contribution.main.direct;
				if (!resolve || !draws.DrawFamilyOwned(family) || !Draws::FamilySetUp(scene, family))
					continue;
				auto pipelines = Draws::FamilyPipelinesOf(scene, family);
				Api::DrawFamilyDirect direct{ scene, *a_inputs.adopted };
				if (!pipelines || !resolve(direct) || !direct.arguments)
					continue;
				if (!frame)
					frame = std::make_shared<FamilyFrame>();
				auto& draw = frame->draws[f];
				draw.pipelines = std::move(pipelines);
				draw.row = direct.row;
				draw.arguments = direct.arguments->GetAPIResource().GetHandle();
				a_slot.holds.push_back(std::move(direct.arguments));
			}
			return frame;
		}

		/** @brief A rendering of the main targets for the families' draws (as Sites.cpp's main passes): a_colours or depth only, all loaded. */
		struct FamilyPass
		{
			std::array<rhi::ColorAttachment, Draws::kColorTargets> colours{};
			rhi::DepthAttachment depth{};
			rhi::PassBeginInfo begin{};

			FamilyPass(const SiteRun& a_run, bool a_colours, const char* a_name)
			{
				const auto& targets = *a_run.targets;
				const auto& shape = *a_run.shape;
				for (std::uint32_t i = 0; a_colours && i < targets.count; ++i) {
					colours[i].rtv = targets.colours[i];
					colours[i].loadOp = rhi::LoadOp::Load;
					colours[i].storeOp = rhi::StoreOp::Store;
					colours[i].resource = targets.colourResources[i];
				}
				depth.dsv = targets.depth;
				depth.depthLoad = rhi::LoadOp::Load;
				depth.depthStore = rhi::StoreOp::Store;
				depth.stencilLoad = rhi::LoadOp::Load;
				depth.stencilStore = rhi::StoreOp::Store;
				begin.colors = { colours.data(), a_colours ? targets.count : 0u };
				begin.depth = &depth;
				begin.width = shape.width;
				begin.height = shape.height;
				begin.minDepth = shape.minDepth;
				begin.maxDepth = shape.maxDepth;
				begin.contents = rhi::PassContents::Inline;
				begin.debugName = a_name;
			}
			FamilyPass(const FamilyPass&) = delete;
			FamilyPass& operator=(const FamilyPass&) = delete;
		};
	}

	void PrepareFamilies(FrameSlot& a_slot, SiteRun& a_run, const MainCommitInputs& a_inputs)
	{
		// The depth run resolves the frame's; the colour run draws the same ones (none when the depth site did not run this frame: their
		// culls did not either, and their colour has no depth to test against).
		if (a_run.site == Site::Depth)
			a_slot.familyState = ResolveFamilies(a_slot, a_inputs);
		if (const auto* frame = static_cast<const FamilyFrame*>(a_slot.familyState.get()))
			a_run.bindings.families = frame->draws;
	}

	void RecordFamilies(SiteOut& a_out, SiteRun& a_run, bool a_depthPass)
	{
		const auto& families = a_run.bindings.families;
		if (!a_run.slot->set || std::none_of(families.begin(), families.end(), [](const FamilyDraw& a_draw) { return a_draw.pipelines != nullptr; }))
			return;
		auto& slot = *a_run.slot;
		auto& list = a_out.Inline();
		FamilyPass pass(a_run, !a_depthPass, a_depthPass ? "DCLF direct families depth" : "DCLF direct families colour");
		list.BeginPass(pass.begin);
		// The main view's pass (its depth or colour: the Z-prepass's layout, the frame push words, the heaps), then the families' draws.
		RecordContext context{ list, *slot.set, ViewType::Main, 0, a_run.site, a_depthPass, a_run.bindings, &slot.counters };
		if (BeginList(context))
			RecordFamilyDraws(context);
		list.EndPass();
	}
}
