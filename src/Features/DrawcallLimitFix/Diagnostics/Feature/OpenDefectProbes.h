#pragma once

namespace DCLF
{
	// The probes of the open defects (docs/development/dclf-open-defects.md). DrawcallLimitFix::ProbeOpaqueTarget is
	// defined with them.
	void ProbeShadowMask(bool a_running);
	void ProbeShadowMaps(bool a_running);
	void ProbeTerrainPassState(bool a_running, std::uint32_t a_index, const RE::BSRenderPass* a_pass);
	/**
	 * @brief CS_DCLF_TREE_LOD_AUDIT=1, main thread at the scene frame's start, with or without DCLF: the attached tree LOD
	 * blocks' instances (BGSDistantTreeBlock) against their references. An instance shown while its reference's full tree is
	 * loaded and visible is drawn over it (counted apart while the tree's fade is above 0: the engine crossfades the two, the
	 * instance's alpha 1 - the fade); one hidden without a visible full tree leaves a hole (dclf-lod.md, "Tree instance hiding").
	 */
	void AuditTreeLod();
	/**
	 * @brief TEMP (CS_DCLF_REFLECTION_CENSUS=1): the water reflection cube map's draws (BSCubeMapCamera's face render, vfunc 0x35,
	 * AE 0x1414ed920; skyrim-engine-notes.md, "Water reflections: the cube map"). The engine's pass draw (FUN_1414f2ad0) is
	 * thunked at its six calls; inside a face it records each draw's shader, technique, LOD class, scene root and render state,
	 * and each face's targets and viewport. Reported every 300 frames.
	 */
	void InstallReflectionCensus();
	/**
	 * @brief CS_DCLF_SET_PARITY, render thread at the scene frame's start after the frame's claims are installed: the player's skeletons
	 * against them. The third-person skeleton while it is hidden (first person: FUN_140736150's store) and the first-person one (hidden
	 * for every world view by Main::Draw) must have no geometry claimed for the main view; a claim that outlives the publication's
	 * latency (8 frames) is drawn by DCLF where the engine draws nothing. Reported every 300 frames (`<- PLAYER 3D`).
	 */
	void ObservePlayerClaims();
}
