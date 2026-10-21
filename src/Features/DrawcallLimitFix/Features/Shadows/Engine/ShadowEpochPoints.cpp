// The shadow epoch's frame points (Api::ShadowEpochsContribution), called by Core's FrameDriver from the shell's hooks and the feature's
// own (ShadowCasterPoints.cpp): the frame's shadow views' coverage, before the engine's shadow lights register, the shadow frame, the
// occlusion maps' readiness (at the shadow maps, before the engine draws them) and the shadow epoch under shadow-maps.after, which draws
// the maps taken with the shadow views, over Core's commits (IndirectDraws: ShadowCapture.cpp, ShadowEpochs.cpp).

#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <cstdint>

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		// Render thread, after the install: the epoch's trigger.
		Api::TriggerId shadowMapsAfter;

		void BeforeShadowCasters()
		{
			// The shadow epoch commits the installed publication's shadow payload (built ahead with it): the frame's views are DCLF's only
			// while the selected revision recorded it, decided before any of them registers.
			if (ActiveToggles().shadows)
				IndirectDraws::Get().DecideShadowCoverage();
		}

		void BeforeShadowMaps()
		{
			if (ActiveToggles().shadows)
				IndirectDraws::Get().BeginShadowFrame();
		}

		std::uint32_t AfterShadowMaps(std::uint32_t a_maps)
		{
			// The frame's shadow epoch: every view the accumulator hook captured and the occlusion maps taken, drawn in one graph execution.
			Extensions::TriggerScope trigger(shadowMapsAfter);
			return IndirectDraws::Get().ExecuteShadowFrame(a_maps);
		}

		bool OcclusionReady(std::uint32_t a_map) { return IndirectDraws::Get().OcclusionReady(a_map); }
	}

	void RegisterShadowEpochs(Api::InstallContext& a_context)
	{
		shadowMapsAfter = a_context.FindTrigger("shadow-maps.after");
		a_context.RegisterShadowEpochs(
			{ .beforeShadowCasters = &BeforeShadowCasters, .beforeShadowMaps = &BeforeShadowMaps, .afterShadowMaps = &AfterShadowMaps,
				.occlusionReady = &OcclusionReady });
	}
}
