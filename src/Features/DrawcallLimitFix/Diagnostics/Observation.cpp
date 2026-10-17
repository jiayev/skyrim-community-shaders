// Observe(): the run's Observation, read from the switches once on first use.
#include "Observation.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		Observation Read()
		{
			Observation o;
			o.scene = SwitchEnabled(Switch::PersistentParity);
			o.claims = SwitchEnabled(Switch::SetParity);
			o.claimsOrScene = o.claims || o.scene;
			o.walk = SwitchEnabled(Switch::WalkParity);
			o.walkOrScene = o.walk || o.scene;
			o.changeLog = SwitchEnabled(Switch::ChangeLogParity);
			o.residents = SwitchEnabled(Switch::ResidentParity);
			o.builds = SwitchEnabled(Switch::BuildParity);
			o.mirror = SwitchEnabled(Switch::MirrorParity);
			o.capture = SwitchEnabled(Switch::CaptureParity);
			o.engineDrawsAll = SwitchEnabled(Switch::ParityBoth);
			o.capturePoint = SwitchEnabled(Switch::CapturePointParity);
			o.accumulated = SwitchEnabled(Switch::AccumulatedParity);
			const auto& foliage = SwitchValue(Switch::FoliageParity);
			o.foliage = foliage == "1" || foliage == "wind" || foliage == "land";
			o.foliageFrozen = foliage == "1" || foliage == "land";
			o.foliageLand = foliage == "land";

			o.stats = SwitchEnabled(Switch::Stats);
			o.passStats = SwitchEnabled(Switch::PassStats);
			o.profile = SwitchEnabled(Switch::Profile);
			o.releaseGuard = SwitchEnabled(Switch::ReleaseGuard);
			o.frameTrace = SwitchEnabled(Switch::FrameTrace);
			o.targetProbe = SwitchValue(Switch::TargetProbe);
			return o;
		}
	}

	const Observation& Observe()
	{
		static const Observation observation = Read();
		return observation;
	}
}
