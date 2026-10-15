#pragma once

// Decals (CS_DCLF_DECALS, CS_DCLF_LAYERS; drawcall-limit-fix.md; engine notes, "decals"): the engine's decals (accumulation hints 2 and 3)
// and multi-index shapes' layers (hint 12), which DCLF draws in its decal groups after the opaque draws, each group in the engine's draw
// order. The feature registers its switches and live toggles, the decal and layer object classes' contributions (Api::ObjectClass: the
// Lighting and Effect ports' eligibility reads the toggles through them, as data), the member decals' order (a coordinator component:
// DecalOrder.cpp, writing SceneTables::rangeOrdinal and rangeCount), the decal groups' ranges of the colour segment's second pass
// (Api/MainRanges.h: their slots, order, nvperf names and the opaque group's depth prepass) and its parity
// (Diagnostics/DecalOrderParity.cpp).
//
// Core keeps what the main pass is built of: the ports' decal groups and states (LightingDescriptors, the accumulate patch's decalKey,
// Records.h's kObjectDecal and group bits, the raster keys), the tables' ordinal columns, the main build's decal inputs and templates,
// the sequence buffer's range slots and count words (GpuLayouts.h, BuildDrawsCS), and the colour segment's passes that draw the
// registered ranges (MainOpaque.cpp). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Decals
{
	// The decal groups' slots (Records.h's group bits g: slot Fixed(g - 1), BuildDrawsCS's; Features/Manifest.cpp): accumulation hint 2,
	// opaque (group 1); hint 3, blended (group 2); the multi-index layers, hint 12 (group 3).
	inline constexpr Api::MainRangeSlot kOpaqueRange = Api::MainRangeSlot::Fixed0;
	inline constexpr Api::MainRangeSlot kBlendedRange = Api::MainRangeSlot::Fixed1;
	inline constexpr Api::MainRangeSlot kLayerRange = Api::MainRangeSlot::Fixed2;

	// DecalsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// DecalOrder.cpp: the member decals' order, a coordinator component (scene lane).
	void RegisterOrder(Api::InstallContext& a_context);

	// Diagnostics/DecalOrderParity.cpp: CS_DCLF_PERSISTENT_PARITY's check of the kept order against one made whole, every 60 passes.
	void SubscribeOrderParity();
}
