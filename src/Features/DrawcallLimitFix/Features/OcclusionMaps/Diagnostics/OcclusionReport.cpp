// The occlusion maps' report lines (CS_DCLF_STATS, among the shadow views' with their toggle on): each map DCLF drew in its shadow epoch
// or left to the engine since the last report, and the maps' render-thread time in the shadow commits.

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
					logger::info("[DCLF] {} occlusion: DCLF drew {} maps in the shadow epoch ({} of the set's occluders, last), {} taken it could not draw, {} left to "
								 "the engine whole; the engine culled the map's scene on {} frames; {} frames drawn while occluders were still joining (left out "
								 "until members; {} now); {} member passes withheld (last frame)",
						Extensions::OcclusionMapOf(v).report, shadow.occlusionDrawn[v], shadow.occlusionInputs[v], shadow.occlusionNotReady[v],
						frames.native[v], frames.engine[v], frames.joining[v], frames.lacking[v], frames.withheld);
			if (shadow.occlusionEpochs)
				logger::info("[DCLF] occlusion maps: in {} shadow epochs, render thread {:.3f} ms per epoch (their latches)", shadow.occlusionEpochs,
					shadow.occlusionMs / shadow.occlusionEpochs);
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "occlusion maps", .point = Api::ReportPoint::ShadowViews, .lines = &Lines });
	}
}
