// Subsurface Scattering: the "beast race" reference fact and the IsBeastRace object flag. Both read the feature live: the fact on the
// capturing thread, the flag on the join pool.

#include "Features/DrawcallLimitFix/Integrations/SubsurfaceScattering/SubsurfaceScattering.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"

#include "Features/SubsurfaceScattering.h"

#include <cstdint>

namespace DCLF::Integrations::SubsurfaceScattering
{
	namespace
	{
		// Its bit in the node records' facts (RegisterReferenceFact's, at install, before any capture).
		Api::ReferenceFactId beastFact;

		// The node's reference's race keyword (true without an actor or race, or while the feature or its keyword is missing). Per node
		// capture, inside the engine-read window.
		bool BeastOf(const RE::TESObjectREFR& a_reference)
		{
			const auto& sss = globals::features::subsurfaceScattering;
			if (!sss.loaded || !sss.isBeastRaceKeyword)
				return true;
			if (auto* actor = const_cast<RE::TESObjectREFR&>(a_reference).As<RE::Actor>())
				if (auto* race = actor->GetRace())
					return race->HasKeyword(sss.isBeastRaceKeyword);
			return true;
		}

		/**
		 * @brief Subsurface Scattering's IsBeastRace (BSLightingShader_SetupSkin) for a face property (kFace or kFaceGenRGBTint), from the
		 * node record (BeastOf). Lighting.hlsl reads it only under SKIN.
		 */
		bool BeastRaceFace(const Api::LightingJoinView& a_view)
		{
			using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
			const auto& sss = globals::features::subsurfaceScattering;
			if (!sss.loaded || !sss.isBeastRaceKeyword || !(a_view.property.flags & (static_cast<std::uint64_t>(Flag::kFace) | static_cast<std::uint64_t>(Flag::kFaceGenRGBTint))))
				return false;
			return a_view.Fact(beastFact);
		}

		void Install(Api::InstallContext& a_context)
		{
			// True for a node without a reference, as the record's default was.
			beastFact = a_context.RegisterReferenceFact({ .name = "beast race", .none = true, .sample = &BeastOf });
			a_context.RegisterLightingObjectFlag({ .objectFlag = kObjectBeastRace, .recordFlag = kRecordBeastRace, .applies = &BeastRaceFace });
		}
	}

	Api::Feature Integration()
	{
		return { "subsurface scattering", &Install };
	}
}
