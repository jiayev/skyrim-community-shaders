#pragma once

// Actors (CS_DCLF_ACTORS; drawcall-limit-fix.md, "Trees and actors"): the geometry under an actor's 3D (bodies, armour, heads) and the actor
// techniques (actor skin, faces, hair, eyes), DCLF's while its toggle is on. The feature registers their switch and live toggle (which
// Features/FirstPerson's needs), the actor parent's contribution (Api::EngineParent::Actor: the classification's chain
// test reads it as data) and the actor techniques' contributions (the Lighting port's eligibility reads them as data).
//
// Core keeps the path an actor's objects take whatever the toggle:
// - the skinned geometry, any NiSkinInstance's (creatures, effects, LOD trees, the first-person arms): the bone palettes FrameValues samples
//   into the palette part it shares with the particles, the skin partitions and their LOD in the tables and the walk, the skinned and skin
//   partitions toggles, kObjectSkinned and the vertex stages' bones;
// - an actor-owned entry's classification: Tracked::actorOwned, the per-frame actor trait and its verdict recheck (Coordinator::
//   ActorRecordKept), SceneTables::actorObjects, the mirror's actor bit (NodeRecord::actor);
// - the move and fade-amount events of the actors' updates (Engine/Capture/SceneEvents: Actor::UpdateActor3DPosition, Actor::SetAlpha, the
//   animation and ragdoll jobs), which every mover's placement follows;
// - the actors' animation stamp from DCLF's shadow views (Engine/Cull/SideEffects; its report line is the accumulated writes');
// - Advanced Skin's coupling: the actors' wetness (SceneTables::actorWetness, FrameView::CaptureWetness, FrameValues' fan-out into the shading
//   rows' SkinPerGeometry) and its material textures (MaterialPortFeature's SkinTextures).
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Actors
{
	// ActorsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
}
