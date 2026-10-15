// The occlusion maps' report lines (CS_DCLF_STATS, among the shadow views' with their toggle on): each map DCLF drew or left to the
// engine since the last report, and the occlusion epochs' render-thread time.

#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"

namespace DCLF::Features::OcclusionMaps
{
	namespace
	{
		void Lines(std::uint32_t)
		{
			// The shadow counters are reset right after this point (Reports::Stats); the driver's frames are taken here.
			const auto& shadow = IndirectDraws::Get().GetShadowStats();
			const auto frames = FrameDriver::Get().TakeOcclusionFrames();
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				if (shadow.occlusionDrawn[v] || shadow.occlusionNotReady[v] || frames.native[v])
					logger::info("[DCLF] {} occlusion: DCLF drew {} maps ({} of the set's occluders, last), {} it could not draw, {} left to the engine whole; the engine "
								 "registered the map's other occluders on {} frames ({} not members now); {} member passes withheld (last frame)",
						Extensions::OcclusionMapOf(v).report, shadow.occlusionDrawn[v], shadow.occlusionInputs[v], shadow.occlusionNotReady[v],
						frames.native[v], frames.engine[v], frames.lacking[v], frames.withheld);
			if (shadow.occlusionEpochs)
				logger::info("[DCLF] occlusion maps: {} epochs, render thread {:.3f} ms per epoch", shadow.occlusionEpochs, shadow.occlusionMs / shadow.occlusionEpochs);
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "occlusion maps", .point = Api::ReportPoint::ShadowViews, .lines = &Lines });
	}
}
