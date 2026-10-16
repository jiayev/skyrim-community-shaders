// Actors' engine events (the feature's early hooks): Actor::SetAlpha's fadeAmount writes into the fade amounts channel (needed by name at
// install: InstallContext::NeedEngineChannel), and the actors'
// move writers Core's own complete (MoveEvents, Engine/Capture/SceneEvents.h), on reserved slots of the report's move writers' calls.

#include "Features/DrawcallLimitFix/Features/Actors/Actors.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"

#include <cstddef>
#include <cstdint>

namespace DCLF::Features::Actors
{
	namespace
	{
		// The reserved writers (Extensions::ReserveMoveWriter), before their thunks go in: slots 2-4, where Core's table had them.
		Api::MoveWriterId actor3DPosition, skyCellSkin, ragdoll;
		// The fade amounts channel, needed at install (bound at the end of InstallFeatures, before the early hooks fire).
		Api::EngineChannelId fadeAmounts;

		/**
		 * @brief Actor::SetAlpha (vfunc 0xE1, 0x1406c1a50; Actor, Character, PlayerCharacter tables): fadeAmount (+0x100) of the 3D
		 * root, and the player's first-person root.
		 */
		template <class T>
		struct ActorSetAlpha
		{
			static void thunk(RE::Actor* a_this, float a_alpha)
			{
				RE::NiAVObject* roots[2]{ a_this->Get3D1(false), a_this->Get3D1(true) };
				float before[2]{};
				for (std::uint32_t i = 0; i < 2; ++i)
					before[i] = roots[i] ? Engine::At<float>(roots[i], 0x100) : 0.0f;
				func(a_this, a_alpha);
				for (std::uint32_t i = 0; i < 2; ++i)
					if (roots[i] && Engine::At<float>(roots[i], 0x100) != before[i] && (i == 0 || roots[1] != roots[0]))
						Extensions::PushChannelKey(fadeAmounts, roots[i]);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct ActorTable;
		struct CharacterTable;
		struct PlayerTable;

		// The move writers, each pushing its key to the scene pass and to FrameValues' producer (as Core's writers do).
		struct ActorUpdate3DPosition
		{
			static void thunk(RE::Actor* a_actor)
			{
				func(a_actor);
				Extensions::PushReservedMove(actor3DPosition, a_actor, Api::MoveReach::WithPlacements);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SkyCellSkin
		{
			static std::uint64_t thunk(RE::Actor* a_actor)
			{
				const auto result = func(a_actor);
				Extensions::PushReservedMove(skyCellSkin, a_actor, Api::MoveReach::WithPlacements);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct RagdollJob
		{
			// FUN_140770dc0's list: the actors' handles at +0x30, their count at +0x40.
			static void thunk(std::byte* a_list)
			{
				func(a_list);
				const auto* handles = *reinterpret_cast<const RE::RefHandle* const*>(a_list + 0x30);
				const auto count = *reinterpret_cast<const std::uint32_t*>(a_list + 0x40);
				for (std::uint32_t i = 0; handles && i < count; ++i) {
					// Every handle a call; a handle that no longer resolves pushes nothing.
					RE::NiPointer<RE::TESObjectREFR> reference;
					const bool found = RE::LookupReferenceByHandle(handles[i], reference);
					Extensions::PushReservedMove(ragdoll, found ? reference.get() : nullptr, Api::MoveReach::WithPlacements);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void NeedFadeAmounts(Api::InstallContext& a_context)
	{
		fadeAmounts = a_context.NeedEngineChannel("fade amounts");
	}

	void InstallEngineEvents()
	{
		// SetAlpha's slot: no other hook takes it (it went in with Core's scene events before; nothing between reads or patches it).
		stl::write_vfunc<0xE1, ActorSetAlpha<ActorTable>>(RE::VTABLE_Actor[0]);
		stl::write_vfunc<0xE1, ActorSetAlpha<CharacterTable>>(RE::VTABLE_Character[0]);
		stl::write_vfunc<0xE1, ActorSetAlpha<PlayerTable>>(RE::VTABLE_PlayerCharacter[0]);
		// The move writers complete Core's (its install line names them): all or none, so only when Core's are in.
		if (!Extensions::CoreMoveWritersLive())
			return;
		actor3DPosition = Extensions::ReserveMoveWriter("actor 3D position");
		skyCellSkin = Extensions::ReserveMoveWriter("sky cell skin");
		ragdoll = Extensions::ReserveMoveWriter("ragdoll");
		constexpr std::uintptr_t kActorUpdate3DPosition = 0x69eb80;  // Actor::UpdateActor3DPosition
		constexpr std::uintptr_t kSkyCellSkin = 0x663430;            // FUN_140663430
		constexpr std::uintptr_t kRagdollJob = 0x770dc0;             // FUN_140770dc0
		stl::detour_thunk<ActorUpdate3DPosition>(REL::Offset(kActorUpdate3DPosition).address());
		stl::detour_thunk<SkyCellSkin>(REL::Offset(kSkyCellSkin).address());
		stl::detour_thunk<RagdollJob>(REL::Offset(kRagdollJob).address());
		for (const auto writer : { actor3DPosition, skyCellSkin, ragdoll })
			Extensions::MoveWriterInstalled(writer);
	}
}
