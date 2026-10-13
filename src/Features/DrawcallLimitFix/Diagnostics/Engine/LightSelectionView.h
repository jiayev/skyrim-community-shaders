#pragma once

#include "Features/DrawcallLimitFix/Engine/Views/LightSelection.h"

namespace DCLF::Diagnostics
{
	/** @brief The light selection observer's read-only view of LightSelection's private state (LightSelection befriends it). Render thread. */
	struct LightSelectionView
	{
		static const LightSelection& Self() { return LightSelection::Get(); }
		/** @brief This frame's selection (valid or not). */
		static const LightSelection::Frame& Frame() { return Self().frame; }
		/** @brief Each light's portal-graph entry as DCLF keeps it, and this frame's room verdicts. */
		static const auto& LightRooms() { return Self().lightRooms; }
		static const auto& RoomVerdicts() { return Self().roomVerdicts; }
		/** @brief The focus flag per light as DCLF writes it, and last frame's host. */
		static const auto& FocusFlags() { return Self().focusFlags; }
		static const RE::BSShadowLight* LastFocusHost() { return Self().lastFocusHost; }
	};
}
