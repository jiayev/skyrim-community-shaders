#pragma once

// Reflection (CS_DCLF_REFLECTIONS; dclf-architecture.md, "Water reflections"): the water reflection's cube map faces drawn by DCLF as a view of the
// one scene, while its toggle is on. The feature registers its switch and live toggle and the view's contribution
// (Api::EngineView::Reflection): its set phase and view bit (kSetReflection, kViewReflection), its membership (the LOD techniques of the
// main set), its key rule and shape (six faces with tree LOD), its life (the last shape kept while off), area (its imported target's),
// withholding (in renderers of its own: the cube camera's accumulators, named by the face render's hook) and target (the engine's cube with
// DCLF's own depth), and its coverage (Covered). Its work after tree LOD's decision (Prepare: the faces' targets, the LOD slots' forward
// pipelines, its resources) and at the depth commit (Execute: the frame's faces, drawn by the depth epoch, which hosts the faces' epoch's
// passes, committed inside it), its engine hooks (ReflectionFaces: the
// update, its LOD add-roots, the face render, its orientation and draws; the feature's early hooks), its state and report counters
// (ReflectionFrame, Report: its lines at the views' async work) and the faces' camera parity (Diagnostics/FaceCameraParity.cpp) are the
// feature's, on the view engine's calls (IndirectDraws.h, "The view engine").
//
// Core keeps what every bucketed view shares, keyed by the view's slot or its membership (Core/Render/Views/ViewSlots.h,
// Core/Extensions/Views.h): the epoch's passes (ViewEpoch*.cpp; the schedule hosts them in the depth epoch), its revisions, the slot's state (Epochs/ViewEpoch.h's
// ViewEpochState), a LodTechniques view's slot readiness and phase readiness, the colour target's import, and PassCapture's withholding in a
// view's own renderers. It keeps a trees view's tree LOD per face, the view engine's under the tree LOD class (Api::ObjectClass::TreeLod,
// Features/TreeLod's): its pipeline, ownership and row (IndirectDraws::ResolveViewFaces, DecideViewFaces and SubmitViewEpoch's rows);
// the cube camera's LOD roots the faces draw in the engine's place (IndirectDraws::ViewLodRootsOwned: the tree root by the faces' tree LOD,
// the LOD land and objects roots by the LOD classes and the LOD gates' show veto, Features/Lod's) and an OwnRenderers view's residue watch
// under them (PassCapture::WatchViewResidue, on the updates the view's residueDue keeps them: the feature's Diagnostics/Residue.cpp, which
// also classifies the residue); and the forward targets by view (DrawPipelines::SetViewForwardTargets: the faces' keys carry no view bit).
// The feature reaches them through the view engine's calls alone, never a LOD feature's headers.
// Included by the feature's sources only (and Features/Manifest.cpp, for the constants).

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <string>

namespace RE
{
	class NiCamera;
}

namespace DCLF::Features::Reflection
{
	// The view's set phase (SceneSet.h's SetPhase bits) and view bit at the GPU cull (GpuLayouts.h's kView*): Features/Manifest.cpp checks
	// them against Core's.
	inline constexpr std::uint8_t kSetReflection = 1u << 5;
	inline constexpr std::uint32_t kViewReflection = 1u << 7;
	// The cube's faces, each epoch's (at most Core's kMaxViewFaces).
	inline constexpr std::uint32_t kFaces = 6;

	// ReflectionFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// ReflectionView.cpp: the view's work after tree LOD's decision (ViewPoint::FrameStartAfterTreeLod), its coverage, and the face hooks'
	// callbacks (a face's camera once the engine oriented it, the face drawn).
	void Prepare(const Api::ViewPointContext& a_context);
	bool Covered(const Api::ViewCoverage& a_coverage);
	// The update's cull hidden source (Api::CullHiddenSource, listed with their bits now): the player's third-person 3D and the water
	// objects' shapes, which TESWaterReflections::Update hides around the cube map's faces.
	void CullHiddenByUpdate(Api::CullHiddenNodes& a_nodes);
	void FaceOriented(const RE::NiCamera& a_camera, std::uint32_t a_face);
	void FaceDrawn();

	// ReflectionEpoch.cpp: the faces' commit at the depth commit, inside it (ViewPoint::DepthCommitted: the depth epoch hosts the faces'
	// passes), and the view's report lines, taking its counters.
	void Execute(const Api::ViewPointContext& a_context);
	std::string Report();

	// ReflectionFaces.cpp (its own header): the engine hooks. Diagnostics/FaceCameraParity.cpp: CS_DCLF_SET_PARITY's or
	// CS_DCLF_PERSISTENT_PARITY's check of the faces' cameras against the engine's at their draws (its counts: ReflectionView.h).
	void SubscribeFaceCameraParity();

	// Diagnostics/Residue.cpp: the residue watch's question (the view's residueDue: every 30th update while CS_DCLF_SET_PARITY or
	// CS_DCLF_PERSISTENT_PARITY observes), the residue's classification by the scene pass that takes the frame's drain (subscribed under
	// either parity), and the report's part of it: the classes since the last report (empty: none).
	bool ResidueDue();
	void SubscribeResidue();
	std::string ResidueKinds();
}
