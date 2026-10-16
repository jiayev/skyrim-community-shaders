// The shadow engine's frame points (ShadowPoints.h): FrameDriver's shadow and occlusion points, over IndirectDraws' shadow functions
// (Shadow/ShadowCapture.cpp, ShadowEpochs.cpp) and the frame's shadow view list.

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowPoints.h"

#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF::Draws
{
	namespace
	{
		class EnginePoints final : public ShadowPoints
		{
		public:
			void BeforeShadowMaps() override
			{
				// The frame's shadow views, in the order the engine is about to render them. Everything downstream - the capture's
				// attribution, the withholding, the epochs - identifies a view by this list.
				auto& draws = IndirectDraws::Get();
				ShadowViews::Get().SetViewCapacity(ActiveToggles().shadows ? draws.ShadowViewCapacity() : UINT32_MAX);
				ShadowViews::Get().Rebuild();
				// The shadow views' candidates: a scene revision's input (its main latch holds a cascade and a shadow volume each).
				draws.NoteRevisionInputs();
				// The shadow epoch commits the installed publication's shadow payload (built ahead with it).
				if (ActiveToggles().shadows) {
					draws.DecideShadowCoverage();
					draws.BeginShadowFrame();
				}
			}

			void AfterShadowMaps() override { IndirectDraws::Get().ExecuteShadowFrame(); }

			bool OcclusionReady(std::uint32_t a_map) override { return IndirectDraws::Get().OcclusionReady(a_map); }

			std::uint32_t DrawOcclusion(std::uint32_t a_maps) override { return IndirectDraws::Get().ExecuteOcclusion(a_maps); }
		};

		EnginePoints enginePoints;
	}

	ShadowPoints& ShadowEnginePoints()
	{
		return enginePoints;
	}
}
