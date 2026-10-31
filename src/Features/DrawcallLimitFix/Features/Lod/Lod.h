#pragma once

// Lod (CS_DCLF_LOD_OBJECTS, CS_DCLF_LOD_TERRAIN; dclf-architecture.md, "LOD"): the terrain manager's LOD blocks under TES::lodLandRoot DCLF draws, object
// LOD (a BSSubIndexTriShape with the LOD object flags, drawn by its visible segments: the LODObjects and LODObjectHD techniques) and terrain
// LOD (a land block's shape: the LODLand and LODLandNoise techniques). The feature registers their switches and live toggles, object LOD's
// segment events (LodSegmentEvents.h: the writers' hooks at its early hooks, an engine channel applied after the switch events, the
// component that keeps Core's drawn ranges of the shapes up to date, its report line and parity), and the two object classes'
// contributions (Api::ObjectClass::LodObjects, LodTerrain, Api/ObjectContributors.h) with their techniques' and the sub-index shapes'
// (Api::TechniqueContribution, GeometryTypeContribution: the LOD flags and the geometry type each class's objects are), which Core reads as
// data: the Lighting port's techniques and its LOD flags' eligibility (Ineligible::Lod: Extensions::PropertyFlagRules), the sub-index
// shapes' eligibility, the LOD root listed as a category node (the classes' categoryRoot, TES::lodLandRoot), and a view's LOD roots
// drawn in the engine's place (the view root contribution, LodRootsOwned: the cube camera's LOD land and objects roots, while both
// classes are on and the swaps gated; IndirectDraws::ViewLodRootsOwned reads it). And the LOD gates (CS_DCLF_LOD_GATES): the engine's
// LOD swaps held until DCLF's claims cover the incoming level (LodGates.h: the swap thunks and detours, installed by the feature's
// install, the show veto, the render thread's flip; LodGateComponent.h: the gate events on a SceneStack channel, the coordinator
// component's marks, vetoes, settle, publication and adoption, its report lines, observers and parity).
//
// The feature captures object LOD's drawn ranges (its sub-index shapes' contribution's ranges, Api::GeometryTypeContribution: LodSegments.h,
// the segments' layout). Core keeps them for the walk (the coordinator's entryRanges: the attach's capture, the range slots), and keeps
// the generic structures the gates use (Api/EngineEvents.h, Api/CoordinatorComponents.h), the LOD fades, and the shadow and
// previous-transform predicates (IsLodLand, IsLodObject). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Lod
{
	// The engine roots a covered view's faces draw in the engine's place while both classes are on and the swaps gated (its view root
	// contribution; one owner: Features/Manifest.cpp lists it).
	inline constexpr Api::ViewRoot kOwnedRoot = Api::ViewRoot::LodLandAndObjects;

	// LodFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
}
