// Light Limit Fix: the "light limit fix" classify input, the room index (an ancestor value) and the graph buffers at t35-t37. The render
// thread's callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"

#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/ORGLightCulling.h"

#include <cstdint>
#include <span>

namespace DCLF::Integrations::LightLimitFix
{
	namespace
	{
		// Whether Light Limit Fix is loaded. Any thread.
		bool Loaded()
		{
			return globals::features::lightLimitFix.loaded;
		}

		// Light Limit Fix's GetParentRoomNode: the nearest BSMultiBoundRoom or BSPortalSharedNode up the chain, by RTTI.
		const void* RoomRtti()
		{
			return REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSMultiBoundRoom }.get();
		}

		const void* PortalRtti()
		{
			return REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSPortalSharedNode }.get();
		}

		std::uint64_t RoomMapGeneration()
		{
			return globals::features::lightLimitFix.GetRoomMapGeneration();
		}

		// Light Limit Fix's map is the render thread's (it swaps it at its own point): only a copy is posted.
		void RoomMap(Api::AncestorMapSink& a_map)
		{
			const auto& lightFix = globals::features::lightLimitFix;
			a_map.Reserve(lightFix.roomNodes.size());
			for (const auto& [node, index] : lightFix.roomNodes)
				a_map.Put(node, index);
		}

		// Its lights, light index list and light grid, while its ORG culling is active.
		bool GraphBufferIndices(std::span<std::uint32_t> a_out)
		{
			return ORGLightCulling::Get().GetShaderResourceIndices(a_out[0], a_out[1], a_out[2]);
		}

		void Install(Api::InstallContext& a_context)
		{
			// Loaded: its clusters light the effects (Features/Effects' variants) and its rooms key the room index.
			const auto loaded = a_context.RegisterClassifyInput({ .name = "light limit fix", .sample = &Loaded });
			// StrictLightData::RoomIndex: the room's index in its map, taken at a join while the map's generation is behind.
			a_context.RegisterAncestorValue({ .name = "Light Limit Fix's room index", .recordWord = kRoomRecordWord, .none = kNoRoom,
				.ancestorRtti = { &RoomRtti, &PortalRtti }, .whileOn = loaded, .generation = &RoomMapGeneration, .fill = &RoomMap });
			// t35-t37, which the colour passes declare.
			a_context.RegisterFrameGraphBuffers({ .name = "light limit fix", .firstRegister = kLightsRegister,
				.resources = { "cs.llf.lights", "cs.llf.light-index-list", "cs.llf.light-grid" }, .indices = &GraphBufferIndices });
		}
	}

	Api::Feature Integration()
	{
		return { "light limit fix", &Install };
	}
}
