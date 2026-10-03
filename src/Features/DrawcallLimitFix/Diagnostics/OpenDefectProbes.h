#pragma once

namespace DCLF
{
	// The probes of the open defects (docs/development/dclf-open-defects.md). DrawcallLimitFix::ProbeOpaqueTarget is
	// defined with them.
	void ProbeShadowMask(bool a_running);
	void ProbeShadowMaps(bool a_running);
	void ProbeTerrainPassState(bool a_running, std::uint32_t a_index, const RE::BSRenderPass* a_pass);
}
