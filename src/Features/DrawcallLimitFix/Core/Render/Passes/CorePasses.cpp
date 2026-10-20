// Core's passes (Api::PassDef), which the schedule (Schedule/Frame.cpp) names by key: the main view's culling and copies (core.*), the
// diagnostics' anchors (diag.*), and through ViewEpochRecord.cpp the views' (view.*). Each make is its pass's condition and factory; the
// executor adds the pass. The features register their own passes (the main view's draws, main-view.*: Features/MainView; the forward
// epoch's forward.*: Features/Forward; the shadow and occlusion epochs' shadows.*: Features/Shadows/Engine).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/IndexPoolPass.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF::Draws
{
	namespace
	{
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

		std::shared_ptr<org::RenderPass> MemberQueries(const Api::PassContext& a_context)
		{
			return MakeBuildDrawsPass(a_context.Main(), a_context.segment, 1, true);
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

			// The two-phase tail, in the depth segment, in this order: build-draws (phase 1, last frame's HZB), depth draw, hzb (rebuilt from
		// it by the depth pyramid's producer, Api/DepthPyramid.h), build-draws-phase2 and the features' phase 2 passes (tree-lod-cull-phase2;
		// phase 1's rejects retested), depth-phase2 (the rescues: Features/MainView's). Phase 1's frame-old HZB misses what just came out
		// from behind an occluder; phase 2 takes those back. Each only while the HZB exists (PassContext::PhaseTwo).
		std::shared_ptr<org::RenderPass> BuildDrawsPhaseTwo(const Api::PassContext& a_context)
		{
			return a_context.PhaseTwo() ? MakeBuildDrawsPass(a_context.Main(), a_context.segment, 2) : nullptr;
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
			// Not while the colour shape draws its main buckets (CS_DCLF_MAIN_PLAIN): BuildDraws then raises no count, and the last scan
			// (a shape without them) left them zero.
			const auto segment = a_context.segment;
			return resources->sort ? resources->sort->ScanPass([segment](const org::PassPrepareContext& a_preparation) {
				const auto shape = CurrentFrame(a_preparation, segment);
				return !(shape && shape->classPlan && !shape->classPlan->main.empty());
			}) :
			                         nullptr;
		}

		std::shared_ptr<org::RenderPass> SortScatter(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->sort ? MakeSortSequencesPass(resources, a_context.segment) : nullptr;
		}

		// The forward sort after the colour build; its draw in the forward epoch (Features/Forward's forward.draw).
		std::shared_ptr<org::RenderPass> ForwardSort(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			if (!resources->forwardSort)
				return nullptr;
			return MakeForwardSortPass(resources->sequences, { nullptr, resources->classCounts }, [main = resources](const org::PassPrepareContext& a_preparation) {
				const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
				if (!frame || !BuildsDraws(a_preparation, *main, RenderGraphRuntime::Segment::MainOpaque))
					return ForwardSortSource{ frame ? frame->generation : 0 };
				ForwardSortSource source{ frame->generation, main->forwardSort };
				// The forward buckets' pairs (CS_DCLF_FORWARD_PLAIN), each counted by its classCounts word past the classes', ranked into
				// firstInstance for the plain draw.
				if (frame->classPlan && !frame->classPlan->forward.empty()) {
					const auto& plan = *frame->classPlan;
					for (std::size_t b = 0; b < plan.forward.size(); ++b)
						source.ranges.push_back({ plan.forward[b].first, plan.forward[b].capacity, plan.ForwardCountWord(b) * static_cast<std::uint32_t>(sizeof(std::uint32_t)) });
					source.options = ForwardSortConstants::kRankInstance;
					return source;
				}
				// Each state class's forward pair (its staging half, then its sorted half), counted by its classCounts word.
				for (std::uint32_t c = 0; c < frame->classes.size(); ++c)
					if (const auto& range = frame->classes[c].ranges[kClassRangeForward]; range.capacity)
						source.ranges.push_back({ range.first, range.capacity, ClassCountWord(c, kClassRangeForward) * static_cast<std::uint32_t>(sizeof(std::uint32_t)) });
				return source;
			});
		}

		using DiagPoint = Diagnostics::MainGraphPasses::Point;

		constexpr Api::PassDef kCorePasses[] = {
			// The main view's culling and copies (the main extension's resources).
			{ .key = "core.z.latched-copies", .passName = "cs.dclf.z.latched-copies", .kind = PassKind::Copy, .make = &LatchedCopies },
			{ .key = "core.latched-copies", .passName = "cs.dclf.latched-copies", .kind = PassKind::Copy, .make = &LatchedCopies },
			{ .key = "core.z.portal-queries", .passName = "cs.dclf.z.member-portal-queries", .queue = Api::PassQueue::Compute, .make = &MemberQueries },
			{ .key = "core.z.build-draws", .passName = "cs.dclf.z.build-draws", .make = &BuildDraws },
			{ .key = "core.build-draws", .passName = "cs.dclf.build-draws", .make = &BuildDraws },
			{ .key = "core.z.index-pool", .passName = "cs.dclf.z.index-pool", .make = &IndexPoolCopies },
			{ .key = "core.build-draws-phase2", .passName = "cs.dclf.build-draws-phase2", .make = &BuildDrawsPhaseTwo },
			{ .key = "core.z.visibility-events", .passName = "cs.dclf.z.visibility-events", .make = &VisibilityEvents },
			{ .key = "core.z.visibility-readback", .passName = "cs.dclf.z.visibility-readback", .kind = PassKind::Copy, .queue = PassQueue::None, .make = &VisibilityEventsCopy },
			{ .key = "core.sort-scan", .passName = "cs.dclf.sort-scan", .make = &SortScan },
			{ .key = "core.sort-scatter", .passName = "cs.dclf.sort-scatter", .make = &SortScatter },
			{ .key = "core.forward-sort", .passName = "cs.dclf.forward-sort", .make = &ForwardSort },
			// The anchors: the diagnostics' passes at each point.
			{ .key = "diag.before-colour", .emit = &EmitDiagnostics<DiagPoint::kBeforeColour> },
			{ .key = "diag.after-colour", .emit = &EmitDiagnostics<DiagPoint::kAfterColour> },
			{ .key = "diag.after-depth", .emit = &EmitDiagnostics<DiagPoint::kAfterDepth> },
		};
	}

	void RegisterCorePasses()
	{
		for (const auto& pass : kCorePasses)
			RegisterPassDef(pass, "Core");
		for (const auto& pass : ViewPassDefs())
			RegisterPassDef(pass, "Core");
	}
}
