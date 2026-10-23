// Actors: the geometry under an actor's 3D and the actor techniques DCLF draws. Their switch and the actors toggle, the actor parent's
// contribution and the actor techniques'; the actor-owned entry class, the actors' engine events at the early hooks and the animation stamp.

#include "Features/DrawcallLimitFix/Features/Actors/Actors.h"

#include "ShaderCache.h"

#include <cstdint>

namespace DCLF::Features::Actors
{
	namespace
	{
		using Technique = SIE::ShaderCache::LightingShaderTechniques;

		void Install(Api::InstallContext& a_context)
		{
			NeedFadeAmounts(a_context);
			// Where Core's were listed: after Features/Skins' skin partitions' (before Features/TreeWind's and Features/Decals', which register after
			// this feature).
			const auto actorsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_ACTORS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: actors stay native (live toggle's seed)", .after = "CS_DCLF_SKIN_PARTITIONS" });
			// Geometry under an actor's 3D, and the actor techniques: an object class. Features/FirstPerson's toggle needs it (the player's
			// skeleton).
			const auto actors = a_context.RegisterToggle({ .name = "actors", .label = "Actors", .seed = actorsSwitch, .entersClassification = true,
				.after = "skin partitions" });
			// Every leaf under an actor's 3D (Coordinator::ClassifyFrame's chain test: Ineligible::Actor while the toggle is off).
			a_context.RegisterParent({ .parent = Api::EngineParent::Actor, .leaves = Api::ParentLeaves::AllLeaves, .toggle = actors });
			// Actor skin (SKIN path, TintColor), faces (Facegen: tint and detail maps at t3/t4), hair (TintColor) and eyes (eye centres,
			// material VS constants): all written by SetupMaterial. A face part's positions are its head's snapshots' (Features/Faces).
			for (const auto technique : { Technique::FacegenRGBTint, Technique::Facegen, Technique::Hair, Technique::Eye })
				a_context.RegisterTechnique({ .technique = static_cast<std::uint32_t>(technique), .toggle = actors });
			// Actor-owned entries (an ActorCharacter's 3D, carried items too), whatever the toggle: per frame (equipment shown, hidden and
			// swapped with no event), moving with no controller or body (the skeleton: Havok's behaviour graph), published (Advanced Skin's
			// wetness). The timing report's "actor".
			a_context.RegisterEntryClass({ .entryClass = Api::EntryClass::ActorOwned, .name = "actor",
				.ownerFormType = static_cast<std::uint8_t>(RE::FormType::ActorCharacter), .everyFrame = true, .ownMotion = true, .published = true });
			// The high actors' animation stamp from DCLF's shadow views, whatever the toggle.
			RegisterActorRate(a_context);
		}
	}

	Api::Feature Feature()
	{
		// The early hooks: right after the reflection's (the first), where Core's scene events installed them before (nothing between
		// patches the same functions or slot).
		return { "actors", &Install, &InstallEngineEvents };
	}
}
