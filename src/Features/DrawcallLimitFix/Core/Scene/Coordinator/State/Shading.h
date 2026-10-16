#pragma once

// The shading events (CoordinatorShading): the slots named for the next frame's shading sample, the LOD fade changes and their
// counters. FrameConstants.cpp (NameShadingEvents).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorShading
	{
		// Slots named for the next frame's shading sample, into the next deltas.
		std::vector<ShadingItem> shadingNamed;
		bool shadingNamedAll = false;  // NameShadingEvents named every slot once
		// The shading events: slots resampled, LOD fade and emittance events.
		struct ShadingStats
		{
			std::uint64_t resampled = 0, lodFadeEvents = 0, emittanceEvents = 0;
		} shadingStats;
		std::vector<const void*> lodFadeChanged;
	};
}
