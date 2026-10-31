// The shadow views' frame points, Core's half (ShadowPoints.h): the frame's shadow view list, over IndirectDraws' capacity
// (Shadow/ShadowCapture.cpp) and the revision inputs.

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowPoints.h"

#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF::Draws
{
	void BeginShadowViews()
	{
		// Before any shadow light registers: the capacity and the coverage (the epochs' handler, after this) the frame's views are
		// covered by, so the withholding, the sun's claims and the census all test this frame's.
		auto& draws = IndirectDraws::Get();
		ShadowViews::Get().SetViewCapacity(ActiveToggles().shadows ? draws.ShadowViewCapacity() : UINT32_MAX);
		ShadowViews::Get().BeginCasters();
	}

	void ShadowLightKept(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots)
	{
		ShadowViews::Get().AddCaster(a_light, a_firstSlot, a_slots);
	}

	void ListShadowViews()
	{
		// The frame's shadow views, in the order the engine renders them. Everything downstream - the capture's attribution, the
		// withholding, the epochs - identifies a view by this list.
		ShadowViews::Get().Rebuild();
	}

	void NoteShadowRevisionInputs()
	{
		// The shadow views' candidates: a scene revision's input (its main latch holds a cascade and a shadow volume each).
		IndirectDraws::Get().NoteRevisionInputs();
	}
}
