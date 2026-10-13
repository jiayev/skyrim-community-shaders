#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "CaptureParity.h"
#include "HiddenWatch.h"
#include "MirrorWatch.h"
#include "Events.h"
#include "SceneObservers.h"
#include "Features/DrawcallLimitFix/Runtime/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

// The scene area's observers (Coordinator's coordinator, its mirror and its tables), each subscribed only when the run's switches ask
// for it. The order of the subscriptions to one event is the order its observers run in.

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
		if (o.fanout)
			InstallFanoutParity();
		if (o.mirror) {
			InstallMirrorReads();
			InstallMirrorParity();
		}
		if (o.timeline)
			InstallTimeline();
		// Prepass, the render thread: the used sets, the technique rows, the extras rows, the shading.
		if (o.scene) {
			InstallFrameParity();
			InstallTreeLodParity();
			InstallLightMaskCheck();
			InstallSlotDrainCheck();
			InstallCandidateParity();
			InstallDecalOrderParity();
			InstallLodSegmentParity();
			InstallShadowSetParity();
			InstallReplayParity();
		}
		// The capture's drain: the reflection residue (watched under the set or persistent parity and the timeline), the registration
		// parity, the decal order probe.
		InstallRegistrations(o.claimsOrScene || o.timeline, o.scene, o.decalOrderProbe);
		// After the joins: the resident parity, then the slot probe.
		if (o.residents)
			InstallResidentParity();
		if (o.slotProbe)
			InstallSlotProbe();
		// The LOD gates' flips against the claims (the set or persistent parity).
		if (o.claims) {
			InstallFirstPersonFiling();
			InstallCommitParity();
		}
		if (o.claimsOrScene) {
			InstallGateObservers();
			InstallShadowVolumeParity();
		}
		// CS_DCLF_CAPTURE_PARITY: the materials written each frame, for its follow-ups.
		if (o.capture)
			Subscribe<AccumulatePrepared>([](const AccumulatePrepared&) { CaptureParity::Get().DrainWrittenMaterials(); });
		if (MirrorWatch::Enabled())
			InstallMirrorWatchPlacement();
		if (HiddenWatch::Enabled())
			InstallHiddenWatchArming();
		if (SwitchEnabled(Switch::Census))
			InstallCensus();
	}
}
