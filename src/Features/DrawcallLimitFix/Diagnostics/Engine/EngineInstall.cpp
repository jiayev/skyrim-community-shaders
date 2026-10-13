#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "Accumulated.h"
#include "BillboardTurns.h"
#include "CutObservers.h"
#include "ImportTimings.h"
#include "LightCulls.h"
#include "LightSelectionParity.h"
#include "PortalParity.h"
#include "Registrations.h"
#include "RegistrationCensus.h"
#include "ReleaseGuard.h"
#include "ShadowViewTally.h"
#include "SunCasters.h"
#include "Features/DrawcallLimitFix/Diagnostics/Feature/OpenDefectProbes.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

// The engine area's observers, and the engine hooks only observers use (each checks its own switch).

namespace DCLF::Diagnostics
{
	void InstallEngine()
	{
		const auto& o = Observe();
		// CS_DCLF_RELEASE_GUARD: the engine's last releases on DCLF's threads counted.
		ReleaseGuard::Install();
		// CS_DCLF_REFLECTION_CENSUS: the water reflection cube map's draws.
		InstallReflectionCensus();
		// CS_DCLF_SET_PARITY: the billboards' observer on NiBillboardNode::OnVisible (before PrimaryCull reads the slot).
		BillboardTurns::Install();
		// CS_DCLF_TIMELINE: the engine's import work by thread (shares the LOD drain's and the retirement's detours with LodGates, which
		// installs after).
		if (o.timeline)
			ImportTimings::Install();
		// CS_DCLF_ACCUMULATED_PARITY: the engine's kAccumulated sampled for the decode's comparison, the animation gate, the frame camera.
		if (o.accumulated)
			Accumulated::Install();
		// The primary cull's observers: the cut, the stand-in walk, the light masks and the scene lists (CS_DCLF_PERSISTENT_PARITY), the
		// fade visibility, port and node snapshots (CS_DCLF_FADE_PARITY).
		CutObservers::Install(o.scene, o.fade);
		// CS_DCLF_PERSISTENT_PARITY: the point lights' culls on the frames they skip nothing.
		if (o.scene)
			LightCulls::Install();
		// CS_DCLF_CENSUS: the passes the registration hooks hand on to the engine.
		RegistrationCensus::Install();
		// The shadow views against the engine's (both parities), the sun's claimed casters (CS_DCLF_SET_PARITY) and the sun exclusion's
		// watch (CS_DCLF_PERSISTENT_PARITY).
		if (o.claimsOrScene) {
			InstallShadowViews();
			PortalParity::Install();
			LightSelectionParity::Install();
		}
		SunCasters::Install(o.claims, o.scene);
		// CS_DCLF_SET_PARITY: the members' withheld forward, refraction, Effect Lighting and particle passes.
		if (o.claims)
			Registrations::Install();
	}
}
