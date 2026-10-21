// The main graph extension: registers the main view's resources and adds its passes (the schedule's: Epochs/ScheduleExecutor.h) to the
// depth, colour and forward epochs.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"

namespace DCLF::Draws
{
	/** @brief The buffers a main commit may latch into (LatchedUploads); its latched copies declare them. */
	std::vector<LatchedTarget> MainLatchedTargets(const Resources& a_resources)
	{
		std::vector<LatchedTarget> out;
		for (const auto& buffer : { a_resources.frameConstants, a_resources.count, a_resources.retestDispatch })
			if (buffer)
				out.push_back({ buffer, nullptr });
		for (const auto& counts : a_resources.zBucketCounts)
			if (counts)
				out.push_back({ nullptr, counts });
		if (a_resources.classCounts)
			out.push_back({ nullptr, a_resources.classCounts });
		for (const auto& frameBuffer : a_resources.frameBuffers)
			if (frameBuffer.copy)
				out.push_back({ frameBuffer.copy, nullptr });
		// The features' scene resources' latched buffers (Api::SceneResources::latched: Features/TreeLod's draw row and list header); not the
		// next-frame state's frame rows (the features', the fades'), which the compute queue reads.
		Extensions::LatchSceneResources(a_resources.scene->sceneResources,
			[](void* a_out, const std::shared_ptr<org::Buffer>& a_buffer, const Versioned& a_versioned, std::size_t) {
				static_cast<std::vector<LatchedTarget>*>(a_out)->push_back({ a_buffer, a_versioned });
			},
			&out);
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
			Register(a_graph, org::ResourceIdentifier("cs.dclf.class-counts"), resources->classCounts);
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
			Register(a_graph, org::ResourceIdentifier("cs.dclf.retests"), resources->retests);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.retest-dispatch"), resources->retestDispatch);
			for (const auto& frameBuffer : resources->frameBuffers)
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister)), frameBuffer.copy);
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.native-target{}", i)), resources->native[i]);
			if (resources->nativeDepth)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.native-depth"), resources->nativeDepth);
			if (resources->terrainDepth)
				Register(a_graph, org::ResourceIdentifier(Extensions::NearTwin().resource), resources->terrainDepth);
			if (resources->refractionNormals)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.refraction-normals"), resources->refractionNormals);
			// The depth pyramid under its producer's names (a texture only with a producer: Extensions::DepthPyramidOf).
			if (resources->hzb) {
				const auto& pyramid = *Extensions::DepthPyramidOf();
				Register(a_graph, org::ResourceIdentifier(pyramid.resource), resources->hzb);
				if (resources->hzbScratch)
					Register(a_graph, org::ResourceIdentifier(pyramid.scratchResource), resources->hzbScratch);
			}
			if (resources->sort)
				resources->sort->Register(a_graph);
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			// The depth, colour and forward epochs' passes (Schedule/Frame.cpp; Passes/CorePasses.cpp makes them).
			Api::PassContext context{};
			context.main = &resources;
			GatherScheduled(Api::EpochShape::Main, {}, context, a_out);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeMainOpaqueExtension(std::shared_ptr<Resources> a_resources)
	{
		return std::make_unique<MainOpaqueExtension>(std::move(a_resources));
	}
}
