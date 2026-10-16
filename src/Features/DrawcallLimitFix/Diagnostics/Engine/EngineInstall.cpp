// The engine area's observers, and the engine hooks only observers use (each checks its own switch).

#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "Accumulated.h"
#include "CutObservers.h"
#include "LightCulls.h"
#include "LightSelectionParity.h"
#include "Registrations.h"
#include "RegistrationCensus.h"
#include "ReleaseGuard.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Diagnostics
{
	void InstallEngine()
	{
		const auto& o = Observe();
		// CS_DCLF_RELEASE_GUARD: the engine's last releases on DCLF's threads counted.
		ReleaseGuard::Install();
		// CS_DCLF_ACCUMULATED_PARITY: the engine's kAccumulated sampled for the decode's comparison, the animation gate, the frame camera.
		if (o.accumulated)
			Accumulated::Install();
		// The primary cull's observers: the cut, the stand-in walk, the light masks and the scene lists (CS_DCLF_PERSISTENT_PARITY), the
		// fade port and node snapshots (CS_DCLF_FADE_PARITY).
		CutObservers::Install(o.scene, o.fade);
		// CS_DCLF_PERSISTENT_PARITY: the point lights' culls on the frames they skip nothing.
		if (o.scene)
			LightCulls::Install();
		// CS_DCLF_CENSUS: the passes the registration hooks hand on to the engine.
		RegistrationCensus::Install();
		// The light selection against the engine's (both parities; the portal views' are Features/Portals', the shadow views' their
		// sources' features': their install, as is the sun exclusion's watch).
		if (o.claimsOrScene)
			LightSelectionParity::Install();
		// CS_DCLF_SET_PARITY: the members' withheld forward and refraction passes (the particle members' and the Effect Lighting ones are their
		// features').
		if (o.claims)
			Registrations::Install();
	}
}
