#pragma once

namespace DCLF
{
	// The probes of the open defects (docs/development/dclf-open-defects.md). DrawcallLimitFix::ProbeOpaqueTarget is
	// defined with them.
	void ProbeShadowMask(bool a_running);
	void ProbeShadowMaps(bool a_running);
	void ProbeTerrainPassState(bool a_running, std::uint32_t a_index, const RE::BSRenderPass* a_pass);
	/**
	 * @brief TEMP (CS_DCLF_LOD_CENSUS=1): what the engine still draws itself, by shader, LOD kind, technique, geometry class and
	 * scene root. a_mode is ~0u for a main-pass draw (a_depth: the depth pass), else the shadow mode of an unwithheld
	 * registration. Reported every 300 main-pass frames.
	 */
	void CensusNativePass(const RE::BSRenderPass* a_pass, std::uint32_t a_technique, std::uint32_t a_mode, bool a_depth);
	/**
	 * @brief CS_DCLF_TREE_LOD_AUDIT=1, main thread at the scene frame's start, with or without DCLF: the attached tree LOD
	 * blocks' instances (BGSDistantTreeBlock) against their references. An instance shown while its reference's full tree is
	 * loaded and visible is drawn over it (counted apart while the tree's fade is above 0: the engine crossfades the two, the
	 * instance's alpha 1 - the fade); one hidden without a visible full tree leaves a hole (dclf-lod.md, "Tree instance hiding").
	 */
	void AuditTreeLod();
}
