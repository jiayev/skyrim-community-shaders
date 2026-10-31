#pragma once

// Skins (CS_DCLF_SKINNED, CS_DCLF_SKIN_PARTITIONS; engine notes: skinning): which skinned shapes are DCLF's. The feature registers their
// switches and live toggles (the skinned toggle, which Features/FirstPerson's needs, and the skins of several partitions', which needs it)
// and their object classes' contributions (Api::ObjectClass::Skinned and SkinPartitions: the eligibility, the Lighting port's kSkinned,
// the walk's palette rows and the Effect port's skinned effects read them as data), and its report line (ReportPoint::
// SceneCoordinatorLastPass: the last pass's skinned candidates and their palette rows).
//
// Core keeps the path a skinned shape takes once its class is on, shared with the classes that read skins (Features/Actors, the LOD
// chains, Features/FirstPerson, Features/Effects, the blood decals): the skin engine facts (Core/Engine/SkinPartitions.h: the partitions
// the engine draws; Core/Engine/SkinPalettes.h: the palette rows and their per-item sample in the engine lease), the palette part
// FrameValues shares with the particles (kObjectPaletteBlock), the drawn-range column the LOD chains and the decals share, the vertex
// stages' bones, kTraitSkin and the Ineligible reasons.
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Skins
{
	// SkinsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
	// Diagnostics/SkinsReport.cpp: the skinned line (CS_DCLF_STATS, among the coordinator's last-pass lines).
	void RegisterReport(Api::InstallContext& a_context);
}
