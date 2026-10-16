// Advanced Skin's wetness field: its membership hook (the actors' index, from the publications' writes) and its capture (each actor's
// value at the frame's start, fanned out). Render thread. Its material textures are SkinTextures.cpp's.

#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/OwnerValues.h"

#include "Features/Skin.h"

#include <cstdint>

namespace DCLF::Integrations::AdvancedSkin
{
	namespace
	{
		// The capture's actor index: membership from the coordinator's walks (SceneTables::classMembers' changes, via the log); values and
		// fan-out are the render thread's.
		OwnerValues& Owners()
		{
			static OwnerValues owners;
			return owners;
		}

		void Membership(const Api::ShadingFieldMembership& a_membership)
		{
			auto& owners = Owners();
			for (const auto& change : a_membership.changes) {
				if (change.clear)
					owners.Clear();
				else
					owners.Set(change.slot, change.group, change.identity, change.resetValue);
			}
		}

		void Capture(Api::ShadingFieldCapture& a_capture)
		{
			// Advanced Skin's wetness, per actor-owned object. Skin::GetWetness keeps each actor's fading state and computes it once a
			// frame (the first call; later ones, including its own SetupGeometry hook for the draws the engine still makes this frame,
			// return the same value), so calling it here for every actor in the tables advances every actor's fade once a frame,
			// whether or not the engine draws it. Its cache is not thread-safe: the render thread's, at the frame's start.
			ZoneScopedN("CS.DCLF.Capture.Wetness");
			auto& skin = globals::features::skin;
			const bool wetness = skin.loaded && skin.settings.EnableSkin;
			// The actors' membership is the render thread's own index (the log's changes, Membership), their geometries the newest
			// publication's (held by it), never the coordinator's tables.
			const auto geometry = a_capture.objectGeometry;
			const auto wetnessStats = Owners().Update([&](std::uint32_t o) {
				auto* mesh = o < geometry.size() ? geometry[o] : nullptr;
				const float4 value = wetness && mesh ? skin.GetWetness(mesh) : float4{};
				return OwnerValues::Value{ value.x, value.y, value.z, value.w };
			}, [&](std::uint32_t o, const auto& row) { a_capture.Put(o, row); });
			TracyPlot("CS.DCLF.Wetness.Actors", static_cast<std::int64_t>(wetnessStats.actors));
			TracyPlot("CS.DCLF.Wetness.ChangedActors", static_cast<std::int64_t>(wetnessStats.changed));
			TracyPlot("CS.DCLF.Wetness.VisitedMeshes", static_cast<std::int64_t>(wetnessStats.visited));
		}

		void Install(Api::InstallContext& a_context)
		{
			// Features/Actors' class, registered published whatever its toggle.
			a_context.RegisterShadingField({ .name = "wetness", .field = Api::ShadingRowField::SkinPerGeometry, .members = Api::EntryClass::ActorOwned,
				.membership = &Membership, .capture = &Capture });
			// Its t71/t74 hook on SetupMaterial and SkinPerGeometry's per-draw buffer (SkinTextures.cpp).
			InstallMaterialTextures(a_context);
		}
	}

	Api::Feature Integration()
	{
		return { "advanced skin", &Install };
	}
}
