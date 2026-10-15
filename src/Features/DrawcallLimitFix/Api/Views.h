#pragma once

// The view contribution contract (Api::InstallContext::RegisterView, RegisterViewWork): a view of the engine's that Core's view machinery
// can draw in its place (its scene set, its view bit at the GPU cull, its epochs and their revisions) but leaves to the engine until a
// feature takes it, under the feature's toggle; and the feature's work at the view's engine points.

#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstdint>

namespace DCLF::Api
{
	class ViewPointContext;  // Core-defined (Core/Extensions/Views.h)

	/**
	 * @brief An engine view Core's view machinery draws once a feature takes it. The order is Core's slot order (Core/Render/Views/
	 * ViewSlots.h): its epochs, latches, revision key groups, coverage and setup run in it.
	 */
	enum class EngineView : std::uint8_t
	{
		// The water reflection's cube faces: the LOD techniques of the main set (kSetReflection), the view bit kViewReflection at the GPU
		// cull, one epoch per reflection update after the engine's face loop. Core's for now (its toggle "reflections").
		Reflection,
		// The player's first-person skeleton (arms, weapons, shield, magic hands): the members of kSetFirstPerson, the view bit
		// kViewFirstPerson at the GPU cull, a depth epoch in RenderDepth and a colour epoch at Main::RenderFirstPersonView, and their
		// forward part.
		FirstPerson,
		Count,
	};

	/** @brief How a view's entries join its set phase. */
	enum class ViewMembership : std::uint8_t
	{
		CategoryRoot,   // entries filed under the view's category root only: an exclusive phase (the main mask is the view bit alone)
		LodTechniques,  // a main-eligible slot whose pipeline is a LOD Lighting technique, beside the main phase
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
	 * @brief A view contribution: the view is DCLF's while the toggle is on. Read where the view's epochs run, its revisions are made and
	 * its frame requests are taken, as data (no call per object).
	 *
	 * - view: the view, one feature's.
	 * - toggle: a registered toggle.
	 * - forwardDecals: a decal on the view's entries is a forward object of the view's forward part, with its decal group's depth bias,
	 *   test and blend (the classification's; read as data, decided once at install), rather than the engine's.
	 */
	struct ViewContribution
	{
		EngineView view = EngineView::Count;
		ToggleId toggle;
		bool forwardDecals = false;
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
	 * view's work in registration order (after Core's own, while Core keeps a view's). Runs whatever the view's toggle: the work reads it.
	 */
	struct ViewWork
	{
		EngineView view = EngineView::Count;
		ViewPoint point = ViewPoint::Count;
		void (*run)(const ViewPointContext& a_context) = nullptr;
	};
}
