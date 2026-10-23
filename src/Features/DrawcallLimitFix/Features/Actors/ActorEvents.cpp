// Actors' engine events (the feature's early hooks, SceneWriterFramework's actor calls): Actor::SetAlpha's fadeAmount writes into the
// fade amounts channel (needed by name at install: InstallContext::NeedEngineChannel), and the actors' move writers Core's own complete
// (MoveEvents, Core/Engine/Capture/SceneEvents.h), on reserved slots of the report's move writers' calls.

#include "Features/DrawcallLimitFix/Features/Actors/Actors.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace DCLF::Features::Actors
{
	namespace
	{
		// The reserved writers (Extensions::ReserveMoveWriter), before their subscriptions go in: slots 2-4, where Core's table had them.
		Api::MoveWriterId actor3DPosition, skyCellSkin, ragdoll;
		// The fade amounts channel, needed at install (bound at the end of InstallFeatures, before the early hooks fire).
		Api::EngineChannelId fadeAmounts;

		/**
		 * @brief Actor::SetAlpha (vfunc 0xE1, 0x1406c1a50; Actor, Character, PlayerCharacter tables): fadeAmount (+0x100) of the 3D
		 * root, and the player's first-person root. PRE records both roots (scratch[0], [1]) and their fadeAmount ([2], [3]).
		 */
		void OnSetAlpha(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			if (SceneWriter::IsPre(a_event)) {
				auto* actor = SceneWriter::Arg<RE::Actor*>(a_event, 0);
				RE::NiAVObject* roots[2]{ actor->Get3D1(false), actor->Get3D1(true) };
				for (std::uint32_t i = 0; i < 2; ++i) {
					const float before = roots[i] ? Engine::At<float>(roots[i], 0x100) : 0.0f;
					a_scratch[i] = reinterpret_cast<std::uintptr_t>(roots[i]);
					std::memcpy(&a_scratch[2 + i], &before, sizeof(before));
				}
				return;
			}
			for (std::uint32_t i = 0; i < 2; ++i) {
				auto* root = reinterpret_cast<RE::NiAVObject*>(a_scratch[i]);
				float before;
				std::memcpy(&before, &a_scratch[2 + i], sizeof(before));
				if (root && Engine::At<float>(root, 0x100) != before && (i == 0 || a_scratch[1] != a_scratch[0]))
					Extensions::PushChannelKey(fadeAmounts, root);
			}
		}

		// The move writers, each pushing its key to the scene pass and to FrameValues' producer (as Core's writers do).
		void OnUpdate3DPosition(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			Extensions::PushReservedMove(actor3DPosition, SceneWriter::Arg<RE::Actor*>(a_event, 0), Api::MoveReach::WithPlacements);
		}

		void OnSkyCellSkin(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			Extensions::PushReservedMove(skyCellSkin, SceneWriter::Arg<RE::Actor*>(a_event, 0), Api::MoveReach::WithPlacements);
		}

		// FUN_140770dc0's list: the actors' handles at +0x30, their count at +0x40.
		void OnRagdollJob(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto* list = SceneWriter::Arg<const std::byte*>(a_event, 0);
			const auto* handles = *reinterpret_cast<const RE::RefHandle* const*>(list + 0x30);
			const auto count = *reinterpret_cast<const std::uint32_t*>(list + 0x40);
			for (std::uint32_t i = 0; handles && i < count; ++i) {
				// Every handle a call; a handle that no longer resolves pushes nothing.
				RE::NiPointer<RE::TESObjectREFR> reference;
				const bool found = RE::LookupReferenceByHandle(handles[i], reference);
				Extensions::PushReservedMove(ragdoll, found ? reference.get() : nullptr, Api::MoveReach::WithPlacements);
			}
		}
	}

	void NeedFadeAmounts(Api::InstallContext& a_context)
	{
		fadeAmounts = a_context.NeedEngineChannel("fade amounts");
	}

	void InstallEngineEvents()
	{
		// SceneWriterFramework's actor calls, which DrawcallLimitFix::PostPostLoad required (SceneWriter::kRequiredCaps).
		if (!SceneWriter::SubscribeCall(SWF_HOOK_ACTOR_SET_ALPHA, &OnSetAlpha))
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused Actor::SetAlpha after reporting it installed");
		// The move writers complete Core's (its install line names them): all or none, so only when Core's are in.
		if (!Extensions::CoreMoveWritersLive())
			return;
		actor3DPosition = Extensions::ReserveMoveWriter("actor 3D position");
		skyCellSkin = Extensions::ReserveMoveWriter("sky cell skin");
		ragdoll = Extensions::ReserveMoveWriter("ragdoll");
		const bool subscribed = SceneWriter::SubscribeCall(SWF_HOOK_ACTOR_UPDATE_3D_POSITION, &OnUpdate3DPosition, SWF_PHASE_POST) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_ACTOR_SKY_CELL_SKIN, &OnSkyCellSkin, SWF_PHASE_POST) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_ACTOR_RAGDOLL_JOB, &OnRagdollJob, SWF_PHASE_POST);
		if (!subscribed)
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused the actors' move writers after reporting them installed");
		for (const auto writer : { actor3DPosition, skyCellSkin, ragdoll })
			Extensions::MoveWriterInstalled(writer);
	}
}
