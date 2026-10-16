// The shadow views' frame point, Core's half (ShadowPoints.h): the frame's shadow view list, over IndirectDraws' capacity
// (Shadow/ShadowCapture.cpp) and the revision inputs.

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowPoints.h"

#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF::Draws
{
	void ListShadowViews()
	{
		// The frame's shadow views, in the order the engine is about to render them. Everything downstream - the capture's
		// attribution, the withholding, the epochs - identifies a view by this list.
		auto& draws = IndirectDraws::Get();
		ShadowViews::Get().SetViewCapacity(ActiveToggles().shadows ? draws.ShadowViewCapacity() : UINT32_MAX);
		ShadowViews::Get().Rebuild();
		// The shadow views' candidates: a scene revision's input (its main latch holds a cascade and a shadow volume each).
		draws.NoteRevisionInputs();
	}
}
