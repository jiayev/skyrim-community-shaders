#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"

namespace DCLF::Features::Fades
{
	namespace
	{
		std::shared_ptr<const void> Setup(const Api::PassSetup& a_setup) { return SharedSceneOf(a_setup.scene); }
		template <bool Evaluate>
		std::shared_ptr<org::RenderPass> Make(const Api::PassContext& a_context)
		{
			const auto resources = a_context.Main();
			const auto scene = SharedSceneOf(*resources->scene);
			return Portals::MakeBatchPass(resources, scene ? scene->portalBatch : nullptr, a_context.segment, Evaluate);
		}
	}

	void RegisterPortalBatchPasses(Api::InstallContext& a_context)
	{
		a_context.RegisterPass({ .key = "fades.portal-batch", .passName = "cs.dclf.z.fade-portal-batch", .queue = Api::PassQueue::Compute, .make = &Make<false>, .setup = &Setup });
		a_context.RegisterPass({ .key = "fades.portal-evaluate", .passName = "cs.dclf.z.fade-portal-evaluate", .queue = Api::PassQueue::Compute, .make = &Make<true>, .setup = &Setup });
	}
}
