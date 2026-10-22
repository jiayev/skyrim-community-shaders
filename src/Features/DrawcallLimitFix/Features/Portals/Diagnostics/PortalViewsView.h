#pragma once

#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

namespace DCLF::Diagnostics
{
	/** @brief The portal observer's read-only view of PortalViews' private state (PortalViews befriends it). Render thread, at Build's end. */
	struct PortalViewsView
	{
		static const PortalViews& Self() { return PortalViews::Get(); }
		/** @brief This frame's walk: the graph, unbound space's program, the rooms' keys and programs, the rooms it popped. */
		static const PortalViews::Frame& Frame() { return Self().frame; }
		/** @brief The camera the walk measured from: its eye and view planes. */
		static const auto& Camera() { return Self().camera; }
		/** @brief DCLF's per-frame states of the shapes (0 untested, 1 accepted, 2 rejected). */
		static const auto& States() { return Self().states; }
		/** @brief DCLF's copies of the boxes' silhouette sets (BoxScratch: set[2], hullCount). */
		static const auto& Boxes() { return Self().boxes; }
		static auto Corners(const void* a_quad) { return PortalViews::Corners(a_quad); }
		/** @brief Unbound space's top program again with the engine's occluder states (PortalViews::UnboundWithEngineStates). */
		static PortalViews::Program UnboundWithEngineStates(const void* a_graph) { return PortalViews::Get().UnboundWithEngineStates(a_graph); }
		template <class Quad, class Flags>
		static bool QuadVsSet(const PortalViews::PlaneSet& a_set, const Quad& a_quad, Flags& a_flags)
		{
			return PortalViews::QuadVsSet(a_set, a_quad, a_flags);
		}
	};
}
