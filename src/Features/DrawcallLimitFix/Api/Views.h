#pragma once

// The view contribution contract (Api::InstallContext::RegisterView, RegisterViewWork): a view of the engine's that Core's view machinery
// can draw in its place (its scene set, its view bit at the GPU cull, its epochs and their revisions) but leaves to the engine until a
// feature takes it, under the feature's toggle; and the feature's work at the view's engine points.

#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Core-defined (Core/Extensions/Views.h): what a view's callbacks and work are handed.
	class ViewCoverage;
	class ViewRootCapture;
	class ViewAccumulators;
	class ViewPointContext;

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
		std::uint32_t keyFlags = 0;               // the ForwardPipelineKey flags its keys carry (their targets and culling)
	};
	/**
	 * @brief A view's shape: what tells its epochs apart (Core's ViewTraits, with the view's slot: its epochs and segments).
	 *
	 * - faces: the faces each epoch draws (at most Core's kMaxViewFaces), each with a camera and a range of the sequences.
	 * - trees: tree LOD drawn per face. forwardPart: a forward range, sorted by the depth epoch and drawn after the buckets by the colour
	 *   epoch. depthPass: the depth epoch draws the buckets' depth pipelines into the main depth (the colour epoch then their colour ones).
	 * - viewBits: the view's bit at the GPU cull (the feature's kView*, checked by Features/Manifest.cpp).
	 * - name (the log), prefix (its graph resources', passes' and extension's identifiers), blocks (its blocks' resource) and target (its
	 *   target's resource); labels: its latch writes, as FrameData counts them. Copied at install.
	 * - depthRange: the viewport's maximum depth until the view's work sets one (IndirectDraws::SetViewDepthRange).
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
	 *   signatures): the view's root, a category node filed under itself, and the engine pointers it is found from.
	 * - accumulators (InMainRenderers, required): render thread, once a frame (PassCapture::RefreshMainRenderers): the engine accumulators
	 *   its members register into.
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
	};

	/** @brief The engine points a view's work runs at (render thread, while DCLF runs). */
	enum class ViewPoint : std::uint8_t
	{
		FrameStartBeforeCoverage,  // FrameDriver::BeginFrame, after SyncFrameTables, before SunAccumulation::BeginFrame and DecideCoverage
		FrameStartAfterTreeLod,    // FrameDriver::BeginFrame, after DecideTreeLod, before PublishListFilter and FrameValues::Kick
		DepthCommitted,            // Main::RenderDepth's world-drawn point, right after the depth commit (FrameDriver::DepthCommitted)
		Count,
	};
	/**
	 * @brief A feature's work for a view at an engine point: run there once a frame, render thread, the views in EngineView order and a
	 * view's work in registration order (the view's feature's). Runs whatever the view's toggle: the work reads it.
	 */
	struct ViewWork
	{
		EngineView view = EngineView::Count;
		ViewPoint point = ViewPoint::Count;
		void (*run)(const ViewPointContext& a_context) = nullptr;
	};
}
