#pragma once

// The portal programs' encoding (PortalViews::Encode, PortalViews::Visible), which PortalPrograms.hlsli's PortalTest reads: the feature's
// buffer sends the words (PortalPrograms.cpp), which Core binds by its cull binding (Api/CullInputs.h) and never reads.

#include <cstdint>

namespace DCLF
{
	/**
	 * @brief The main camera's portal and occluder programs (Features/Portals/PortalViews), for FadeStateCS's and BuildDrawsCS's main
	 * view tests: a ByteAddressBuffer uploaded with the depth commit (PortalPrograms.cpp).
	 *
	 * Layout (bytes): the camera's view planes (NiFrustumPlanes: six {n, d}, the active mask at +96; kPortalSetBytes), then a header
	 * {program count, 0, 0, 0}, then a directory entry per program (kPortalDirectoryBytes: flags, operator count, set count, first
	 * operator, first operator record, first set, cull mode, maximum executable path length), then compact 16-byte operator records
	 * {type, plane set index, next if true, next if false}. Only reachable tests and terminals are emitted, in forward topological order, and the
	 * plane sets. Program 0 is unbound space; program 1 + i the portal graph's room i (graph +0x40). A root's program is
	 * kPortalNoProgram when there is no walk this frame (the frustum alone).
	 */
	inline constexpr std::uint32_t kPortalSetBytes = 112;
	inline constexpr std::uint32_t kPortalHeaderBytes = kPortalSetBytes + 16;
	inline constexpr std::uint32_t kPortalDirectoryBytes = 32;
	inline constexpr std::uint32_t kPortalOpBytes = 16;
	inline constexpr std::uint32_t kPortalNoProgram = 0xFFFFFFFFu;
	inline constexpr std::uint32_t kPortalReached = 1u << 0;   // the walk reached it (else a room's children are not culled into)
	inline constexpr std::uint32_t kPortalSkipView = 1u << 1;  // skipViewFrustum: the view test is not made
}
