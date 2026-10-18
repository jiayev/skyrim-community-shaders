#pragma once

// The view contribution contract (Api::InstallContext::RegisterView, RegisterViewWork): a view of the engine's that Core's view machinery
// can draw in its place (its scene set, its view bit at the GPU cull, its epochs and their revisions) but leaves to the engine until a
// feature takes it, under the feature's toggle; the feature's work at the view's engine points; and the engine roots a covered view's faces
// draw in the engine's place (RegisterViewRoot).

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Api/ViewCommon.h"

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Core-defined (Core/Extensions/Views.h): what a view's callbacks are handed (its work's context: Api/ViewCommon.h).
	class ViewCoverage;
	class ViewRootCapture;
	class ViewAccumulators;

	/**
	 * @brief An engine view Core's view machinery draws once a feature takes it. The order is Core's slot order (Core/Render/Views/
	 * ViewSlots.h): its epochs, latches, revision key groups, coverage and setup run in it.
	 */
	enum class EngineView : std::uint8_t
	{
		// The water reflection's cube faces: the LOD techniques of the main set, one epoch a frame after the engine's face loop drawing them
		// into the engine's cube, and tree LOD per face (Features/Reflection: its set phase and view bit, renderers and engine hooks).
		Reflection,
		// The player's first-person skeleton (arms, weapons, shield, magic hands): a depth epoch in RenderDepth and a colour epoch at
		// Main::RenderFirstPersonView, and their forward part (Features/FirstPerson: its set phase, view bit and engine hooks).
		FirstPerson,
		Count,
	};

	/** @brief How a view's entries join its set phase. */
	enum class ViewMembership : std::uint8_t
	{
		CategoryRoot,   // entries filed under the view's category root only: an exclusive phase (the main mask is the view bit alone)
		LodTechniques,  // a main-eligible slot whose pipeline is a LOD Lighting technique, beside the main phase
	};
	/** @brief How a view's pipeline slots map to its forward keys (Core's ViewForwardKey, per slot). */
	struct ViewKeyRule
	{
		bool lodTechniquesOnly = false;           // only a slot whose pass is a LOD Lighting technique has a key
		bool forwardRasterToForwardPart = false;  // a forward slot has none: the view's forward part draws it (the main set's pipeline)
		// The ForwardPipelineKey flags its keys carry: the view's own bits (the feature's, above Core's two-sided and tree LOD bits; none
		// for at most one view), by which the pipeline lane finds the key's view (its forward targets, DrawPipelines::SetViewForwardTargets).
		std::uint32_t keyFlags = 0;
		// Its keys cull back faces unless two-sided (a plain projection); else front faces (a cube face's projection mirrors the image).
		bool cullBackFaces = false;
	};
	/**
	 * @brief A view's shape: what tells its epochs apart (Core's ViewTraits, with its epochs and segments from the schedule: Schedule/Frame.cpp).
	 *
	 * - faces: the faces each epoch draws (at most Core's kMaxViewFaces), each with a camera and a range of the sequences.
	 * - trees: tree LOD drawn per face (the DrawFamily::TreeInstances family's faces, Api/DrawFamilies.h). forwardPart: a forward
	 *   range, sorted by the depth epoch and drawn after the buckets by the colour epoch. depthPass: the depth epoch draws the buckets'
	 *   depth pipelines into the main depth (the colour epoch then their colour ones).
	 * - viewBits: the view's bit at the GPU cull (the feature's kView*, checked by Features/Manifest.cpp).
	 * - name (the log), prefix (its graph resources', passes' and extension's identifiers), blocks (its blocks' resource) and target (its
	 *   target's resource); labels: its latch writes, as FrameData counts them. Copied at install.
	 * - depthRange: the viewport's maximum depth until the view's work sets one (IndirectDraws::SetViewDepthRange).
	 * - frustumCull: the view's cull tests each member's bound against its faces' frustums; false draws every member of the view
	 *   untested (a view of a handful of objects always before its camera: first person, whose skinned bounds did not follow its
	 *   camera in every orientation).
	 */
	struct ViewShape
	{
		std::uint32_t faces = 1;
		bool trees = false;
		bool forwardPart = false;
		bool depthPass = false;
		std::uint32_t viewBits = 0;
		std::string_view name, prefix, blocks, target;
		struct Labels
		{
			std::string_view map, tables, latches, blocks, trees;
		} labels;
		float depthRange = 1.0f;
		bool frustumCull = true;
	};
	/** @brief Whether a view's shape stands while its toggle is off (KeepLast) or is dropped from the revision (DropWhenOff). */
	enum class ViewShapeLife : std::uint8_t
	{
		KeepLast,
		DropWhenOff,
	};
	/** @brief A view's render area: its imported target's, or the main colour commit's viewport. */
	enum class ViewArea : std::uint8_t
	{
		Target,
		MainColourViewport,
	};
	/** @brief Where the engine registers a view's members' passes that DCLF withholds: the main accumulators, or renderers of its own. */
	enum class ViewWithholding : std::uint8_t
	{
		InMainRenderers,
		OwnRenderers,
	};
	/** @brief What a view's epochs draw into: an imported colour target with DCLF's own depth, or an imported depth target. */
	enum class ViewTargetImport : std::uint8_t
	{
		ColourWithOwnDepth,
		DepthTarget,
	};

	/**
	 * @brief A view contribution: the view is DCLF's while the toggle is on. Read where the view's epochs run, its revisions are made,
	 * its frame requests are taken and its members are decided, as data (no call per object); its callbacks run once a frame.
	 *
	 * - view: the view, one feature's. toggle: a registered toggle.
	 * - forwardDecals: a decal on the view's entries is a forward object of the view's forward part, with its decal group's depth bias,
	 *   test and blend (the classification's; read as data, decided once at install), rather than the engine's.
	 * - phase: the view's set phase (the feature's SetPhase bit, checked by Features/Manifest.cpp): its members' claims.
	 * - membership: how entries join the phase. CategoryRoot: the entries under the root captureRoot names, the phase exclusive (their
	 *   main mask the view bit alone; the classification's hidden chain stops at the root, whose kHidden is the view's).
	 * - keys, shape: the view's forward keys and its epochs' shape. life: whether its shape stands while the toggle is off.
	 * - area, withholding, targetImport: its render area, where the engine's registrations of its members are withheld (InMainRenderers:
	 *   the accumulators the callback names; OwnRenderers: the renderers the feature's engine hooks name as the engine renders the view,
	 *   while they say its members are withheld there: PassCapture::SetViewRenderers, SetViewWithholdActive), and what it draws into.
	 * - coveredOnFailedGraph: whether its members are withheld on a failed graph. coveredInitially: before the first coverage.
	 * - covered (required): render thread, at the frame's start (DecideCoverage), once a frame, the views in EngineView order: whether its
	 *   epochs draw its members this frame (then the engine's registrations of them are withheld).
	 * - captureRoot (CategoryRoot, required): render thread, once a frame while the toggle is on (the category capture and its
	 *   signatures, and FrameGlobals' capture of the view's subtree, Core/Shading/FrameGlobals.cpp): the view's root, a category node filed
	 *   under itself, and the engine pointers it is found from.
	 * - accumulators (InMainRenderers, required): render thread, once a frame (PassCapture::RefreshMainRenderers): the engine accumulators
	 *   its members register into.
	 * - residueDue (OwnRenderers, optional): render thread, once per render of the view that would leave its LOD roots to DCLF
	 *   (IndirectDraws::ViewLodRootsOwned): whether this one keeps them, its renderers' passes DCLF does not withhold under them watched
	 *   (PassCapture::WatchViewResidue, taken by TakeViewResidue). A diagnostics question: false on the normal path.
	 */
	struct ViewContribution
	{
		EngineView view = EngineView::Count;
		ToggleId toggle;
		bool forwardDecals = false;
		std::uint8_t phase = 0;
		ViewMembership membership = ViewMembership::LodTechniques;
		ViewKeyRule keys;
		ViewShape shape;
		ViewShapeLife life = ViewShapeLife::KeepLast;
		ViewArea area = ViewArea::Target;
		ViewWithholding withholding = ViewWithholding::OwnRenderers;
		ViewTargetImport targetImport = ViewTargetImport::ColourWithOwnDepth;
		bool coveredInitially = false, coveredOnFailedGraph = false;
		bool (*covered)(const ViewCoverage& a_coverage) = nullptr;
		void (*captureRoot)(ViewRootCapture& a_capture) = nullptr;
		void (*accumulators)(ViewAccumulators& a_accumulators) = nullptr;
		bool (*residueDue)() = nullptr;
	};

	/**
	 * @brief A feature's work for a view at an engine point (Api/ViewCommon.h's ViewPoint): run there once a frame, render thread, the
	 * views in EngineView order and a view's work in registration order (the view's feature's). Runs whatever the view's toggle: the work
	 * reads it.
	 */
	struct ViewWork
	{
		EngineView view = EngineView::Count;
		ViewPoint point = ViewPoint::Count;
		void (*run)(const ViewPointContext& a_context) = nullptr;
	};

	/**
	 * @brief An engine root a covered view's faces can draw in the engine's place, its add-root skipped so the engine neither culls nor
	 * registers its members: one owner each, a draw family's faces (Api::DrawFamilyContribution::faces.root) or a view root contribution.
	 */
	enum class ViewRoot : std::uint8_t
	{
		LodLandAndObjects,  // the LOD land and objects roots (the terrain manager's LOD blocks)
		LodTrees,           // the LOD trees root (tree LOD's instanced shapes)
		Count,
	};
	/** @brief a_root's bit in a view's owned roots (the roots' word the view's engine hooks read). */
	inline constexpr std::uint32_t ViewRootBit(ViewRoot a_root) { return 1u << static_cast<std::uint32_t>(a_root); }

	// Core-defined (Core/Extensions/Views.h): what a root's owned callback is handed - the view and the active toggles.
	class ViewRootFrame;

	/**
	 * @brief A view root contribution (Api::InstallContext::RegisterViewRoot): a root whose members a covered view's faces draw while
	 * owned says so. owned: render thread, once per plain render of a covered, drawable view (where the view's owned roots are decided),
	 * handed the view and the active toggles; read of data the feature registered (no engine read).
	 */
	struct ViewRootContribution
	{
		ViewRoot root = ViewRoot::Count;
		bool (*owned)(const ViewRootFrame& a_frame) = nullptr;
	};

	// Core-defined (Core/Extensions/Views.h): where a cull hidden source lists its nodes.
	class CullHiddenNodes;

	/** @brief How a cull hidden source's nodes are listed: with their hidden bit as it is now, or hidden (their bit not read). */
	enum class CullHiddenBit : std::uint8_t
	{
		AsNow,
		Hidden,
	};

	/**
	 * @brief A cull hidden source (Api::InstallContext::RegisterCullHiddenSource): engine nodes whose hidden bit the engine changes around
	 * the scene's views within the frame, which the frame's capture lists with the bit the views see (FrameGlobals::cullHidden, read by the
	 * membership in place of the bit now: Coordinator::HiddenForWalk). nodes: render thread, at the frame's start (FrameGlobals::Capture),
	 * once a frame off a loading screen, in registration order: its nodes (CullHiddenNodes::Add), each listed with bit's. The list is
	 * sorted after every source: the order is not a result.
	 */
	struct CullHiddenSource
	{
		CullHiddenBit bit = CullHiddenBit::AsNow;
		void (*nodes)(CullHiddenNodes& a_nodes) = nullptr;
	};
}
