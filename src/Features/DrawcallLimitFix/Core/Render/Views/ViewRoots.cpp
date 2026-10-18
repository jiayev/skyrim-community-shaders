// A covered view's engine roots its faces draw in the engine's place (IndirectDraws.h, "The view engine"): the cube camera's
// (ViewLodRootsOwned), read of the draw families' faces (their faces' roots) and the view root contributions (the LOD land and objects
// roots: Features/Lod's, dclf-architecture.md, "Water reflections"). Render thread.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF
{
	std::uint32_t IndirectDraws::ViewLodRootsOwned(Api::EngineView a_view, bool a_plain)
	{
		// The roots whose members DCLF's faces draw, in a plain update of a frame whose faces are DCLF's (DecideCoverage):
		// - a draw family's faces' root while the faces draw the family (DecideViewFaces): drawn from DCLF's mirror, no registration needed.
		// - a view root contribution's root while its owned says so (the LOD land and objects roots while the engine's LOD swaps are gated
		//   and both LOD classes are DCLF's: Features/Lod's LodRootsOwned). Unowned, its roots stay, the members withheld (PassCapture).
		auto& capture = PassCapture::Get();
		capture.WatchViewResidue(a_view, 0);
		const auto toggles = ActiveToggles();
		if (!a_plain || !ViewDrawable(a_view, toggles) || !impl->ViewCovered(a_view) || !Policy::WithholdFromEngine())
			return 0;
		std::uint32_t owned = 0;
		const auto& facesOwned = impl->View(a_view).facesOwned;
		for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f) {
			const auto root = Extensions::DrawFamilies()[f].contribution.faces.root;
			owned |= (facesOwned[f] && root < Api::ViewRoot::Count) ? Api::ViewRootBit(root) : 0u;
		}
		const auto& views = Extensions::Views();
		const Api::ViewRootFrame frame{ a_view, toggles };
		for (std::size_t r = 0; r < Extensions::kViewRoots; ++r)
			if (const auto rootOwned = views.rootOwned[r]; rootOwned && rootOwned(frame))
				owned |= Api::ViewRootBit(static_cast<Api::ViewRoot>(r));
		// The updates that keep the roots (the view's residueDue: a diagnostics question): what the engine registers into the faces and
		// DCLF does not withhold is watched, by root - the view root contributions' (what skipping them loses: any is a defect once they
		// are owned) and the owned ones'.
		if (const auto residueDue = Extensions::ViewEntryOf(a_view).residueDue; residueDue && residueDue()) {
			capture.WatchViewResidue(a_view, views.rootBits | owned);
			return 0;
		}
		return owned;
	}
}
