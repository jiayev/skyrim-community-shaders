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
		static void RootDetached(const void* a_key, const Api::Mark& a_mark) { Coordinator::Get().GateRootDetached(a_key, a_mark.token); }
		static void Rescan() { Coordinator::Get().ResetGateTags(); }
		static void UnderMark(const void* a_geometry, const Api::Mark& a_mark)
		{
			Coordinator::Get().TagGateUnder(static_cast<const RE::BSGeometry*>(a_geometry), a_mark);
		}
		static void Settle(bool a_live) { Coordinator::Get().UpdateGates(a_live); }
		static void Take() { Coordinator::Get().TakeGateOutcomes(); }
		static void ApplyOutcomes() { Coordinator::Get().ApplyGateOutcomes(); }
		static void Forgot(const void* a_entry) { Coordinator::Get().UntagGate(static_cast<const RE::BSGeometry*>(a_entry)); }
		static void ForgotAll() { Coordinator::Get().gateTags.clear(); }
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

		// A gate root detached (its mark erased: the coordinator's UnmarkDetached, before the mirror's records go) leaves its gate.
		void RootDetached(const Api::CoordinatorComponentContext&, const void* a_key, const Api::Mark& a_mark)
		{
			GateComponentAccess::RootDetached(a_key, a_mark);
		}

		// The rescan: the gates' tags dropped with the tracked set (its adds tag again).
		void Rescan(const Api::CoordinatorComponentContext&)
		{
			GateComponentAccess::Rescan();
		}

		// A newly tracked geometry under a gate root (the nearest mark on its chain, its prepare's): tagged on the root's side.
		void UnderMark(const Api::CoordinatorComponentContext&, const void* a_geometry, const Api::Mark& a_mark)
		{
			GateComponentAccess::UnderMark(a_geometry, a_mark);
		}

		// The commit's settle (its slots decided, before the set's change is noted and the claims snapshotted): the ready gates flip, their
		// vetoes dropped and set in this commit.
		void Settle(const Api::CoordinatorComponentContext&, bool a_live)
		{
			GateComponentAccess::Settle(a_live);
		}

		// Before the pass's events are collected: the outcomes LodGates posted so far (each after its gate's events, so the batch holds them).
		void Take(const Api::CoordinatorComponentContext&)
		{
			GateComponentAccess::Take();
		}

		// After the batch (AfterBatch, a pass that applies one): the outcomes taken retire their gates.
		void ApplyOutcomes(const Api::CoordinatorComponentContext&, Api::EngineChannelId a_channel, std::span<const void* const>)
		{
			if (a_channel == GateAdoption::channel)
				GateComponentAccess::ApplyOutcomes();
		}

		// A tracked entry erased: off its gate's tag (its veto is the coordinator's, erased with it).
		void Forgot(const Api::CoordinatorComponentContext&, const void* a_entry)
		{
			GateComponentAccess::Forgot(a_entry);
		}

		// The tracked set cleared (the rescan): every tag with it.
		void ForgotAll(const Api::CoordinatorComponentContext&)
		{
			GateComponentAccess::ForgotAll();
		}
	}

	void RegisterCoreGates(Api::InstallContext& a_context)
	{
		// The LOD gates, while their hooks are installed.
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
		// The gates' component: gate events applied in stack order, their roots marked (detached, rescanned, entries tagged under them, the
		// tags forgotten with their entries), their incoming observed and their flipped outgoing held by the commit's vetoes, flips made by
		// the commit's settle and published by its publication, outcomes taken before the batch and applied after it, released by the frame
		// adopting them.
		GateAdoption::component = a_context.RegisterCoordinatorComponent({ .name = "LOD gates",
			.make = &GateAdoption::MakeState,
			.retirement = { .forgot = &Forgot, .forgotAll = &ForgotAll },
			.events = { .keys = &ApplyOutcomes, .stacked = &ApplyGateEvent, .detached = &RootDetached, .rescan = &Rescan, .underMark = &UnderMark },
			.commit = { .settle = &Settle, .take = &Take, .published = &GateAdoption::Published },
			.adoption = { .adopt = &GateAdoption::Adopt } });
	}
}
