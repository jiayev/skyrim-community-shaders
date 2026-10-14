// FrameRetirement: DCLF's retirement clock, advanced by the graph host's completed-frame callback (RenderGraphRuntime.cpp).
#include "Features/DrawcallLimitFix/Runtime/FrameRetirement.h"

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <ORGModuleServices/BackingPool.h>
#	include <OpenRenderGraph/PersistentGraphHost.h>
#endif

namespace DCLF
{
#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
	const std::shared_ptr<org::services::RetirementClock>& FrameRetirement()
	{
		static const auto clock = std::make_shared<org::services::RetirementClock>();
		return clock;
	}

	std::uint64_t SubmittedRetirementPoint()
	{
		const auto* host = RenderGraphRuntime::Get().Host();
		return host ? RetirementPoint(host->LastHostFrame()) : 0;
	}
#else
	const std::shared_ptr<org::services::RetirementClock>& FrameRetirement()
	{
		static const std::shared_ptr<org::services::RetirementClock> none;
		return none;
	}

	std::uint64_t SubmittedRetirementPoint() { return 0; }
#endif
}
