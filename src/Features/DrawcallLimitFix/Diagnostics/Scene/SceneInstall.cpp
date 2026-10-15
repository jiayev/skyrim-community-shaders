// The scene area's observers (the coordinator, its mirror and its tables, and the render thread's frame), each subscribed only when
// the run's switches ask for it. The order of the subscriptions to one event is the order its observers run in.

#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "CaptureParity.h"
#include "MirrorWatch.h"
#include "Events.h"
#include "SceneObservers.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Diagnostics
{
	void InstallScene()
	{
		const auto& o = Observe();
		// The change log first in the scene phase, before anything of the pass writes.
		if (o.changeLog)
			InstallChangeLogParity();
		if (o.walk)
			InstallWalkParity();
		if (o.mirror) {
			InstallMirrorReads();
			InstallMirrorParity();
		}
		// Prepass, the render thread: the used sets, the technique rows, the extras rows, the shading.
		if (o.scene) {
			InstallFrameParity();
			InstallSlotDrainCheck();
			InstallCandidateParity();
			InstallShadowSetParity();
			InstallReplayParity();
		}
		// The capture's drain: the reflection residue (watched under the set or persistent parity), the registration parity.
		InstallRegistrations(o.claimsOrScene, o.scene);
		// After the joins: the resident parity.
		if (o.residents)
			InstallResidentParity();
		// (Features/Lod's LOD gate observers, the flips against the claims, follow under either parity, and their parity, after the commit
		// parity, under the persistent one: the feature's install.)
		if (o.claims) {
			InstallFirstPersonFiling();
			InstallCommitParity();
		}
		// (Features/Shadows/Local's shadow volume parity follows under either parity: the feature's install.)
		// CS_DCLF_CAPTURE_PARITY: the materials written each frame, for its follow-ups.
		if (o.capture)
			Subscribe<ScenePoint::AccumulatePrepared>([](const On<ScenePoint::AccumulatePrepared>&) { CaptureParity::Get().DrainWrittenMaterials(); });
		if (MirrorWatch::Enabled())
			InstallMirrorWatchPlacement();
		if (SwitchEnabled(Switch::Census))
			InstallCensus();
	}
}
