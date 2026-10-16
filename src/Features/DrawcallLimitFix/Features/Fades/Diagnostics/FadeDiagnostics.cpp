// Fades' diagnostics install (FadesInternal.h's RegisterDiagnostics): the report lines, and the fade parity's observers under its switch
// (CS_DCLF_FADE_PARITY, the feature's: FadeParity, the gate Diagnostics' installs had them under).

#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"
#include "Features/DrawcallLimitFix/Features/Fades/Diagnostics/FadeDiagnostics.h"

namespace DCLF::Features::Fades
{
	void RegisterDiagnostics(Api::InstallContext& a_context)
	{
		RegisterReport(a_context);
		// CS_DCLF_FADE_PARITY: the port against the engine's functions (and the nodes the log compares), then FadeStateCS's log. A main
		// epoch's readbacks: the fade log's after the build parity's, the culling's counters' and the set parity's (Diagnostics' install
		// runs first), and after the features' listed before Fades (Features/TreeWind's tree wind).
		if (FadeParity()) {
			SubscribePortParity();
			SubscribeFadeLog();
		}
	}
}
