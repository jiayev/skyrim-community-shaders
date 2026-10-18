#pragma once

// FirstPerson (CS_DCLF_FIRST_PERSON, CS_DCLF_FIRST_PERSON_DECALS; dclf-architecture.md, "First person as a view bit"): the player's
// first-person skeleton (arms, weapons, shield, magic hands) drawn by DCLF as a view of the one scene, while its toggle is on. The feature
// registers its switches and live toggle (which needs the skinned and Features/Actors' actors) and the view's contribution
// (Api::EngineView::FirstPerson): its set phase and view bit (kSetFirstPerson, kViewFirstPerson), its membership (a category root: the
// first-person root, CaptureRoot), its key rule and shape (one face, a forward part, a depth pass), its life (dropped while off), area
// (the main colour viewport), withholding (in the main accumulators: Accumulators) and target (kPOST_ZPREPASS_COPY), and its coverage
// (Covered). Its work at the frame's start (BeginFrame: the latch, the targets, the skeleton's walk, the pipelines and resources), its
// two epochs (ExecuteDepth, ExecuteColour) and their engine hooks (InstallHooks: RenderDepth's first-person draw, Main::RenderFirstPersonView's
// accumulator render), its state and report counters (FirstPersonFrame, Report: its lines at the views' async work) and the skeleton's
// parity (Diagnostics/SkeletonParity.cpp) and its scene diagnostics (Diagnostics/RootViewChecks.cpp) are the feature's, on the view
// engine's calls (IndirectDraws.h, "The view engine").
//
// Core keeps what every bucketed view shares, keyed by the view's slot or its membership (Core/Render/Views/ViewSlots.h,
// Core/Extensions/Views.h):
// - the epochs' segments and Tracy zones' passes (ViewEpoch*.cpp: the view's passes and its forward part), their revisions (the
//   shape, key group, coverage inputs), the slot's state (Epochs/ViewEpoch.h's ViewEpochState: targets, resources, depth range, forward
//   compatibility), the forward keys' targets by view (DrawPipelines::SetViewForwardTargets, keyed by kForwardKeyFirstPerson);
// - the scene's side of a CategoryRoot view: its root as a category node (the classification's root stop, its hidden verdicts and
//   misfiled entries announced: RootViewHidden, ViewMisfiled), the set's decisions (SetParticipation, the slot readiness, the
//   first-person decals' forward keys: Coordinator::ViewDecalForward; the left-out slots announced: SetSlotDecided, SetSlotRedecided),
//   the placements' per-frame chains (Placements.cpp, Walk.cpp), the frame requests' root and the frame globals' culled bits
//   (FrameGlobals::NoteFirstPersonCulled; the subtree announced: RootSubtreeSynced);
// - FrameValues' root-view rows and palettes (taken at the frame's start, the palette marks), and its counters of the palette rows
//   classes' first-person writes (Features/Particles faces their quads to the view's eye), which the report reads with no Particles include;
// - PassCapture's withholding of the members' engine registrations (InMainRenderers), and the census's first-person view.
// Included by the feature's sources only (and Features/Manifest.cpp, for the constants).

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::FirstPerson
{
	// The view's set phase (SceneSet.h's SetPhase bits) and view bit at the GPU cull (GpuLayouts.h's kView*; BuildDrawsCS.hlsl's
	// kViewFirstPerson mirrors it): Features/Manifest.cpp checks them against Core's.
	inline constexpr std::uint8_t kSetFirstPerson = 1u << 6;
	inline constexpr std::uint32_t kViewFirstPerson = 1u << 11;
	// The view's forward keys' bit (Api::ViewKeyRule::keyFlags, above Core's ForwardPipelineKey bits): their targets are the first
	// person's (kMAIN + motion vectors, kPOST_ZPREPASS_COPY's depth), every channel written. Its value is in the keys' recipe ids.
	inline constexpr std::uint32_t kForwardKeyFirstPerson = 4;

	// FirstPersonFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// FirstPersonView.cpp: the view's work at the frame's start (ViewPoint::FrameStartBeforeCoverage), and its callbacks.
	void BeginFrame(const Api::ViewPointContext& a_context);
	bool Covered(const Api::ViewCoverage& a_coverage);
	void CaptureRoot(Api::ViewRootCapture& a_capture);
	void Accumulators(Api::ViewAccumulators& a_accumulators);
	// The skeleton's cull hidden source (Api::CullHiddenSource, listed hidden): the first-person 3D, while it is not the third-person one.
	void CullHiddenSkeleton(Api::CullHiddenNodes& a_nodes);

	// FirstPersonEpochs.cpp: the depth epoch (RenderDepth's first-person draw), the colour epoch (Main::RenderFirstPersonView's accumulator
	// render), and the view's report lines, taking its counters.
	void ExecuteDepth(bool a_controlDriven);
	void ExecuteColour();
	std::string Report();

	// FirstPersonHooks.cpp: the two engine hooks that run the epochs (their triggers, fp.depth-draw and fp.colour-render), from the
	// feature's install.
	void InstallHooks(Api::InstallContext& a_context);

	// Diagnostics/SkeletonParity.cpp: CS_DCLF_SET_PARITY's log of the skeleton's slots and claimed phases, when they change.
	void SubscribeSkeletonParity();

	// Diagnostics/RootViewChecks.cpp: the view's scene diagnostics on Core's root view hook points (the subtree's bits, chain and hidden
	// geometry; its entries' hidden verdicts; under CS_DCLF_SET_PARITY the misfiled entries and the slots the set's decisions leave out),
	// subscribed from the install; and their report lines after the set's (ReportPoint::SceneCoordinatorSet, the scene lane).
	void SubscribeRootViewChecks();
	void SetLines(std::uint32_t a_sceneFrame);
}
