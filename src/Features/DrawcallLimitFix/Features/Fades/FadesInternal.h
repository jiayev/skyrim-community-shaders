#pragma once

// Fades' registrations, one per source file, called by the feature's install (FadesFeature.cpp) in the order Core's fade code registered
// in (the family and component, the channels, the render side, the root states, the writers last), and its settings. Included by the
// feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Fades
{
	// FadeRoots.cpp: the fade roots' table family and the coordinator component that lists and steps its rows.
	void RegisterScene(Api::InstallContext& a_context);
	// FadeRender.cpp (its parts in FadeSeeds, FadeResources, FadeCommit, FadeStage: FadeScene.h): the seeds pool, the scene resources, the
	// depth commit hook, the stages, the frame and cull bindings.
	void RegisterRender(Api::InstallContext& a_context);
	// FadeEvents.cpp: the engine channels "fade snaps" then "fade amounts" (their ids for the component's events.keys).
	void RegisterEngine(Api::InstallContext& a_context, Api::EngineChannelId& a_snaps, Api::EngineChannelId& a_amounts);
	// FadeWriters.cpp: the stand-in observer (the writer tables and stores; the animated batch at listJobsDone), and the AnimatedFade and
	// FadeOnVisible hooks. Last: Main::Update's entry keeps its order.
	void RegisterWriters(Api::InstallContext& a_context);
	// FadeEvents.cpp: the feature's early hook (FadeSnap).
	void InstallEarlyHooks();
	// Diagnostics/: the report lines, the parity and the port check.
	void RegisterDiagnostics(Api::InstallContext& a_context);

	// FadesFeature.cpp: the feature's switches (any thread after the install).
	/** @brief CS_DCLF_FADE_PARITY: the fade port against the engine's functions, and FadeStateCS against the port. */
	bool FadeParity();
	/** @brief CS_DCLF_EMPTY_FADE_NODE (default on): the engine's empty fade node is no fade root. */
	bool EmptyFadeNodeIsNone();
}
