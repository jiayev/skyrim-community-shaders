#pragma once

// Portals: DCLF's port of the main camera's portal and occluder walk (PortalViews: its programs, the shadow lights' walks and the VisibleSet,
// over the engine's shapes, PortalShapes.h), Main::IsRoomVisible answered from it (RoomVisibility), and their parity. The feature is the
// main cull input (Api::MainCullInputContribution, Api/CullInputs.h): whether the walk reached unbound space (the scene lists' keep check),
// the rooms' structure events, the programs (PortalPrograms.h's encoding, which Core sends and FadeStateCS and BuildDraws' phase 1 read),
// their version and each fade root's program (ProgramOf, judged in one batch). Its early hooks write the walk's thunk (Main::Update's call
// of FUN_1401a43c0; DCLF's walk follows it while the primary cull is installed) and note it for the primary cull's install, which requires
// it: without this feature the primary cull stays off. Its stand-in observer's updateEntry makes RoomVisibility live for the frame. Its
// report lines are self-scheduled (Diagnostics/PortalLines.cpp), and its parity (Diagnostics/PortalParity) runs under CS_DCLF_SET_PARITY or
// CS_DCLF_PERSISTENT_PARITY.
//
// Core keeps the category filing of the portal graphs' nodes, the programs buffer and the fade roots' programs (created among the fade
// buffers, uploaded with the depth commit) and the event PortalWalkBuilt on its bus. No switch, toggle, GPU stage or table. The light
// selection's per-light walks (Features/LocalLights' LightSelection, DEPENDS {Portals}: WalkLight, SharesRoom) are static calls into PortalViews. Included by the
// feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Portals
{
	// PortalsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// Diagnostics/PortalLines.cpp, each self-scheduled: the portal views' parity line, logged every 300 builds with CS_DCLF_SET_PARITY or
	// CS_DCLF_PERSISTENT_PARITY (PortalParity's window); Main::IsRoomVisible's answers, logged every 300 published VisibleSets (PortalViews'
	// window).
	void PortalViewsLine();
	void RoomVisibilityLines();
}
