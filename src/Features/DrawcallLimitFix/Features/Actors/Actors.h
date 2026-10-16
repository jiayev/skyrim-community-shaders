#pragma once

// Actors (CS_DCLF_ACTORS; drawcall-limit-fix.md, "Trees and actors"): the geometry under an actor's 3D (bodies, armour, heads) and the actor
// techniques (actor skin, faces, hair, eyes), DCLF's while its toggle is on. The feature registers their switch and live toggle (which
// Features/FirstPerson's needs), the actor parent's contribution (Api::EngineParent::Actor: the classification's chain
// test reads it as data) and the actor techniques' contributions (the Lighting port's eligibility reads them as data).
// Whatever the toggle, it also owns:
// - the actors' engine events, at its early hooks (ActorEvents.cpp): Actor::SetAlpha's fadeAmount writes into Core's fadeAmount events
//   (Extensions::PushFadeAmount; their channel is Core's until Features/Fades), and the move writers of Actor::UpdateActor3DPosition, the
//   sky cell's skin job and the ragdoll job on reserved move-writer slots (Extensions::ReserveMoveWriter), which every mover's placement
//   follows;
// - the actors' animation stamp from DCLF's shadow views (ActorRate.cpp: the stand-in observer's afterWrites) and its report line
//   (ReportPoint::AccumulatedWrites).
//
// Core keeps the path an actor's objects take whatever the toggle:
// - the skinned geometry, any NiSkinInstance's (creatures, effects, LOD trees, the first-person arms): the bone palettes FrameValues samples
//   into the palette part it shares with the particles, the skin partitions and their LOD in the tables and the walk, the skinned and skin
//   partitions toggles, kObjectPaletteBlock and the vertex stages' bones;
// - an actor-owned entry's classification: Tracked::actorOwned, the per-frame actor trait and its verdict recheck (Coordinator::
//   ActorRecordKept), SceneTables::actorObjects, the mirror's actor bit (NodeRecord::actor);
// - the animation path's move writer (Engine/Capture/SceneEvents: FUN_14066afa0, every reference's animation) and the fadeAmount channel;
// - Advanced Skin's coupling: the actors' wetness (SceneTables::actorWetness, FrameView::CaptureWetness, FrameValues' fan-out into the shading
//   rows' SkinPerGeometry) and its material textures (MaterialPortFeature's SkinTextures).
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Actors
{
	// ActorsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
	// ActorEvents.cpp: the early hooks' engine events (SetAlpha, the reserved move writers).
	void InstallEngineEvents();
	// ActorRate.cpp: the animation stamp's stand-in observer and report line.
	void RegisterActorRate(Api::InstallContext& a_context);
}
