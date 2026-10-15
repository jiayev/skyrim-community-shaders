#pragma once

// The view contributions' Core side (Api/Views.h): each engine view's toggle and forward decals, by view (the view machinery's epochs,
// revisions, frame requests and classification read them as data).

#include "Features/DrawcallLimitFix/Api/Views.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <cstddef>

namespace DCLF::Extensions
{
	/** @brief A view's contribution (none: the view stays the engine's). */
	struct ViewEntry
	{
		Api::ToggleId toggle;
		bool forwardDecals = false;
	};
	using ViewEntries = std::array<ViewEntry, static_cast<std::size_t>(Api::EngineView::Count)>;
	/** @brief Any thread, after InstallFeatures (all none before): the view contributions, by view. */
	const ViewEntries& RegisteredViews();

	/** @brief Whether DCLF draws a_view under a_toggles (its contribution's toggle on; false for none). */
	inline bool ViewOn(const ToggleSet& a_toggles, Api::EngineView a_view)
	{
		return a_toggles.On(RegisteredViews()[static_cast<std::size_t>(a_view)].toggle);
	}

	/** @brief Whether a decal on a_view's entries is a forward object of its forward part (its contribution's; false for none). */
	inline bool ViewForwardDecals(Api::EngineView a_view)
	{
		return RegisteredViews()[static_cast<std::size_t>(a_view)].forwardDecals;
	}
}
