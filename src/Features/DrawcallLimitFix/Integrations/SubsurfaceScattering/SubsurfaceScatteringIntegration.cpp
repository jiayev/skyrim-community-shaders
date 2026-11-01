// Subsurface Scattering: the "beast race" reference fact and the IsBeastRace object flag. Both read the feature live: the fact on the
// capturing thread, the flag on the join pool.

#include "Features/DrawcallLimitFix/Integrations/SubsurfaceScattering/SubsurfaceScattering.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include "Features/SubsurfaceScattering.h"
#include "ShaderCache.h"

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

		void Commit(const Api::CommitContext& a_commit)
		{
			auto& sss = globals::features::subsurfaceScattering;
			if (a_commit.kind != Api::CommitKind::Colour || !sss.loaded || sss.validMaterials)
				return;
			// Indirect skin draws bypass SetupSkin, which normally enables the post-process.
			// Inspect the small used pipeline set, not every actor or scene object. The mask
			// still restricts scattering to skin pixels that survived the GPU's visibility tests.
			using Technique = SIE::ShaderCache::LightingShaderTechniques;
			SceneTables::ForEachBit(a_commit.tables.usedPipelineBits, [&](std::uint32_t slot) {
				const auto descriptor = a_commit.tables.pipelines[slot].pixelDescriptor;
				const auto technique = static_cast<Technique>((descriptor >> 24) & 0x3fu);
				if ((descriptor & static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::Deferred)) &&
					(technique == Technique::Facegen || technique == Technique::FacegenRGBTint))
					sss.validMaterials = true;
			});
		}

		void Install(Api::InstallContext& a_context)
		{
			// True for a node without a reference, as the record's default was.
			beastFact = a_context.RegisterReferenceFact({ .name = "beast race", .none = true, .sample = &BeastOf });
			a_context.RegisterLightingObjectFlag({ .objectFlag = kObjectBeastRace, .recordFlag = kRecordBeastRace, .applies = &BeastRaceFace });
			a_context.RegisterCommitHook({ .name = "subsurface scattering materials", .commit = &Commit });
		}
	}

	Api::Feature Integration()
	{
		return { "subsurface scattering", &Install };
	}
}
