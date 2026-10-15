#pragma once

// Decals (CS_DCLF_DECALS, CS_DCLF_LAYERS; drawcall-limit-fix.md; engine notes, "decals"): the engine's decals (accumulation hints 2 and 3)
// and multi-index shapes' layers (hint 12), which DCLF draws in its decal groups after the opaque draws, each group in the engine's draw
// order. The feature registers its switches and live toggles, the decal and layer object classes' contributions (Api::ObjectClass: the
// Lighting and Effect ports' eligibility reads the toggles through them, as data), the member decals' order (a coordinator component:
// DecalOrder.cpp, writing SceneTables::decalOrdinal and decalCount) and its parity (Diagnostics/DecalOrderParity.cpp).
//
// Core keeps what the main pass is built of: the ports' decal groups and states (LightingDescriptors, the accumulate patch's decalKey,
// Records.h's kObjectDecal and group bits, the raster keys), the tables' ordinal columns, the main build's decal inputs and templates,
// the sequence buffer's decal ranges and count words (GpuLayouts.h, BuildDrawsCS), and the colour segment's decal depth pass and second
// pass (MainOpaque.cpp). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Decals
{
	// DecalsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// DecalOrder.cpp: the member decals' order, a coordinator component (scene lane).
	void RegisterOrder(Api::InstallContext& a_context);

	// Diagnostics/DecalOrderParity.cpp: CS_DCLF_PERSISTENT_PARITY's check of the kept order against one made whole, every 60 passes.
	void SubscribeOrderParity();
}
