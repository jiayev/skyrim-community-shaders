#pragma once

// FirstPerson (CS_DCLF_FIRST_PERSON, CS_DCLF_FIRST_PERSON_DECALS; drawcall-limit-fix.md, "First person as a view bit"): the player's
// first-person skeleton (arms, weapons, shield, magic hands) drawn by DCLF as a view of the one scene, while its toggle is on. The feature
// registers its switches and live toggle (which needs the skinned and Features/Actors' actors), the view's contribution
// (Api::EngineView::FirstPerson: the view machinery reads its toggle and its forward decals as data), the view's report lines (at the draws'
// async work) and the skeleton's parity (Diagnostics/SkeletonParity.cpp).
//
// Core keeps the view machinery the reflection and the shadow views share:
// - the epochs (Core/Render/Epochs/FirstPerson.cpp: the latch, the targets, the pipelines, the depth and colour epochs, their segments and
//   Tracy zones; ViewEpoch*.cpp: the view's passes and its forward part), their revisions (Revision/: the view's shape, key group and
//   coverage), its resources and state (Impl/Views.h's FirstPersonState, the report's counters: IndirectDraws::FirstPersonReport);
// - the scene's side: the first-person root as a category node (State/Categories.h, ClassifyFrame's category stop and
//   NoteFirstPersonHidden, the misfiled observer), kSetFirstPerson and its decisions (SetParticipation, FirstPersonSlotReady), the view bit
//   kViewFirstPerson, the placements' per-frame chains (Placements.cpp, Walk.cpp), the first-person decals' forward keys
//   (Coordinator::FirstPersonDecalForward), the frame requests' root and the frame globals' culled bits;
// - FrameValues' first-person rows and palettes (taken at the frame's start, the palette marks) and its particles faced to the view's eye;
// - PassCapture's withholding of the members' engine registrations, and the census's first-person view;
// - the shell's two engine hooks (RenderDepth's first-person draw, Main::RenderFirstPersonView's accumulator render), which run the epochs.
// Moving them needs a view contract (see dclf-layout.md, "view kinds"). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::FirstPerson
{
	// FirstPersonFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// Diagnostics/SkeletonParity.cpp: CS_DCLF_SET_PARITY's log of the skeleton's slots and claimed phases, when they change.
	void SubscribeSkeletonParity();
}
