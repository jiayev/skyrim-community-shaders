// Portals' report lines (Portals.h), each self-scheduled by what it reports: the portal views' parity line every 300 builds
// (PortalParity's window), the room visibility's every 300 published VisibleSets (PortalViews' window).

#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Features/Portals/RoomVisibility.h"
#include "Features/DrawcallLimitFix/Features/Portals/Diagnostics/PortalParity.h"

namespace DCLF::Features::Portals
{
	void PortalViewsLine()
	{
		const auto p = Diagnostics::PortalParity::Take();
		const auto b = PortalViews::Get().TakeBuildCounts();
		const std::uint64_t differ = p.roomsMissing + p.roomsExtra + p.programsDiffer + p.unboundDiffer + p.accumDiffer;
		logger::info("[DCLF] portal views (T5a: DCLF's walk of the portal graph, against the engine's entry): {} frames ({} built, {} without a graph or camera), {} rooms in the "
					 "engine's maps ({} not reached, {} reached only by DCLF), {} programs ({} rooms', {} unbound space's differ; {} unbound space's matching with the engine's occluder states), {} walks' room lists differ; programs held {} ops, "
					 "{} sets ({} portals, {} occlusion planes, {} boxes); largest plane difference {:.3g}; {} frames the camera moved after the engine's walk (by up to "
					 "{:.3g}, {} of them turned: {} differences there, not counted); boxes: {} checked, {} with other corners (by up to {:.3g}), {} sets differ; view planes other than the list process's on {} frames{}{}{}{}{}",
			p.frames, b.built, b.skipped, p.rooms, p.roomsMissing, p.roomsExtra, p.programs, p.programsDiffer, p.unboundDiffer, p.unboundEngineStates, p.accumDiffer, p.ops, p.sets, b.programs.portals,
			b.programs.planes, b.programs.boxes, p.largest, p.eyeMoved, p.largestMove, p.turned, p.movedDiffer, p.boxChecks, p.boxCornersDiffer, p.largestCorner, p.boxSetsDiffer,
			p.planesDiffer, p.planesFirst.empty() ? "" : " (first: " + p.planesFirst + ")", p.occluderFirst.empty() ? "" : "; unbound space's occluders: " + p.occluderFirst,
			p.boxFirst.empty() ? "" : "; first box: " + p.boxFirst, differ ? " <- PORTAL VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
	}

	void RoomVisibilityLines()
	{
		if (!RoomVisibility::Installed())
			return;
		const auto a = RoomVisibility::Take();
		logger::info("[DCLF] room visibility (T7f checklist item 3): {} calls answered by DCLF's VisibleSet, {} by the engine (DCLF off, or no set yet)", a.dclf, a.engine);
	}
}
