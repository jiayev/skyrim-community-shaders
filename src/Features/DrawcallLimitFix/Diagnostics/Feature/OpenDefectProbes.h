#pragma once

namespace DCLF
{
	// The probes of the open defects (docs/development/dclf-validation.md, "The target probe", "Set parity"). DrawcallLimitFix::ProbeOpaqueTarget is
	// defined with them.
	// (The tree LOD audit, CS_DCLF_TREE_LOD_AUDIT, is Features/TreeLod's.)
	/**
	 * @brief CS_DCLF_SET_PARITY, render thread at the scene frame's start after the frame's claims are installed: the player's skeletons
	 * against them. The third-person skeleton while it is hidden (first person: FUN_140736150's store) and the first-person one (hidden
	 * for every world view by Main::Draw) must have no geometry claimed for the main view; a claim that outlives the publication's
	 * latency (8 frames) is drawn by DCLF where the engine draws nothing. Reported every 300 frames (`<- PLAYER 3D`).
	 */
	void ObservePlayerClaims();
}
