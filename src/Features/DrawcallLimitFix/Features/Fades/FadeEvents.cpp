// Fades' engine events: the fade snaps and fade amounts engine channels (their queues), and the feature's early hook, the placement
// snap (FadeSnap), which feeds the first.

#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/NodeCaptures.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"  // the fade node's fields (kFadeStatics, kFadeState)

namespace DCLF::Features::Fades
{
	namespace
	{
		// AE 1.6.1170 (module offsets).
		constexpr std::uintptr_t kFadeSnap = 0x147aa20;  // FUN_14147aa20: a fade node's placement snap

		/**
		 * @brief Placement snaps: FUN_14147aa20 (node, camera), from the cell attach (FUN_1402d1280, FUN_1402d5090), sets a fade
		 * node's fade, faded-in flag, snap radius (+0x134) and last visible frame. A root listed before its snap is reseeded from
		 * the node (the fade roots' component's ReseedFadeRoot, FadeRoots.cpp). Identity only; drained after the attach and detach
		 * events, by the "fade snaps" engine channel, the queue its keys are in.
		 */
		// No wake: a snap comes with its cell's attach, whose event wakes the pass.
		EventQueue<const void*> fadeSnapEvents;

		/**
		 * @brief fadeAmount events: Actor::SetAlpha (vfunc 0xE1, 0x1406c1a50) wrote an actor's 3D root's fadeAmount (+0x100)
		 * (FadeRootStatic::fadeAmount). Actor::UpdateAlpha calls it each frame of a fade (FUN_1406eb630 steps ActorProcess's alpha
		 * 0 to 1 after MoveToHigh, Enable or a resurrection); without it an actor listed mid-fade keeps that alpha. Identity only
		 * (the fade roots' component's RefreshFadeAmount, FadeRoots.cpp, rereads the node). The "fade amounts" engine channel's
		 * queue (pushed through the channel's push, Extensions::PushChannelKey, drained by the channel).
		 */
		// No wake: written every frame of a fade; the next pass takes them.
		EventQueue<const void*> fadeAmountEvents;

		// FUN_14147aa20: a fade node's placement snap. The cell attaches (FUN_1402d1280, FUN_1402d5090) call it after writing the
		// fade statics (LOD type `and`/`or` at +0x153, range through FUN_14021f200), so its update carries them. Its key goes to the
		// fade snaps channel's queue (fadeSnapEvents, drained by the channel).
		struct FadeSnap
		{
			static std::uint64_t thunk(RE::NiAVObject* a_node, void* a_camera)
			{
				const auto result = func(a_node, a_camera);
				if (a_node) {
					fadeSnapEvents.Push(a_node);
					if (netimmerse_cast<RE::BSFadeNode*>(a_node))
						Extensions::PushNodeUpdate(a_node, Scene::kFadeStatics | Scene::kFadeState);
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		std::size_t DrainFadeSnaps(Api::KeySink& a_sink)
		{
			return fadeSnapEvents.Drain(a_sink);
		}

		std::size_t DrainFadeAmounts(Api::KeySink& a_sink)
		{
			return fadeAmountEvents.Drain(a_sink);
		}

		// Features/Actors' Actor::SetAlpha, through Extensions::PushChannelKey: the fadeAmount event (the fade roots' component's
		// RefreshFadeAmount, FadeRoots.cpp, rereads the node) and, for a fade node, the mirror's value.
		void PushFadeAmount(const void* a_key)
		{
			auto* root = static_cast<RE::NiAVObject*>(const_cast<void*>(a_key));
			fadeAmountEvents.Push(a_key);
			if (root->AsFadeNode())
				Extensions::PushNodeUpdate(root, Api::kNodeFieldFadeAmount);  // the mirror's value
		}
	}

	void RegisterEngine(Api::InstallContext& a_context, Api::EngineChannelId& a_snaps, Api::EngineChannelId& a_amounts)
	{
		// Drained and applied where Core's were: snaps, then amounts, before the property events (the other BeforeStructural channels
		// after them: none today). Their keys are not named for the mirror's parity (their writers push the mirror's value). No wake: a snap comes with its cell's attach, an amount every frame of a fade.
		a_snaps = a_context.RegisterEngineChannel({ .name = "fade snaps",
			.kind = Api::ChannelKind::Keys,
			.apply = Api::ChannelApply::BeforeStructural,
			.drain = &DrainFadeSnaps });
		a_amounts = a_context.RegisterEngineChannel({ .name = "fade amounts",
			.kind = Api::ChannelKind::Keys,
			.apply = Api::ChannelApply::BeforeStructural,
			.drain = &DrainFadeAmounts,
			.push = &PushFadeAmount });
	}

	void InstallEarlyHooks()
	{
		// The snap's detour, where Core's scene events installed it (no other hook shares its site), and the note the primary cull's
		// install requires (Extensions::RootStatesInstalled): without Fades it stays off with its warning.
		stl::detour_thunk<FadeSnap>(REL::Offset(kFadeSnap).address());
		Extensions::NoteRootStatesInstalled();
	}
}
