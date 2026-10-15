// Core's built-in gates (Scene::RegisterCoreGates): registered as Core at the start of InstallFeatures, before every feature, on the
// generic structures a feature's gates would use (Api/EngineEvents.h: SceneStack channel, show veto, adoption guard, reserved hidden
// site; Api/CoordinatorComponents.h: the component's marks, vetoes, commit and adoption hooks).

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LodGates.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/GateAdoption.h"

namespace DCLF::Scene
{
	/** @brief The gates' hooks into the coordinator's own gate state, still Core's (Coordinator's friend until the gates move out). */
	struct GateComponentAccess
	{
		static void ApplyGateEvent(const Api::Stacked& a_event)
		{
			Coordinator::Get().ApplyGateEvent(a_event.token, a_event.pinned, a_event.named, a_event.stampNs, a_event.stampFrame);
		}
	};

	namespace
	{
		// A gate event (ApplyEvents, in stack order: after its incoming blocks' attaches) opens its gate: its incoming roots pinned, its
		// outgoing named.
		void ApplyGateEvent(const Api::CoordinatorComponentContext&, Api::EngineChannelId a_channel, const Api::Stacked& a_event)
		{
			if (a_channel == GateAdoption::channel)
				GateComponentAccess::ApplyGateEvent(a_event);
		}
	}

	void RegisterCoreGates(Api::InstallContext& a_context)
	{
		// The LOD gates, while their hooks are installed. Still to move onto the generic structures: their component's marks and events
		// (b3), commit hooks (b4).
		if (!LodGates::Enabled())
			return;
		// Their events on the scene's stack (in order with the incoming blocks' attaches), each, and each outcome they post, waking a
		// pass. No queue: nothing a pass drains or a load screen carries; applied by the component's stacked hook, never as keys.
		GateAdoption::channel = a_context.RegisterEngineChannel({ .name = "LOD gates",
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
		LodGates::NoteRegistered(GateAdoption::channel, a_context.ReserveHiddenSite());
		// The frames withdraw their claims while the installed publication predates a forced release (an older one may claim the
		// outgoing blocks it detached).
		a_context.RegisterAdoptionGuard({ .name = "LOD gates' forced releases", .generation = &GateAdoption::ForcedGeneration });
		// The gates' component: gate events applied in stack order, flips published by the commit that made them, released by the frame
		// adopting them.
		GateAdoption::component = a_context.RegisterCoordinatorComponent({ .name = "LOD gates",
			.make = &GateAdoption::MakeState,
			.events = { .stacked = &ApplyGateEvent },
			.commit = { .published = &GateAdoption::Published },
			.adoption = { .adopt = &GateAdoption::Adopt } });
	}
}
