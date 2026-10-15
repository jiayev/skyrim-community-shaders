#pragma once

// The fade roots (CoordinatorFades): seed requests, owned roots and the roots' references. Residents.cpp.
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorFades
	{
		// Fade root seed requests (into the deltas); the mirror gives a row's coordinator half, the seed the rest.
		std::vector<FadeSeedItem> fadeSeedRequests;
		// ApplyFadeRootsOwned's set (kFadeRootOwned), by node: whether each is stood in (kFadeRootStoodIn).
		ankerl::unordered_dense::map<const void*, bool> fadeRootOwned;
		std::vector<RE::NiPointer<RE::NiAVObject>> fadeRootOwners;
	};
}
