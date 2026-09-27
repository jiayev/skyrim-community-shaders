#pragma once

#include <cstdint>

struct ID3D11BlendState;
struct ID3D11RasterizerState;

namespace DCLF
{
	/**
	 * @brief The engine's fixed-function state tables, indexed by the RendererShadowState fields
	 * (engine notes: renderer shadow state), and the globals that pick a decal's depth bias.
	 *
	 * The rasterizer array is the one ShadowmapCascadeRasterizerFix clones; the blend array is the one
	 * Deferred.cpp swaps for its deferred variants between StartDeferred and ResetBlendStates. A state
	 * read from the blend table therefore depends on WHEN it is read: inside the deferred pass it is what
	 * a native draw in the G-buffer uses, outside it is the forward one.
	 */
	using RasterStateArray = ID3D11RasterizerState* [2][3][12][2];  // [fill][cull][depth bias][scissor]
	using BlendStateArray = ID3D11BlendState* [7][2][13][2];        // [blend mode][alpha to coverage][write mode][extra]

	RasterStateArray& EngineRasterStates();
	BlendStateArray& EngineBlendStates();

	/**
	 * @brief The depth-bias mode the engine's decal groups draw with (decompiled FUN_1414b3bb0 and
	 * FUN_1414b3d50, measured with CS_DCLF_DECAL_PROBE).
	 *
	 * Group 1 (accumulation hint 2, opaque decals): 6 + DrawWorld::disableSunShadows while the
	 * ToggleDepthBias global is set, 0 otherwise. Group 2 (hint 3, blended decals): 10 + disableSunShadows,
	 * unconditionally. Modes 7 and 11 - what interiors use, where sun shadows are off - hold no bias at all.
	 */
	std::uint32_t DecalDepthBiasMode(std::uint32_t a_decalGroup);
}
