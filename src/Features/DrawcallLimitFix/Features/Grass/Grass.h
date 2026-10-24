#pragma once

// Grass (CS_DCLF_GRASS): the engine's grass (the BSGrassShaderProperty instance shapes) drawn in the engine's place, from the mirrored
// instance records, as the Grass draw family (Api/DrawFamilies.h). The feature registers its switch (CS_DCLF_GRASS), its live toggle
// and the grass class's contribution (Api::ObjectClass::Grass, which gates the family), registers its sink on GrassShared's capture at
// its early hooks (GrassHooks.cpp: the shapes' records, removals and rejections, pushed onto events; the capture is shared with Grass
// Optimizations, src/GrassShared) and keeps the render thread's mirror (GrassMirror.h, drained by the family's serve). It draws it: the
// tables as scene resources, with what they post with the revision's inputs and latch in the depth commit, and the depth commit's uploads
// as a commit hook (GrassTables.cpp); GrassCullCS and GrassArgsCS (their HLSL in the feature's shader folder, Features/Grass/) in the
// depth segment's two phases, the colour draws' arguments and the counts' readback as its passes (GrassPasses.cpp); the family - its
// frame's decision and program source, its depth and colour draws: arrays of instanced draws with a GPU count, one per bucket and LOD
// tier (GrassDraws.cpp). It has no faces: a trees view draws no grass. Its report lines (Diagnostics/GrassReport.cpp) and the mirror's
// parity (Diagnostics/GrassParity.cpp) are its own.
//
// While the family is DCLF's the engine's grass passes are withheld, so nothing hooked on BSGrassShader::SetupGeometry runs: the
// decision tells GrassShared (SetDclfOwned) and runs its stood-down callbacks (Grass Optimizations' pending captures, Grass Collision's
// update and publish), whose products the depth commit reads (GrassShared::Settings, LatestCollision).
//
// Core keeps the family's slot (Core/Render/DrawFamilies/FamilySlots.h): its pipelines on the lane (DrawPipelines' grass family: a
// depth program and two colour programs) from the program source, the ownership it publishes from the decision (PassCapture's
// withholding of the engine's grass passes) and the shapes key's part. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstdint>

namespace DCLF::Features::Grass
{
	// Engine::Offsets (Address Library ids) of the engine state this feature alone reads (stage 0's reverse engineering).
	namespace Address
	{
		inline constexpr Engine::Offset kGrassShader{ { 414752 }, { 527801 } };        // BSGrassShader*: the singleton (DistantTree's is 0x33dcd10)
		inline constexpr Engine::Offset kGrassManager{ { 400452 }, { 514292 } };       // BGSGrassManager*
		inline constexpr std::uintptr_t kGrassManagerNode = 0x68;        // its grass node (NiAVObject*), +0xF4 bit 0: ToggleGrass off
		inline constexpr Engine::Offset kWindTimer{ { 390957 }, { 513217 } };          // float: the shader manager's timer SetupGeometry's WindTimer reads
		inline constexpr Engine::Offset kWindSpeed{ { 414753 }, { 527802 } };          // float: the grass wind's strength (clamped to 60)
		inline constexpr Engine::Offset kWindGrassMultiplier{ { 391112 }, { 513337 } };  // float: fWindGrassMultiplier:Display
		inline constexpr Engine::Offset kAlphaParams{ { 414681 }, { 527758 } };        // float[2]: AlphaParam1, AlphaParam2 (the distance fade)
		inline constexpr Engine::Offset kShadowClampValue{ { 391115 }, { 513339 } };   // float: fShadowClampValue:Display
		inline constexpr Engine::Offset kFogNearColorW{ { 390973 }, { 513233 } };      // float: FogNearColor.w
	}

	// GrassFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// GrassHooks.cpp: the sink on GrassShared's capture (the feature's early hooks), registered and installed once at PostPostLoad;
	// sets installed (GrassMirror.h).
	void InstallHooks();

	// GrassTables.cpp: the tables (scene resources) and the depth commit's uploads (commit hook).
	void RegisterTables(Api::InstallContext& a_context);
	// GrassPasses.cpp: the cull and the draws' arguments in the depth segment's two phases, the colour draws' arguments and the counts'
	// readback (grass.cull, grass.args, grass.cull-phase2, grass.args-phase2, grass.args-colour, grass.counts).
	void RegisterPasses(Api::InstallContext& a_context);
	// GrassDraws.cpp: the Grass draw family (after the tables: its resources).
	void RegisterFamily(Api::InstallContext& a_context);
	// Diagnostics/GrassReport.cpp: the draws' and the mirror's report lines.
	void RegisterReport(Api::InstallContext& a_context);
	// Diagnostics/GrassParity.cpp: CS_DCLF_PERSISTENT_PARITY's mirror check (Diagnostics::Observe().scene).
	void SubscribeParity();
}
