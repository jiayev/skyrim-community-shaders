// A covered view's engine roots its faces draw in the engine's place (IndirectDraws.h, "The view engine"): the cube camera's LOD roots
// (ViewLodRootsOwned), read of the faces' tree LOD and the LOD classes' contributions and the LOD gates (dclf-lod.md, "Water reflections").
// Render thread.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF
{
	// The roots' bits are the roots' word the view's engine hooks read (PassCapture's, until the roots are the views' own).
	static_assert(Api::ViewRootBit(Api::ViewRoot::LodLandAndObjects) == PassCapture::kLodRoots && Api::ViewRootBit(Api::ViewRoot::LodTrees) == PassCapture::kTreeRoot,
		"Api::ViewRootBit is not PassCapture's kLodRoots and kTreeRoot");

	std::uint32_t IndirectDraws::ViewLodRootsOwned(Api::EngineView a_view, bool a_plain)
	{
		// The roots whose members DCLF's faces draw, in a plain update of a frame whose faces are DCLF's (DecideCoverage):
		// - LOD trees while the faces' tree LOD is DCLF's (DecideViewTreeLod): drawn from DCLF's mirror, no registration needed.
		// - LOD land and objects while the engine's LOD swaps are gated (a show veto registered: the LOD gates') and both are DCLF's
		//   (their classes): a swap's incoming blocks are shown only by the frame whose claims hold them, so the faces never need the
		//   engine's registrations of them. Ungated, the LOD chunks and objects the engine shows or attaches in motion take DCLF a few frames
		//   to bind (ClassifyResidue) and their roots stay, the members withheld (PassCapture).
		auto& capture = PassCapture::Get();
		capture.WatchViewResidue(a_view, 0);
		if (!a_plain || !ViewDrawable(a_view) || !impl->ViewCovered(a_view) || !Policy::WithholdFromEngine())
			return 0;
		const auto toggles = ActiveToggles();
		const bool lodOwned = Extensions::ShowVetoRegistered() && Extensions::ObjectClassOn(toggles, Api::ObjectClass::LodObjects) &&
		                      Extensions::ObjectClassOn(toggles, Api::ObjectClass::LodTerrain);
		const std::uint32_t owned = (impl->View(a_view).treeOwned ? FamilySlotOf(Api::DrawFamily::TreeInstances).lodRoot : 0u) | (lodOwned ? PassCapture::kLodRoots : 0u);
		// The updates that keep the roots (the view's residueDue: a diagnostics question): what the engine registers into the faces and
		// DCLF does not withhold is watched, by root - the LOD roots' (what skipping them loses: any is a defect once they are owned) and
		// the tree root's.
		if (const auto residueDue = Extensions::ViewEntryOf(a_view).residueDue; residueDue && residueDue()) {
			capture.WatchViewResidue(a_view, PassCapture::kLodRoots | owned);
			return 0;
		}
		return owned;
	}
}
