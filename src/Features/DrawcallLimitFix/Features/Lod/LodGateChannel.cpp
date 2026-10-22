// The LOD gates' install (the feature's): the engine side's hooks, then their registrations on the generic structures (Api/EngineEvents.h:
// SceneStack channel, show veto, reserved hidden site; Api/CoordinatorComponents.h: the component's marks, vetoes, commit
// and adoption hooks), first of the feature's: their channel and component before the segments'.

#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodGates.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"

namespace DCLF::Features::Lod::Gates
{
	void RegisterGates(Api::InstallContext& a_context, Api::SwitchId a_switch)
	{
		// CS_DCLF_LOD_GATES (on unless 0): the engine's LOD swaps gated on DCLF's claims (skyrim-engine-notes.md, "LOD swaps and their
		// gates"), no gate opening while DCLF does not run. Installed here, after the shell's own engine side, where the shell installed
		// them before the features: no other hook patches their sites or detours their functions (the LOD drain, the retirement, the
		// forced paths), so the move past the earlier features' hooks changes no patch.
		LodGates::Install(a_switch, &ShellState::Running);
		// The LOD gates, while their hooks are installed.
		if (!LodGates::Enabled())
			return;
		// Their events on the scene's stack (in order with the incoming blocks' attaches), each, and each outcome they post, waking a
		// pass. No queue: nothing a pass drains or a load screen carries; applied by the component's stacked hook, never as keys.
		channel = a_context.RegisterEngineChannel({ .name = "LOD gates",
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
		LodGates::NoteRegistered(channel, a_context.ReserveHiddenSite());
		// No adoption guard: a forced release's stores (the pending blocks shown, the held ones released and detached) reach the scene as
		// the hidden-node source's and the tracker's events, and its outcome wakes a pass (Extensions::WakeChannel), so the next publication
		// follows it as any scene change does; the frames keep their claims until then (DCLF never withdraws them).
		// The gates' component: gate events applied in stack order, their roots marked (detached, entries tagged under them, the
		// tags forgotten with their entries), their incoming observed and their flipped outgoing held by the commit's vetoes, flips made by
		// the commit's settle and published by its publication, outcomes taken before the batch and applied after it, released by the frame
		// adopting them.
		component = a_context.RegisterCoordinatorComponent({ .name = "LOD gates",
			.make = &MakeState,
			.retirement = { .forgot = &Forgot },
			.events = { .keys = &ApplyOutcomes, .stacked = &ApplyGateEvent, .detached = &RootDetached, .underMark = &UnderMark },
			.commit = { .settle = &Settle, .take = &Take, .published = &Published },
			.adoption = { .adopt = &Adopt } });
		// Their lines: the gates' at the coordinator's report (opened, flipped, retired; the open ones' age and what they wait for), first
		// of its gates point's; the releases' and the engine side's at the frame's (render thread), first of the releases point's.
		a_context.RegisterReportLines({ .name = "LOD gates",
			.point = Api::ReportPoint::SceneCoordinatorGates,
			.component = component,
			.componentText = &GateLines });
		a_context.RegisterReportLines({ .name = "LOD gate releases", .point = Api::ReportPoint::GateReleases, .lines = &ReleaseLines });
	}
}
