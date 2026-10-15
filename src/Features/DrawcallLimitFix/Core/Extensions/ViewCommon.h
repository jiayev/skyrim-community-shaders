#pragma once

// The view families' shared Core side (Api/ViewCommon.h): the set phases and view bits of every feature view of both families (the
// bucketed views, Core/Extensions/Views.h; the occlusion maps, Core/Extensions/ShadowViews.h), checked and resolved once at the end of
// InstallFeatures (ResolveViewBits) into what Core's per-object paths read: the view bits by phase mask (ViewBitsOf's one read) and
// the phases the set's lacking counts are kept by.

#include "Features/DrawcallLimitFix/Api/ViewCommon.h"

#include <array>
#include <cstdint>

namespace DCLF::Extensions
{
	/** @brief The phases and view bits of both families, and what Core derives from them with its own. */
	struct ViewBitTable
	{
		std::uint8_t phases = 0;     // every feature view's set phase, of both families
		std::uint32_t viewBits = 0;  // every feature view's view bit, of both families
		// The view bits a set phase bit takes part in (Core's main and caster phases filled by Core, the feature views' by their entries),
		// and the same by phase mask: ViewBitsOf's one read.
		std::array<std::uint32_t, 8> viewBitsOfPhaseBit{};
		std::array<std::uint32_t, 256> viewBitsOfPhases{};
		// The phases FrameView's lacking counts are by, in their order (Coordinator::CommitSet, FrameView::SetLacking): the sky map's, the
		// precipitation map's (each 0 while no feature takes it), the LodTechniques views' (ViewSet::lodPhases); and all of them.
		std::array<std::uint8_t, 3> lackingPhases{};
		std::uint8_t lackingMask = 0;
	};

	namespace Detail
	{
		// Written only by ResolveViewBits (the main thread, before DCLF's threads exist: their start orders these writes before every
		// read), read-only after: a plain read for the per-object paths.
		inline ViewBitTable resolvedViewBits;
	}

	/** @brief Any thread, after InstallFeatures (empty before): the resolved phases and view bits. */
	inline const ViewBitTable& ResolvedViewBits() { return Detail::resolvedViewBits; }

	/**
	 * @brief Main thread, at the end of InstallFeatures (Registry), after ResolveViews and ResolveShadowViews: the table from both
	 * families' entries. Fails the install for a feature view's phase or view bit that is none, more than one, or Core's or another
	 * view's (of either family). Once.
	 */
	void ResolveViewBits();
}
