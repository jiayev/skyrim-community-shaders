// Core's built-in gates (Scene::RegisterCoreGates): registered as Core at the start of InstallFeatures, before every feature, on the
// generic structures a feature's gates would use (Api/EngineEvents.h: SceneStack channel, show veto, adoption guard, reserved hidden
// site; Api/CoordinatorComponents.h: the component's marks, vetoes, commit and adoption hooks).

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LodGates.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Gates.h"

namespace DCLF::Scene
{
	void RegisterCoreGates(Api::InstallContext& a_context)
	{
		// The LOD gates, while their hooks are installed.
		if (!LodGates::Enabled())
			return;
		// Their events on the scene's stack (in order with the incoming blocks' attaches), each, and each outcome they post, waking a
		// pass. No queue: nothing a pass drains or a load screen carries; applied by the component's stacked hook, never as keys.
		Gates::channel = a_context.RegisterEngineChannel({ .name = "LOD gates",
			.kind = Api::ChannelKind::SceneStack,
			.apply = Api::ChannelApply::AfterBatch,
			.wake = Api::WakeSource::Pass });
		// The show stores the gates veto while open (the hidden-node source's LOD show stubs), and the engine's own LOD hides their
		// coverage observer is told of (parity).
		a_context.RegisterShowVeto({ .open = &LodGates::OpenGates(),
			.vetoes = &LodGates::Vetoes,
			.engineHid = &LodGates::NoteEngineHide,
			.hideSites = LodGates::kEngineHideSites });
		// Their own stores (a flip, a forced release's unhide) carry a hidden site past the source's.
		LodGates::NoteRegistered(Gates::channel, a_context.ReserveHiddenSite());
		// The frames withdraw their claims while the installed publication predates a forced release (an older one may claim the
		// outgoing blocks it detached).
		a_context.RegisterAdoptionGuard({ .name = "LOD gates' forced releases", .generation = &Gates::ForcedGeneration });
		// The gates' component: gate events applied in stack order, their roots marked (detached, rescanned, entries tagged under them, the
		// tags forgotten with their entries), their incoming observed and their flipped outgoing held by the commit's vetoes, flips made by
		// the commit's settle and published by its publication, outcomes taken before the batch and applied after it, released by the frame
		// adopting them.
		Gates::component = a_context.RegisterCoordinatorComponent({ .name = "LOD gates",
			.make = &Gates::MakeState,
			.retirement = { .forgot = &Gates::Forgot, .forgotAll = &Gates::ForgotAll },
			.events = { .keys = &Gates::ApplyOutcomes, .stacked = &Gates::ApplyGateEvent, .detached = &Gates::RootDetached, .rescan = &Gates::Rescan, .underMark = &Gates::UnderMark },
			.commit = { .settle = &Gates::Settle, .take = &Gates::Take, .published = &Gates::Published },
			.adoption = { .adopt = &Gates::Adopt } });
		// Their lines: the gates' at the coordinator's report (opened, flipped, retired; the open ones' age and what they wait for), first
		// of its gates point's; the releases' and the engine side's at the frame's (render thread), first of the releases point's.
		a_context.RegisterReportLines({ .name = "LOD gates",
			.point = Api::ReportPoint::SceneCoordinatorGates,
			.component = Gates::component,
			.componentText = &Gates::GateLines });
		a_context.RegisterReportLines({ .name = "LOD gate releases", .point = Api::ReportPoint::GateReleases, .lines = &Gates::ReleaseLines });
	}
}
