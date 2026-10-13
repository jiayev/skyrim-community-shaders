#include "Observation.h"

#include "Features/DrawcallLimitFix/Runtime/Switches.h"

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
			o.residentDraws = SwitchEnabled(Switch::ResidentDrawParity);
			o.fade = SwitchEnabled(Switch::FadeParity);
			o.builds = SwitchEnabled(Switch::BuildParity);
			o.mirror = SwitchEnabled(Switch::MirrorParity);
			o.bindless = SwitchEnabled(Switch::BindlessParity);
			o.capture = SwitchEnabled(Switch::CaptureParity);
			o.engineDrawsAll = SwitchEnabled(Switch::ParityBoth);
			o.capturePoint = SwitchEnabled(Switch::CapturePointParity);
			o.skylight = SwitchEnabled(Switch::SkylightParity);
			o.fanout = SwitchEnabled(Switch::FanoutParity);
			o.accumulated = SwitchEnabled(Switch::AccumulatedParity);
			const auto& foliage = SwitchValue(Switch::FoliageParity);
			o.foliage = foliage == "1" || foliage == "wind" || foliage == "land";
			o.foliageFrozen = foliage == "1" || foliage == "land";
			o.foliageLand = foliage == "land";

			o.stats = SwitchEnabled(Switch::Stats);
			o.passStats = SwitchEnabled(Switch::PassStats);
			o.profile = SwitchEnabled(Switch::Profile);
			o.timeline = SwitchEnabled(Switch::Timeline);
			o.inputWatch = SwitchEnabled(Switch::InputWatch);
			o.deriveProbe = SwitchEnabled(Switch::DeriveProbe);
			o.coverageProbe = SwitchEnabled(Switch::CoverageProbe);
			o.decalOrderProbe = SwitchEnabled(Switch::DecalOrderProbe);
			o.slotProbe = SwitchEnabled(Switch::SlotProbe);
			o.releaseGuard = SwitchEnabled(Switch::ReleaseGuard);
			o.gbufferProbe = SwitchValue(Switch::GBufferProbe);
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
