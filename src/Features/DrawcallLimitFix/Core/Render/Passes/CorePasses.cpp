// Core's passes (Api::PassDef), which the schedule (Schedule/Frame.cpp) names by key: the main view's (core.*, main-view.*, forward.*),
// the anchors (stages.*, families.*, diag.*), the depth pyramid's (gpu-occlusion.hzb), and through Shadow.cpp and ViewEpochRecord.cpp
// the shadow views' (shadows.*) and the views' (view.*). Each make is its pass's condition and factory; the executor adds the pass.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/IndexPoolPass.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Core/Extensions/GpuStages.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Draws
{
	namespace
	{
		/** @brief The queue a stage's pass asks for (Api::GpuQueue: Graphics is a copy's or compute pass's default). */
		PassQueue QueueOf(Api::GpuQueue a_queue)
		{
			return a_queue == Api::GpuQueue::Compute ? PassQueue::Compute : a_queue == Api::GpuQueue::None ? PassQueue::None : PassQueue::Default;
		}

		/**
		 * @brief stages.*: the features' stages at Slot (Api::GpuStage), in registration order: each whose setup gave a state, as its kind,
		 * on its queue. The phase 2 stages only with the HZB (the two-phase tail's).
		 */
		template <Api::GpuStageSlot Slot>
		void EmitStages(const Api::PassContext& a_context, Api::PassOut& a_out)
		{
			const auto& resources = a_context.Main();
			if constexpr (Slot == Api::GpuStageSlot::DepthSegmentPhaseTwoBeforeDraw)
				if (!resources->hzb)
					return;
			const auto stages = Extensions::RegisteredStages();
			const auto& states = resources->scene->stages;
			for (std::size_t i = 0; i < stages.size() && i < states.size(); ++i)
				if (stages[i].slot == Slot && states[i])
					a_out.Add(stages[i].passName, stages[i].kind == Api::GpuStageKind::Copy ? PassKind::Copy : PassKind::Compute,
						stages[i].makePass({ resources, states[i] }), QueueOf(stages[i].queue));
		}

		/** @brief diag.*: the observers' passes at Point (Diagnostics::MainGraphPasses), appended in the epoch's place. */
		template <Diagnostics::MainGraphPasses::Point Point>
		void EmitDiagnostics(const Api::PassContext& a_context, Api::PassOut& a_out)
		{
			Diagnostics::Notify(Diagnostics::MainGraphPasses{ Point, a_context.Main(), a_context.epoch, a_out.Descs() });
		}

		// Each epoch's first pass copies the commit's latched values. It publishes its declared targets (Resources::latchedTargets),
		// so a commit latches only what the pass will copy. A pass's segment is fixed, so each segment has its own instances.
		std::shared_ptr<org::RenderPass> LatchedCopies(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return MakeLatchedCopiesPass([resources = resources] { return MainLatchedTargets(*resources); },
				[segment = a_context.segment](const org::PassPrepareContext& a_preparation) {
					const auto frame = CurrentFrame(a_preparation, segment);
					return frame ? LatchedSource{ frame->generation, frame, std::span(&frame->latched, 1) } : LatchedSource{};
				},
				&resources->latchedTargets);
		}

		std::shared_ptr<org::RenderPass> BuildDraws(const Api::PassContext& a_context)
		{
			return MakeBuildDrawsPass(a_context.Main(), a_context.segment);
		}

		// The pool copies the depth commit gave out (when the shadow epoch has not), before the draws read them.
		std::shared_ptr<org::RenderPass> IndexPoolCopies(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return std::static_pointer_cast<org::RenderPass>(std::make_shared<IndexPoolPass>(resources->pool, [main = resources](const org::PassPrepareContext& a_preparation) {
				const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
				return IndexPoolPass::LatchOf{ frame ? frame->generation : 0, frame ? frame->latch : nullptr, frame ? frame->latchLayout.PoolOffset() : 0u };
			}));
		}

		std::shared_ptr<org::RenderPass> MainDraws(const Api::PassContext& a_context)
		{
			return MakeMainOpaquePass(a_context.Main(), a_context.segment);
		}

		// The two-phase tail, in the depth segment, in this order: build-draws (phase 1, last frame's HZB), depth draw, hzb (rebuilt from
		// it by the depth pyramid's producer, Api/DepthPyramid.h), build-draws-phase2 and the phase 2 stages
		// (Api::GpuStageSlot::DepthSegmentPhaseTwoBeforeDraw: tree-lod-cull-phase2; phase 1's rejects retested), depth-phase2 (the
		// rescues). Phase 1's frame-old HZB misses what just came out from behind an occluder; phase 2 takes those back. Each only while
		// the HZB exists.
		void EmitDepthPyramid(const Api::PassContext& a_context, Api::PassOut& a_out)
		{
			const auto& resources = a_context.Main();
			if (!resources->hzb)
				return;
			const auto& pyramid = *Extensions::DepthPyramidOf();
			a_out.Add(pyramid.passName, PassKind::Compute, pyramid.makePass({ resources, resources->hzbState }));
		}

		std::shared_ptr<org::RenderPass> BuildDrawsPhaseTwo(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->hzb ? MakeBuildDrawsPass(resources, a_context.segment, 2) : nullptr;
		}

		std::shared_ptr<org::RenderPass> DepthDrawPhaseTwo(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->hzb ? MakeMainOpaquePass(resources, a_context.segment, true) : nullptr;
		}

		// Visibility changes once both phases' verdicts are final, and their readback copy.
		bool VisibilityEventsReady(const Resources& a_resources)
		{
			const auto& sceneBuffers = *a_resources.scene;
			return sceneBuffers.visibilityEventsProgram && sceneBuffers.visibilityReadback && Extensions::CullBindingBuffer(Api::CullBindingSlot::RootRows, sceneBuffers) &&
			       a_resources.frustum;
		}

		std::shared_ptr<org::RenderPass> VisibilityEvents(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return VisibilityEventsReady(*resources) ? MakeVisibilityEventsPass(resources) : nullptr;
		}

		std::shared_ptr<org::RenderPass> VisibilityEventsCopy(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return VisibilityEventsReady(*resources) ? MakeEventReadbackPass(resources->scene, resources->scene->visibilityEvents, resources->scene->visibilityReadback) : nullptr;
		}

		// The sort by pipeline (DrawSort): the scan runs in every execution (so the counts it clears are zero even without a build), the
		// scatter only after a build.
		std::shared_ptr<org::RenderPass> SortScan(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->sort ? resources->sort->ScanPass() : nullptr;
		}

		std::shared_ptr<org::RenderPass> SortScatter(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->sort ? MakeSortSequencesPass(resources, a_context.segment) : nullptr;
		}

		// The forward sort after the colour build; its draw in the forward epoch (IndirectDraws::ExecuteForward).
		std::shared_ptr<org::RenderPass> ForwardSort(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			if (!resources->forwardSort)
				return nullptr;
			return MakeForwardSortPass(resources->sequences, { resources->count, nullptr }, [main = resources](const org::PassPrepareContext& a_preparation) {
				const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
				if (!frame || !BuildsDraws(a_preparation, *main, RenderGraphRuntime::Segment::MainOpaque))
					return ForwardSortSource{ frame ? frame->generation : 0 };
				return ForwardSortSource{ frame->generation, main->forwardSort, static_cast<std::uint32_t>(ForwardStagingSlot(*frame)), frame->forwardCapacity };
			});
		}

		std::shared_ptr<org::RenderPass> ForwardDraw(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->forwardSort ? MakeForwardDrawPass(resources) : nullptr;
		}

		// Refraction normals, in the forward epoch after the blended draws.
		std::shared_ptr<org::RenderPass> RefractionDraw(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->refractionNormals && resources->nativeDepth && resources->pool ? MakeRefractionDrawPass(resources) : nullptr;
		}

		/** @brief families.faces-cull: a trees view's draw families' culls (tree LOD's, phase 1 per face), each its cullName. */
		void EmitFacesCulls(const Api::PassContext& a_context, Api::PassOut& a_out)
		{
			const auto& resources = a_context.View();
			const auto& traits = *resources->traits;
			for (std::size_t d = 0; d < Extensions::kDrawFamilies && traits.trees; ++d)
				if (const auto& entry = Extensions::DrawFamilies()[d]; resources->faceLists[d][0] && entry.contribution.faces.makeCull)
					if (auto cull = entry.contribution.faces.makeCull({ resources }))
						a_out.Add(fmt::format("{}.{}", traits.prefix, entry.cullName), PassKind::Compute, std::move(cull));
		}

		using DiagPoint = Diagnostics::MainGraphPasses::Point;
		using StageSlot = Api::GpuStageSlot;

		constexpr Api::PassDef kCorePasses[] = {
			// The main view's (the main extension's resources).
			{ .key = "core.z.latched-copies", .passName = "cs.dclf.z.latched-copies", .kind = PassKind::Copy, .make = &LatchedCopies },
			{ .key = "core.latched-copies", .passName = "cs.dclf.latched-copies", .kind = PassKind::Copy, .make = &LatchedCopies },
			{ .key = "core.z.build-draws", .passName = "cs.dclf.z.build-draws", .make = &BuildDraws },
			{ .key = "core.build-draws", .passName = "cs.dclf.build-draws", .make = &BuildDraws },
			{ .key = "core.z.index-pool", .passName = "cs.dclf.z.index-pool", .make = &IndexPoolCopies },
			{ .key = "core.build-draws-phase2", .passName = "cs.dclf.build-draws-phase2", .make = &BuildDrawsPhaseTwo },
			{ .key = "core.z.visibility-events", .passName = "cs.dclf.z.visibility-events", .make = &VisibilityEvents },
			{ .key = "core.z.visibility-readback", .passName = "cs.dclf.z.visibility-readback", .kind = PassKind::Copy, .queue = PassQueue::None, .make = &VisibilityEventsCopy },
			{ .key = "core.sort-scan", .passName = "cs.dclf.sort-scan", .make = &SortScan },
			{ .key = "core.sort-scatter", .passName = "cs.dclf.sort-scatter", .make = &SortScatter },
			{ .key = "core.forward-sort", .passName = "cs.dclf.forward-sort", .make = &ForwardSort },
			{ .key = "main-view.z.depth", .passName = "cs.dclf.z.depth", .kind = PassKind::Render, .make = &MainDraws },
			{ .key = "main-view.depth-phase2", .passName = "cs.dclf.depth-phase2", .kind = PassKind::Render, .make = &DepthDrawPhaseTwo },
			{ .key = "main-view.colour", .passName = "cs.dclf.main-opaque", .kind = PassKind::Render, .make = &MainDraws },
			{ .key = "forward.draw", .passName = "cs.dclf.forward", .kind = PassKind::Render, .make = &ForwardDraw },
			{ .key = "forward.refraction", .passName = "cs.dclf.refraction", .kind = PassKind::Render, .make = &RefractionDraw },
			// The depth pyramid's pass, under its producer's name.
			{ .key = "gpu-occlusion.hzb", .emit = &EmitDepthPyramid },
			// The anchors: the features' GPU stages at each slot (Api::GpuStageSlot), the draw families' face culls, the diagnostics' passes.
			{ .key = "stages.next-frame-state", .emit = &EmitStages<StageSlot::DepthSegmentNextFrameState> },
			{ .key = "stages.cull-state", .emit = &EmitStages<StageSlot::DepthSegmentCullState> },
			{ .key = "stages.before-build", .emit = &EmitStages<StageSlot::DepthSegmentBeforeBuild> },
			{ .key = "stages.before-depth-draw", .emit = &EmitStages<StageSlot::DepthSegmentBeforeDepthDraw> },
			{ .key = "stages.phase-two-before-draw", .emit = &EmitStages<StageSlot::DepthSegmentPhaseTwoBeforeDraw> },
			{ .key = "stages.after-phase-two", .emit = &EmitStages<StageSlot::DepthSegmentAfterPhaseTwo> },
			{ .key = "families.faces-cull", .emit = &EmitFacesCulls },
			{ .key = "diag.before-colour", .emit = &EmitDiagnostics<DiagPoint::kBeforeColour> },
			{ .key = "diag.after-colour", .emit = &EmitDiagnostics<DiagPoint::kAfterColour> },
			{ .key = "diag.after-depth", .emit = &EmitDiagnostics<DiagPoint::kAfterDepth> },
		};
	}

	void RegisterCorePasses()
	{
		for (const auto& pass : kCorePasses)
			RegisterPassDef(pass, "Core");
		for (const auto& pass : ShadowPassDefs())
			RegisterPassDef(pass, "Core");
		for (const auto& pass : ViewPassDefs())
			RegisterPassDef(pass, "Core");
	}
}
