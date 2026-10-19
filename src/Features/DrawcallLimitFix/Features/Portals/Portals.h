#pragma once

// Portals: DCLF's port of the main camera's portal and occluder walk (PortalViews: its programs, the shadow lights' walks and the VisibleSet,
// over the engine's shapes, PortalShapes.h), Main::IsRoomVisible answered from it (RoomVisibility), and their parity. The feature is the
// main cull input (Api::MainCullInputContribution, Api/CullInputs.h): whether the walk reached unbound space (the scene lists' keep check),
// the rooms' structure events, the programs' version, each fade root's program (ProgramOf, judged in one batch) and PortalTest on the CPU
// (the input's test). The programs (PortalPrograms.h's encoding, which FadeStateCS and BuildDraws' phase 1 read) are the feature's
// buffer: its scene resources, required to the words the revision's inputs posted, its commit hook's upload with the depth commit, and
// the Programs cull binding Core's cull reads it by (PortalPrograms.cpp). Its early hooks write the walk's thunk (Main::Update's call
// of FUN_1401a43c0; DCLF's walk follows it while the primary cull is installed) and note it for the primary cull's install, which requires
// it: without this feature the primary cull stays off. Its stand-in observer's updateEntry makes RoomVisibility live for the frame. Its
// report lines are self-scheduled (Diagnostics/PortalLines.cpp), and its parity (Diagnostics/PortalParity) runs under CS_DCLF_SET_PARITY or
// CS_DCLF_PERSISTENT_PARITY.
//
// Core keeps the category filing of the portal graphs' nodes, the fade roots' programs (created among the fade buffers, uploaded with
// the depth commit) and the event PortalWalkBuilt on its bus. No switch, toggle, pass or table. The light
// selection's per-light walks (Features/LocalLights' LightSelection, DEPENDS {Portals}: WalkLight, SharesRoom) are static calls into PortalViews. Included by the
// feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Portals
{
	// PortalsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// PortalPrograms.cpp: the programs buffer's registrations (scene resources, commit hook, cull binding), from the install; and
	// PortalPrograms.hlsli's PortalTest on the CPU over given programs (the main cull input's test).
	void RegisterPrograms(Api::InstallContext& a_context);
	int PortalTest(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius,
		std::uint32_t a_nodeFlags, std::string& a_why);

	// Diagnostics/PortalLines.cpp, each self-scheduled: the portal views' parity line, logged every 300 builds with CS_DCLF_SET_PARITY or
	// CS_DCLF_PERSISTENT_PARITY (PortalParity's window); Main::IsRoomVisible's answers, logged every 300 published VisibleSets (PortalViews'
	// window).
	void PortalViewsLine();
	void RoomVisibilityLines();

	// Diagnostics/PortalCapture.cpp: CS_DCLF_PORTAL_CAPTURE's switches and, when set, its subscription (the registered batches' queries
	// and the frame's programs into corpus files for tests/DclfPortalBench).
	void InstallPortalCapture(Api::InstallContext& a_context);
}
