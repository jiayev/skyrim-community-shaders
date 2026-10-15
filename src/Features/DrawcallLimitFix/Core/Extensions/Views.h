#pragma once

// The view contributions' Core side (Api/Views.h): each engine view's entry, resolved once at InstallFeatures (ResolveViews) from Core's
// own slot and the feature's registration, and what Core derives from them for its per-object paths (the set's phases, the view bits by
// phase). The view machinery's epochs, revisions, frame requests, classification and the builds read them as data.

#include "Features/DrawcallLimitFix/Api/Views.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace DCLF::Api
{
	/** @brief What a view's work is handed at its point (ViewWork::run). */
	class ViewPointContext
	{
	public:
		std::uint32_t frame = 0;  // FrameView's number
		bool failed = false;      // IndirectDraws::Failed: DCLF claims nothing this session
	};
}

namespace DCLF::Extensions
{
	inline constexpr std::size_t kEngineViews = static_cast<std::size_t>(Api::EngineView::Count);
	inline constexpr std::size_t kViewPoints = static_cast<std::size_t>(Api::ViewPoint::Count);

	/**
	 * @brief A view's resolved entry. Core fills what it still keeps of the view (Reflection whole; FirstPerson's set phase, view bit and
	 * shape choices), the feature's registration the rest (FirstPerson's toggle and forward decals).
	 */
	struct ViewEntry
	{
		bool registered = false;               // a feature's registration filled it
		Api::ToggleId toggle;                  // a feature's toggle
		bool ToggleSet::*coreToggle = nullptr;  // a toggle Core keeps (the reflections'), read in place of toggle
		bool forwardDecals = false;
		std::uint8_t phase = 0;      // its set phase (kSet*)
		std::uint32_t viewBit = 0;   // its view bit at the GPU cull (kView*)
		Api::ViewMembership membership = Api::ViewMembership::LodTechniques;
		Api::ViewShapeLife life = Api::ViewShapeLife::KeepLast;
		Api::ViewArea area = Api::ViewArea::Target;
		Api::ViewWithholding withholding = Api::ViewWithholding::OwnRenderers;
		Api::ViewTargetImport targetImport = Api::ViewTargetImport::ColourWithOwnDepth;
		bool coveredInitially = false, coveredOnFailedGraph = false;
		// By ViewPoint: Core's work while it keeps the view's, then the features' (RegisterViewWork), in order.
		std::array<std::vector<void (*)(const Api::ViewPointContext&)>, kViewPoints> work;
	};
	/** @brief Every view's entry, and what Core's per-object paths read of them. */
	struct ViewSet
	{
		std::array<ViewEntry, kEngineViews> views;
		std::uint8_t phases = 0;      // every view's set phase
		std::uint8_t rootPhases = 0;  // the CategoryRoot views' (exclusive phases)
		std::uint8_t lodPhases = 0;   // the LodTechniques views' (beside the main phase)
		std::uint32_t viewBits = 0;   // every view's bit
		// The view bits a set phase bit takes part in (Core's main and shadow phases filled by Core, the views' by their entries), and the
		// same by phase mask: ViewBitsOf's one read.
		std::array<std::uint32_t, 8> viewBitsOfPhaseBit{};
		std::array<std::uint32_t, 256> viewBitsOfPhases{};
	};

	/** @brief Any thread, after InstallFeatures (empty before): the views' entries. */
	const ViewSet& Views();
	/**
	 * @brief Main thread, at the end of InstallFeatures (Registry): the entries resolved from Core's slots, the registered contributions
	 * (by view; EngineView::Count: none) and the registered work. Once.
	 */
	void ResolveViews(std::span<const Api::ViewContribution, kEngineViews> a_views, std::span<const Api::ViewWork> a_work);
	/** @brief Whether Core keeps a_view whole (no feature may register it): the reflection, until its feature takes it. */
	bool CoreKeepsView(Api::EngineView a_view);

	/** @brief Whether DCLF draws a_view under a_toggles (its toggle on; false for none). */
	inline bool ViewOn(const ToggleSet& a_toggles, Api::EngineView a_view)
	{
		const auto& entry = Views().views[static_cast<std::size_t>(a_view)];
		return entry.coreToggle ? a_toggles.*entry.coreToggle : a_toggles.On(entry.toggle);
	}

	/** @brief Whether a decal on a_view's entries is a forward object of its forward part (its contribution's; false for none). */
	inline bool ViewForwardDecals(Api::EngineView a_view)
	{
		return Views().views[static_cast<std::size_t>(a_view)].forwardDecals;
	}
}
