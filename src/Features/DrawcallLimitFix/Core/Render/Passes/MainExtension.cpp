// The main graph extension: registers the main view's resources and adds its passes to the depth, colour and forward epochs.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/IndexPoolPass.h"

namespace DCLF::Draws
{
	/** @brief The buffers a main commit may latch into (LatchedUploads); its latched copies declare them. */
	std::vector<LatchedTarget> MainLatchedTargets(const Resources& a_resources)
	{
		std::vector<LatchedTarget> out;
		for (const auto& buffer : { a_resources.frameConstants, a_resources.count })
			if (buffer)
				out.push_back({ buffer, nullptr });
		for (const auto& counts : a_resources.zBucketCounts)
			if (counts)
				out.push_back({ nullptr, counts });
		for (const auto& frameBuffer : a_resources.frameBuffers)
			if (frameBuffer.copy)
				out.push_back({ frameBuffer.copy, nullptr });
		// Tree LOD's draw row and list header; not the trees' or fades' frame rows, which the compute queue reads.
		if (const auto& scene = *a_resources.scene; scene.treeLodCull) {
			out.push_back({ scene.treeLodDraw, nullptr });
			out.push_back({ nullptr, scene.treeLodVisible });
		}
		return out;
	}

	class MainOpaqueExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit MainOpaqueExtension(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			Register(a_graph, org::ResourceIdentifier("cs.dclf.sequences"), resources->sequences);
			RegisterSceneBuffers(a_graph, *resources->scene);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.draw-count"), resources->count);
			// Read by address; readers declare it so the graph orders them after the latched copies.
			Register(a_graph, org::ResourceIdentifier("cs.dclf.frame-constants"), resources->frameConstants);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.z.bucket-counts"), resources->zBucketCounts[0]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.z.bucket-counts2"), resources->zBucketCounts[1]);
			if (const auto& foliage = resources->foliage) {
				for (std::uint32_t h = 0; h < 2; ++h) {
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.foliage-ids{}", h)), foliage->ids[h]);
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.foliage-colours{}", h)), foliage->colours[h]);
				}
				Register(a_graph, org::ResourceIdentifier("cs.dclf.foliage-results"), foliage->results);
				Register(a_graph, org::ResourceIdentifier("cs.dclf.foliage-owners"), foliage->owners);
			}
			Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility"), resources->visibility);
			if (resources->frustum)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.frustum"), resources->frustum);
			for (const auto& frameBuffer : resources->frameBuffers)
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister)), frameBuffer.copy);
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.native-target{}", i)), resources->native[i]);
			if (resources->nativeDepth)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.native-depth"), resources->nativeDepth);
			if (resources->terrainDepth)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.terrain-blending-depth"), resources->terrainDepth);
			if (resources->refractionNormals)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.refraction-normals"), resources->refractionNormals);
			if (resources->hzb) {
				Register(a_graph, org::ResourceIdentifier("cs.dclf.hzb"), resources->hzb);
				Register(a_graph, org::ResourceIdentifier("cs.dclf.hzb-counter"), resources->hzbCounter);
			}
			if (resources->sort)
				resources->sort->Register(a_graph);
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			using Segment = RenderGraphRuntime::Segment;
			const auto colour = RenderGraphRuntime::EpochOf(Segment::MainOpaque);
			const auto depth = RenderGraphRuntime::EpochOf(Segment::ZPrepass);
			const auto colourSegment = Segment::MainOpaque;
			const auto depthSegment = Segment::ZPrepass;
			// Each epoch's first pass copies the commit's latched values. It publishes its declared targets (Resources::latchedTargets),
			// so a commit latches only what the pass will copy.
			for (const auto segment : { depthSegment, colourSegment })
				AddPass(a_out, PassKind::Copy, segment == depthSegment ? "cs.dclf.z.latched-copies" : "cs.dclf.latched-copies",
					MakeLatchedCopiesPass([resources = resources] { return MainLatchedTargets(*resources); },
						[segment](const org::PassPrepareContext& a_preparation) {
							const auto frame = CurrentFrame(a_preparation, segment);
							return frame ? LatchedSource{ frame->generation, frame, std::span(&frame->latched, 1) } : LatchedSource{};
						},
						&resources->latchedTargets),
					RenderGraphRuntime::EpochOf(segment));
			// A pass's segment is fixed, so each segment has its own instances.
			// The sort by pipeline (DrawSort): the scan runs in every execution (so the counts it clears are zero even without a
			// build), the scatter only after a build.
			const auto addSort = [&](const char* a_scan, const char* a_scatter, RenderGraphRuntime::Segment a_segment, auto a_epoch) {
				if (!resources->sort)
					return;
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute(a_scan, resources->sort->ScanPass())
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(a_epoch));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute(a_scatter,
					MakeSortSequencesPass(resources, a_segment))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(a_epoch));
			};
			// Tree wind for the next frame, beside this epoch's raster work.
			if (resources->scene->treeWind && resources->scene->trees)
				AddPass(a_out, PassKind::Compute, "cs.dclf.z.tree-wind", MakeTreeWindPass(resources->scene), depth, PassQueue::Compute);
			// Fade roots' state for the next frame's culling, likewise.
			if (resources->scene->fadeState && resources->scene->fadeRoots)
				AddPass(a_out, PassKind::Compute, "cs.dclf.z.fade-state", MakeFadeStatePass(resources), depth, PassQueue::Compute);
			// Its write-back events, to the host.
			if (resources->scene->fadeState && resources->scene->fadeRoots && resources->scene->fadeEvents && resources->scene->fadeWriteBack)
				AddPass(a_out, PassKind::Copy, "cs.dclf.z.fade-events", MakeEventReadbackPass(resources->scene, resources->scene->fadeEvents, resources->scene->fadeWriteBack), depth, PassQueue::None);
			// The frame's texture transforms into the payload's material rows, before anything of the frame draws from them.
			if (resources->scene->textureTransforms)
				AddPass(a_out, PassKind::Compute, "cs.dclf.z.texture-transforms", MakeTextureTransformsPass(resources), depth);
			AddPass(a_out, PassKind::Compute, "cs.dclf.z.build-draws", MakeBuildDrawsPass(resources, depthSegment), depth);
			// The pool copies the depth commit gave out (when the shadow epoch has not), before the draws read them.
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.index-pool",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<IndexPoolPass>(resources->pool, [main = resources](const org::PassPrepareContext& a_preparation) {
					const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
					return IndexPoolPass::LatchOf{ frame ? frame->generation : 0, frame ? frame->latch : nullptr, frame ? frame->latchLayout.PoolOffset() : 0u };
				})))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(depth));
			// Tree LOD's phase 1 list, before the depth draw.
			if (resources->scene->treeLodCull)
				AddPass(a_out, PassKind::Compute, "cs.dclf.z.tree-lod-cull", MakeTreeLodCullPass(resources, 1), depth);
			AddPass(a_out, PassKind::Render, "cs.dclf.z.depth", MakeMainOpaquePass(resources, depthSegment), depth);
			AddPass(a_out, PassKind::Compute, "cs.dclf.build-draws", MakeBuildDrawsPass(resources, colourSegment), colour);
			addSort("cs.dclf.sort-scan", "cs.dclf.sort-scatter", colourSegment, colour);
			// The forward sort after the colour build; its draw in the forward epoch (IndirectDraws::ExecuteForward).
			if (resources->forwardSort) {
				AddPass(a_out, PassKind::Compute, "cs.dclf.forward-sort",
					MakeForwardSortPass(resources->sequences, { resources->count, nullptr }, [main = resources](const org::PassPrepareContext& a_preparation) {
						const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
						if (!frame || !BuildsDraws(a_preparation, *main, RenderGraphRuntime::Segment::MainOpaque))
							return ForwardSortSource{ frame ? frame->generation : 0 };
						return ForwardSortSource{ frame->generation, main->forwardSort, static_cast<std::uint32_t>(ForwardStagingSlot(*frame)), frame->forwardCapacity };
					}),
					colour);
				AddPass(a_out, PassKind::Render, "cs.dclf.forward", MakeForwardDrawPass(resources), RenderGraphRuntime::EpochOf(Segment::Forward));
			}
			// Refraction normals, in the forward epoch after the blended draws.
			if (resources->refractionNormals && resources->nativeDepth && resources->pool)
				AddPass(a_out, PassKind::Render, "cs.dclf.refraction", MakeRefractionDrawPass(resources), RenderGraphRuntime::EpochOf(Segment::Forward));
			Diagnostics::Notify(Diagnostics::MainGraphPasses{ Diagnostics::MainGraphPasses::kBeforeColour, resources, colour, a_out });
			AddPass(a_out, PassKind::Render, "cs.dclf.main-opaque", MakeMainOpaquePass(resources, colourSegment), colour);
			Diagnostics::Notify(Diagnostics::MainGraphPasses{ Diagnostics::MainGraphPasses::kAfterColour, resources, colour, a_out });
			// The two-phase tail, in the depth segment, in this order: build-draws (phase 1, last frame's HZB), depth draw, hzb
			// (rebuilt from it), build-draws-phase2 and tree-lod-cull-phase2 (phase 1's rejects retested), depth-phase2 (the
			// rescues). Phase 1's frame-old HZB misses what just came out from behind an occluder; phase 2 takes those back.
			if (resources->hzb) {
				AddPass(a_out, PassKind::Compute, "cs.dclf.hzb", MakeHzbPass(resources, depthSegment), depth);
				AddPass(a_out, PassKind::Compute, "cs.dclf.build-draws-phase2", MakeBuildDrawsPass(resources, depthSegment, 2), depth);
				if (resources->scene->treeLodCull)
					AddPass(a_out, PassKind::Compute, "cs.dclf.tree-lod-cull-phase2", MakeTreeLodCullPass(resources, 2), depth);
				AddPass(a_out, PassKind::Render, "cs.dclf.depth-phase2", MakeMainOpaquePass(resources, depthSegment, true), depth);
			}
			if (resources->scene->treeLodCull && resources->scene->treeLodCounts)
				AddPass(a_out, PassKind::Copy, "cs.dclf.z.tree-lod-counts", MakeTreeLodReadbackPass(resources->scene), depth, PassQueue::None);
			// Visibility changes once both phases' verdicts are final, and their readback copy.
			const auto& sceneBuffers = *resources->scene;
			if (sceneBuffers.visibilityEventsProgram && sceneBuffers.visibilityReadback && sceneBuffers.fadeState && sceneBuffers.fadeRoots && resources->frustum) {
				AddPass(a_out, PassKind::Compute, "cs.dclf.z.visibility-events", MakeVisibilityEventsPass(resources), depth);
				AddPass(a_out, PassKind::Copy, "cs.dclf.z.visibility-readback", MakeEventReadbackPass(resources->scene, resources->scene->visibilityEvents, resources->scene->visibilityReadback), depth, PassQueue::None);
			}
			Diagnostics::Notify(Diagnostics::MainGraphPasses{ Diagnostics::MainGraphPasses::kAfterDepth, resources, depth, a_out });
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeMainOpaqueExtension(std::shared_ptr<Resources> a_resources)
	{
		return std::make_unique<MainOpaqueExtension>(std::move(a_resources));
	}
}
