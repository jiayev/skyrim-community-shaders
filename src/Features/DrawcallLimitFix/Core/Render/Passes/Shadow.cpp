// The shadow views' and occlusion maps' graph extension: their resources registered, and the shadow epoch's passes gathered from the
// schedule (shadows.*: Features/Shadows/Engine's); and the shadow epoch's index pool copies (Core's IndexPoolPass, as the depth epoch's),
// which that feature's shadows.index-pool makes.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/IndexPoolPass.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"

namespace DCLF::Draws
{
	class ShadowExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit ShadowExtension(std::shared_ptr<ShadowResources> a_resources) :
			resources(std::move(a_resources)) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.constants"), resources->constants);
			RegisterSceneBuffers(a_graph, *resources->scene);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.visibility"), resources->visibility);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.view-blocks"), resources->viewBlocks.buffer);
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.sequences{}", s)), resources->sequences[s]);
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-count{}", s)), resources->count[s]);
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.bucket-counts{}", s)), resources->bucketCounts[s]);
			}
			for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
				if (resources->depth[i])
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.depth{}", i)), resources->depth[i]);
			}
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			// The shadow epoch's passes (Schedule/Frame.cpp).
			Api::PassContext context{};
			context.shadow = &resources;
			GatherScheduled(Api::EpochShape::Shadow, {}, context, a_out);
		}

	private:
		std::shared_ptr<ShadowResources> resources;
	};

	std::shared_ptr<org::RenderPass> MakeShadowIndexPoolPass(const std::shared_ptr<ShadowResources>& a_resources)
	{
		return std::static_pointer_cast<org::RenderPass>(std::make_shared<IndexPoolPass>(a_resources->pool, [shadow = a_resources](const org::PassPrepareContext& a_preparation) {
			const auto frame = CurrentShadowFrame(a_preparation);
			return IndexPoolPass::LatchOf{ frame ? frame->generation : 0, frame ? frame->latch : nullptr, frame ? frame->latchLayout.PoolOffset() : 0u };
		}));
	}

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeShadowExtension(std::shared_ptr<ShadowResources> a_resources)
	{
		return std::make_unique<ShadowExtension>(std::move(a_resources));
	}
}
