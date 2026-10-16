#pragma once

// GpuOcclusion: the depth pyramid the two-phase occlusion test reads (Api/DepthPyramid.h). The feature is the pyramid's producer: the
// HZB texture ("cs.dclf.hzb"), its downsample's program (HzbCS.hlsl, in the feature's shader folder, Features/GpuOcclusion/HzbCS.hlsl;
// FidelityFX SPD), the downsample's finished-group counter ("cs.dclf.hzb-counter", the pyramid's scratch) and its pass ("cs.dclf.hzb",
// in the depth epoch between phase 1's depth draw and phase 2's retest build). It registers no switch, toggle, report line or parity.
//
// Core keeps the test: CS_DCLF_CULL's mode, BuildDrawsCS's HZB branch and HzbTest.hlsli (whose mip-0 convention the pyramid keeps), the
// latch's HZB words, the phase 2 block and its stages, the HZB bindings of BuildDraws and the tree LOD cull, and the stats and parity
// observers. Without the feature, culling is frustum-only, as when the program fails. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::GpuOcclusion
{
	// GpuOcclusionFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// HzbPass.cpp: the depth pyramid's contribution (its texture, program and counter, and its pass).
	void RegisterPyramid(Api::InstallContext& a_context);
}
