#pragma once

// The view contribution contract (Api::InstallContext::RegisterView): a view of the engine's that Core's view machinery can draw in its
// place (its scene set, its view bit at the GPU cull, its epochs and their revisions) but leaves to the engine until a feature takes it,
// under the feature's toggle.

#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstdint>

namespace DCLF::Api
{
	/** @brief An engine view Core's view machinery draws once a feature takes it. */
	enum class EngineView : std::uint8_t
	{
		// The player's first-person skeleton (arms, weapons, shield, magic hands): the members of kSetFirstPerson, the view bit
		// kViewFirstPerson at the GPU cull, a depth epoch in RenderDepth and a colour epoch at Main::RenderFirstPersonView, and their
		// forward part.
		FirstPerson,
		Count,
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
}
