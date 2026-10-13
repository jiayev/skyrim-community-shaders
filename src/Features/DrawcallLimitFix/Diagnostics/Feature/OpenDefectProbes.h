#pragma once

namespace DCLF
{
	// The probes of the open defects (docs/development/dclf-open-defects.md). DrawcallLimitFix::ProbeOpaqueTarget is
	// defined with them.
	/**
	 * @brief CS_DCLF_TREE_LOD_AUDIT=1, main thread at the scene frame's start, with or without DCLF: the attached tree LOD
	 * blocks' instances (BGSDistantTreeBlock) against their references. An instance shown while its reference's full tree is
	 * loaded and visible is drawn over it (counted apart while the tree's fade is above 0: the engine crossfades the two, the
	 * instance's alpha 1 - the fade); one hidden without a visible full tree leaves a hole (dclf-lod.md, "Tree instance hiding").
	 */
	void AuditTreeLod();
	/**
	 * @brief CS_DCLF_SET_PARITY, render thread at the scene frame's start after the frame's claims are installed: the player's skeletons
	 * against them. The third-person skeleton while it is hidden (first person: FUN_140736150's store) and the first-person one (hidden
	 * for every world view by Main::Draw) must have no geometry claimed for the main view; a claim that outlives the publication's
	 * latency (8 frames) is drawn by DCLF where the engine draws nothing. Reported every 300 frames (`<- PLAYER 3D`).
	 */
	void ObservePlayerClaims();
}
