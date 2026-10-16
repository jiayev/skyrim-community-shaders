// The sun entry rule's planes (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY), render thread: as a shadow build's inputs take them from
// the Sun board, against the engine's full-frustum processes' own after their cull; counted in the sun views' tally (SunViewParity.cpp's
// line).

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Sun.h"

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Features::Shadows::Sun
{
	namespace
	{
		void CheckSunEntryPlanes(const Diagnostics::SunEntryPlanesTaken& a_taken)
		{
			// The processes' own planes after the full-frustum cull against DCLF's (the Sun board, SunBoard), which the entry rule tests.
			auto& tally = Diagnostics::ShadowViewTally(Diagnostics::ShadowView::Sun);
			const auto& sunViews = SunBoard();
			if (!a_taken.taken) {
				++tally.missing;
				tally.Note("missing full frustum", 0.0f, fmt::format("inputs of frame {}, DCLF's views of frame {} (valid {}, {} processes)", a_taken.frame,
					sunViews.sceneFrame, sunViews.valid, sunViews.fullFrusta.size()));
				return;
			}
			auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
			auto* sun = node ? node->GetRuntimeData().sunShadowDirLight : nullptr;
			if (!sun)
				return;
			std::size_t index = 0;
			for (const auto& process : sun->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray) {
				if (!process)
					continue;
				ShadowCameras::Planes engine;
				for (std::uint32_t p = 0; p < 6; ++p)
					engine.plane[p] = { process->planes.cullingPlanes[p].normal.x, process->planes.cullingPlanes[p].normal.y, process->planes.cullingPlanes[p].normal.z,
						process->planes.cullingPlanes[p].constant };
				engine.mask = process->planes.activePlanes.underlying() & 0x3Fu;
				++tally.fullFrusta;
				if (index >= sunViews.fullFrusta.size()) {
					++tally.fullFrustumPlanes;
					tally.Note("full-frustum planes", 1.0f, fmt::format("process {}: DCLF has {} processes", index, sunViews.fullFrusta.size()));
				} else if (!ShadowCameras::SamePlanes(sunViews.fullFrusta[index], engine)) {
					++tally.fullFrustumPlanes;
					tally.Note("full-frustum planes", 1.0f, fmt::format("process {}: {}", index, ShadowCameras::DescribePlanes(sunViews.fullFrusta[index], engine)));
				}
				++index;
			}
		}
	}

	void SubscribeSunEntryPlanesParity()
	{
		Diagnostics::Subscribe<Diagnostics::SunEntryPlanesTaken>(&CheckSunEntryPlanes);
	}
}
