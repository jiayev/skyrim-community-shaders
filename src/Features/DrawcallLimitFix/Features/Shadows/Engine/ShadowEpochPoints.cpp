// The shadow and occlusion epochs' frame points (Api::ShadowEpochsContribution), called by Core's FrameDriver from the shell's hooks and the
// feature's own (ShadowCasterPoints.cpp): the frame's shadow views' coverage, before the engine's shadow lights register, and the shadow
// frame, the shadow epoch under shadow-maps.after, the occlusion maps' readiness and their epoch under occlusion.draw, over Core's commits
// (IndirectDraws: ShadowCapture.cpp, ShadowEpochs.cpp).

#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <cstdint>

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		// Render thread, after the install: the epochs' triggers.
		Api::TriggerId shadowMapsAfter, occlusionDraw;

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

		void AfterShadowMaps()
		{
			// The frame's shadow epoch: every view the accumulator hook captured, drawn in one graph execution.
			Extensions::TriggerScope trigger(shadowMapsAfter);
			IndirectDraws::Get().ExecuteShadowFrame();
		}

		bool OcclusionReady(std::uint32_t a_map) { return IndirectDraws::Get().OcclusionReady(a_map); }

		std::uint32_t DrawOcclusion(std::uint32_t a_maps)
		{
			Extensions::TriggerScope trigger(occlusionDraw);
			return IndirectDraws::Get().ExecuteOcclusion(a_maps);
		}
	}

	void RegisterShadowEpochs(Api::InstallContext& a_context)
	{
		shadowMapsAfter = a_context.FindTrigger("shadow-maps.after");
		occlusionDraw = a_context.FindTrigger("occlusion.draw");
		a_context.RegisterShadowEpochs(
			{ .beforeShadowCasters = &BeforeShadowCasters, .beforeShadowMaps = &BeforeShadowMaps, .afterShadowMaps = &AfterShadowMaps, .occlusionReady = &OcclusionReady,
				.drawOcclusion = &DrawOcclusion });
	}
}
