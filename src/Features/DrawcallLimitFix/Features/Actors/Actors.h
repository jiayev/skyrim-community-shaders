#pragma once

// Actors (CS_DCLF_ACTORS; dclf-architecture.md, "Trees and actors"): the geometry under an actor's 3D (bodies, armour, heads) and the actor
// techniques (actor skin, faces, hair, eyes), DCLF's while its toggle is on. The feature registers their switch and live toggle (which
// Features/FirstPerson's needs), the actor parent's contribution (Api::EngineParent::Actor: the classification's chain
// test reads it as data) and the actor techniques' contributions (the Lighting port's eligibility reads them as data).
// Whatever the toggle, it also owns:
// - the actor-owned entry class (Api::EntryClass::ActorOwned, an owner class by the ActorCharacter form type: per frame, its own motion,
//   published), which Core's walk resolves and reads as data;
// - the actors' engine events, at its early hooks (ActorEvents.cpp): Actor::SetAlpha's fadeAmount writes into Core's fadeAmount events
//   (the "fade amounts" engine channel, needed by name: Extensions::PushChannelKey), and the move writers of Actor::UpdateActor3DPosition, the
//   sky cell's skin job and the ragdoll job on reserved move-writer slots (Extensions::ReserveMoveWriter), which every mover's placement
//   follows;
// - the actors' animation stamp from DCLF's shadow views (ActorRate.cpp: the stand-in observer's afterWrites) and its report line
//   (ReportPoint::AccumulatedWrites).
//
// Features/Skins decides which skinned geometry, any NiSkinInstance's (creatures, effects, LOD trees, the first-person arms), is DCLF's:
// the skinned and skin partitions switches, toggles and object classes.
// Core keeps the path an actor's objects take whatever the toggle:
// - the skinned geometry's path once its class is on: the bone palettes FrameValues samples into the palette part it shares with the
//   particles, the skin partitions and their LOD in the tables and the walk, kObjectPaletteBlock and the vertex stages' bones;
// - an owner class's entry's classification: Tracked::owner, the per-frame owner class trait and its verdict recheck (Coordinator::
//   FrameVerdictKept), SceneTables::classMembers, the mirror's actor bit (NodeRecord::actor);
// - the animation path's move writer (Core/Engine/Capture/SceneEvents: FUN_14066afa0, every reference's animation) and the fadeAmount channel;
// - Advanced Skin's coupling is Integrations/AdvancedSkin's (its wetness: a shading field of the published actor-owned class; FrameValues'
//   fan-out into the shading rows' SkinPerGeometry); its material textures (t71/t74) are its After material contributor (SkinTextures.cpp).
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Actors
{
	// ActorsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
	// ActorEvents.cpp: the early hooks' engine events (SetAlpha, the reserved move writers), and the fade amounts channel SetAlpha pushes
	// into, needed by name from the install.
	void InstallEngineEvents();
	void NeedFadeAmounts(Api::InstallContext& a_context);
	// ActorRate.cpp: the animation stamp's stand-in observer and report line.
	void RegisterActorRate(Api::InstallContext& a_context);
}
