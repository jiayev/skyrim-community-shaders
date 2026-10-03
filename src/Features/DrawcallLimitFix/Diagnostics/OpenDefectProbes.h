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
}
