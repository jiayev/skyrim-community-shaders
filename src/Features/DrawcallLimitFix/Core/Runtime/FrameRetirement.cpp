// FrameRetirement: DCLF's retirement clock, advanced by the graph host's completed-frame callback (RenderGraphRuntime.cpp).
#include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"

#include "RenderGraph/RenderGraphRuntime.h"

#include <ORGModuleServices/BackingPool.h>
#include <OpenRenderGraph/PersistentGraphHost.h>

namespace DCLF
{
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
}
