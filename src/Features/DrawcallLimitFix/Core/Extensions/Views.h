#pragma once

// The view contributions' Core side (Api/Views.h): each engine view's entry, resolved once at InstallFeatures (ResolveViews) from Core's
// own slot and the feature's registration, and what Core derives from them for its per-object paths (the set's phases, the view bits by
// phase, the root views, the decision and withholding orders). The view machinery's epochs, revisions, frame requests, classification,
// set decisions, pass capture and the builds read them as data; the contexts the views' callbacks and work are handed are defined here.
// The view engine's calls for a feature's work and epochs (its state, targets, pipelines, inputs and epoch submission) are
// IndirectDraws' (Core/Render/IndirectDraws.h, "The view engine").

#include "Features/DrawcallLimitFix/Api/Views.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace RE
{
	class NiNode;
}

namespace DCLF
{
	struct SceneTables;
	struct PipelineCatalog;
}

namespace DCLF::Api
{
	/** @brief What a view's work is handed at its point (ViewWork::run). */
	class ViewPointContext
	{
	public:
		std::uint32_t frame = 0;                     // FrameView's number
		bool failed = false;                         // IndirectDraws::Failed: DCLF claims nothing this session
		const SceneTables* tables = nullptr;         // the frame's installed tables (FrameView::GetTables)
		const PipelineCatalog* catalog = nullptr;    // the frame's catalog (FrameCatalog)
	};

	/**
	 * @brief What a view's coverage is decided from (ViewContribution::covered), precomputed by Core at the frame's start (DecideCoverage),
	 * side-effect free.
	 *
	 * - main: the main epochs cover the frame (its claims stand). on: the view's toggle (Extensions::ViewOn).
	 * - epochsCovered: each of the view's epochs has its recording in the adopted revision, of the graph as built now.
	 * - targetImported: the view's resources exist with their target imported; rebuildPending: those resources wait for their next
	 *   import (the main resources changed).
	 * - mainResourcesCurrent: the view's resources draw from the current main resources, with the engine's second colour target imported.
	 * - rebuildRequested: the graph asked for a rebuild (it is not built as the revision recorded it).
	 * - frameBeforeColourHeld: the main resources' depth and colour commits are of one frame, this one or the one before, of the adopted
	 *   set, after the installed payload (what a view drawing the frame before's colour inputs needs).
	 * - frame: FrameView's number.
	 */
	class ViewCoverage
	{
	public:
		bool main = false;
		bool on = false;
		bool epochsCovered = false;
		bool targetImported = false;
		bool rebuildPending = false;
		bool mainResourcesCurrent = false;
		bool rebuildRequested = false;
		bool frameBeforeColourHeld = false;
		std::uint32_t frame = 0;
	};

	/**
	 * @brief A CategoryRoot view's root (ViewContribution::captureRoot): the category node its entries are filed under (itself filed under
	 * itself; null: none this frame), and the engine pointers it is found from, which the category signatures fold in (inputCount of them;
	 * none: nothing to fold).
	 */
	class ViewRootCapture
	{
	public:
		RE::NiNode* root = nullptr;
		std::array<const void*, 2> inputs{};
		std::uint32_t inputCount = 0;
	};

	/** @brief An InMainRenderers view's engine accumulators (ViewContribution::accumulators): count of them, a null one allowed. */
	class ViewAccumulators
	{
	public:
		std::array<const void*, 4> accumulators{};
		std::uint32_t count = 0;
		bool operator==(const ViewAccumulators&) const = default;
	};
}

namespace DCLF::Extensions
{
	inline constexpr std::size_t kEngineViews = static_cast<std::size_t>(Api::EngineView::Count);
	inline constexpr std::size_t kViewPoints = static_cast<std::size_t>(Api::ViewPoint::Count);

	/**
	 * @brief A view's resolved entry. Core fills what it still keeps of a view (the reflection whole), a feature's registration the rest
	 * (the first person's).
	 */
	struct ViewEntry
	{
		bool registered = false;               // a feature's registration filled it
		Api::ToggleId toggle;                  // a feature's toggle
		bool ToggleSet::*coreToggle = nullptr;  // a toggle Core keeps (the reflections'), read in place of toggle
		bool forwardDecals = false;
		std::uint8_t phase = 0;      // its set phase (kSet*: a feature's own bit, or Core's)
		std::uint32_t viewBit = 0;   // its view bit at the GPU cull (kView*: likewise)
		Api::ViewMembership membership = Api::ViewMembership::LodTechniques;
		Api::ViewKeyRule keys;
		// Its shape, the strings owned here (shape's views point into them; Core's ViewTraits too, TraitsOf).
		Api::ViewShape shape;
		std::string name, prefix, blocks, target;
		std::array<std::string, 5> labels;  // map, tables, latches, blocks, trees
		Api::ViewShapeLife life = Api::ViewShapeLife::KeepLast;
		Api::ViewArea area = Api::ViewArea::Target;
		Api::ViewWithholding withholding = Api::ViewWithholding::OwnRenderers;
		Api::ViewTargetImport targetImport = Api::ViewTargetImport::ColourWithOwnDepth;
		bool coveredInitially = false, coveredOnFailedGraph = false;
		// The feature's callbacks (none for a view Core keeps: Core decides its coverage itself).
		bool (*covered)(const Api::ViewCoverage&) = nullptr;
		void (*captureRoot)(Api::ViewRootCapture&) = nullptr;
		void (*accumulators)(Api::ViewAccumulators&) = nullptr;
		// By ViewPoint: Core's work while it keeps the view's, then the features' (RegisterViewWork), in order.
		std::array<std::vector<void (*)(const Api::ViewPointContext&)>, kViewPoints> work;
	};
	/** @brief Every view's entry, and what Core's per-object paths read of them. */
	struct ViewSet
	{
		std::array<ViewEntry, kEngineViews> views;
		std::uint8_t phases = 0;        // every view's set phase
		std::uint8_t rootPhases = 0;    // the CategoryRoot views' (exclusive phases)
		std::uint8_t lodPhases = 0;     // the LodTechniques views' (beside the main phase)
		std::uint32_t viewBits = 0;     // every view's bit
		std::uint32_t rootViewBits = 0; // the CategoryRoot views' bits
		// The CategoryRoot views, in slot order (their roots, their members' claims, their frame values' rows).
		std::array<Api::EngineView, kEngineViews> rootViews{};
		std::uint32_t rootCount = 0;
		// The views the set's decision tests, in its order (Coordinator::DecideSlot): the CategoryRoot views (main-ready first), then the
		// rest, each in slot order.
		std::array<Api::EngineView, kEngineViews> decideOrder{};
		std::uint32_t decideCount = 0;
		// The InMainRenderers views, in slot order (PassCapture: their accumulators' renderers, tested inside WithholdMain).
		std::array<Api::EngineView, kEngineViews> mainRendererViews{};
		std::uint32_t mainRendererCount = 0;
		// The view bits a set phase bit takes part in (Core's main and shadow phases filled by Core, the views' by their entries), and the
		// same by phase mask: ViewBitsOf's one read.
		std::array<std::uint32_t, 8> viewBitsOfPhaseBit{};
		std::array<std::uint32_t, 256> viewBitsOfPhases{};
	};

	namespace Detail
	{
		// Written only by ResolveViews (the main thread, before DCLF's threads exist: their start orders these writes before every
		// read), read-only after: a plain read for the per-object paths.
		inline ViewSet resolvedViews;
	}

	/** @brief Any thread, after InstallFeatures (empty before): the views' entries. */
	inline const ViewSet& Views() { return Detail::resolvedViews; }
	/** @brief Any thread, after InstallFeatures: a_view's entry. */
	inline const ViewEntry& ViewEntryOf(Api::EngineView a_view) { return Views().views[static_cast<std::size_t>(a_view)]; }
	/**
	 * @brief Main thread, at the end of InstallFeatures (Registry): the entries resolved from Core's slots, the registered contributions
	 * (by view; EngineView::Count: none) and the registered work. Fails the install for a phase or view bit a contribution shares with
	 * Core's or another view's. Once.
	 */
	void ResolveViews(std::span<const Api::ViewContribution, kEngineViews> a_views, std::span<const Api::ViewWork> a_work);
	/** @brief Whether Core keeps a_view whole (no feature may register it): the reflection, until its feature takes it. */
	bool CoreKeepsView(Api::EngineView a_view);

	/** @brief Whether DCLF draws a_view under a_toggles (its toggle on; false for none). */
	inline bool ViewOn(const ToggleSet& a_toggles, Api::EngineView a_view)
	{
		const auto& entry = ViewEntryOf(a_view);
		return entry.coreToggle ? a_toggles.*entry.coreToggle : a_toggles.On(entry.toggle);
	}

	/** @brief Whether a decal on a_view's entries is a forward object of its forward part (its contribution's; false for none). */
	inline bool ViewForwardDecals(Api::EngineView a_view)
	{
		return ViewEntryOf(a_view).forwardDecals;
	}
}
