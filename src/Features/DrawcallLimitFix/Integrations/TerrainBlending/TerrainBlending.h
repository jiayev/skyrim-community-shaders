#pragma once

// Terrain Blending (Features/TerrainBlending.h; CS_DCLF_TB_TERRAIN): while it is loaded and enabled it holds the terrain, replaying every
// kMultiTextureLandscape pass after the opaque pass, blended over what lies beneath, then the passes of meshes opting out with the otherwise
// unused kNoTransparencyMultiSample, testing EQUAL. The integration registers what DCLF needs to draw both as it does:
// - the "terrain blending defers" classify input (Api::ClassifyInputContribution), which classification snapshots per pass;
// - its switch and live toggle ("terrain under TB", needing Features/Landscape's "terrain");
// - the gates on Features/Landscape's techniques (Api::TechniqueGate: MTLand and MTLandLODBlend need the toggle while the input holds);
// - its two held groups (Api::HeldGroupContribution: the terrain blocks in Appended0, the no-blend meshes in Appended1, orders 2 and 3, before
//   Features/Decals' blended decals at 4);
// - its mask, t55 (Api::FrameTexture: the terrain depth while it holds the terrain, else the captured view);
// - its near blocks' depth twin (Api::NearTwinContribution: its terrain depth, imported, and the depth pass's near test).
//
// Core keeps the main pass's structure: the appended slots, their count words and object flag bits (GpuLayouts.h, BuildDrawsCS), the twin
// buckets and the terrain depth pass, the held groups' states and depth tests per slot. Included by the integration's sources and
// Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::TerrainBlending
{
	// Its held groups' slots (Records.h's terrain group bits g: slot Appended(g - 1), BuildDrawsCS's; Features/Manifest.cpp): the terrain
	// blocks (group 1), the no-blend meshes after them (group 2).
	inline constexpr Api::MainRangeSlot kTerrainRange = Api::MainRangeSlot::Appended0;
	inline constexpr Api::MainRangeSlot kNoBlendRange = Api::MainRangeSlot::Appended1;
	// t55: Terrain Blending's mask (TerrainBlending.hlsli's TerrainBlendingMaskTexture), a frame texture (Features/Manifest.cpp).
	inline constexpr std::uint32_t kMaskRegister = 55;

	// TerrainBlendingIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
