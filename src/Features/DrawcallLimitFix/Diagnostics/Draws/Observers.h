#pragma once

#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"

// The draws area's observers (Diagnostics/Draws/*.cpp), subscribed by InstallDraws (DrawsInstall.cpp) to the hook points of Events.h.

namespace DCLF::Diagnostics
{
	struct MainEpochDone;
	struct CullLatchWritten;
	struct MainPassCaptured;
	struct ProbePoint;
	struct FadeLogWindow;
	struct ExtrasSending;
	struct ExtrasSent;
	struct ObjectRecordsUpdated;
	struct GeometryDrawsUpdated;
	struct DrawBoundUpdated;
	struct ShadowBuilt;
	struct ExclusionRebuilt;
	struct DrawsBuiltAhead;
	struct MainBuildBegun;
	struct MainRecordAssembled;
	struct ResidentRegionUpdated;
	struct MainPayloadCommitted;
	struct IndexPoolUpdated;
	struct ShadowTargetPredicted;
	struct OwnShadowViewport;
	struct OwnShadowCamera;
	struct SunEntryPlanesTaken;
	struct OcclusionCaptured;
	struct ShadowViewLatched;
	struct ColourFrameBlock;
	struct MainCullViewProj;
	struct ReflectionFaceDrawn;
	struct FirstPersonSkeletonWalked;
	struct FoliageRecordMade;
	struct MainGraphPasses;

	// The culling's counters, every run (CullCounters.cpp).
	void NoteCullLatch(const CullLatchWritten& a_written);
	void ReadCullCounters(const MainEpochDone& a_epoch);
	// CS_DCLF_BUILD_PARITY (BuildParity.cpp).
	void CheckBuildParity(const MainEpochDone& a_epoch);
	// CS_DCLF_PERSISTENT_PARITY: tree wind (TreeWind.cpp).
	void ReadTreeWind(const MainEpochDone& a_epoch);
	// CS_DCLF_FADE_PARITY (FadeLog.cpp).
	void NextFadeLog(const FadeLogWindow& a_window);
	void ReadFadeLog(const MainEpochDone& a_epoch);
	// CS_DCLF_SET_PARITY (SetParity.cpp).
	void CheckSetParity(const MainEpochDone& a_epoch);
	// CS_DCLF_GBUFFER_PROBE (GBufferProbe.cpp).
	void NoteProbeTargets(const MainPassCaptured& a_captured);
	void ProbeGBuffer(const ProbePoint& a_point);
	void AppendProbePasses(const MainGraphPasses& a_passes);
	// CS_DCLF_PERSISTENT_PARITY: the frame values' rows and palettes against the engine's (FrameValuesParity.cpp; render thread, waits for
	// the frame's producer). a_fresh: the plan the walk made this frame (its slots' rows are the next frame's).
	void CheckFrameValues(const SceneTables& a_tables, const PlacementPlan* a_fresh);
	// CS_DCLF_CAPTURE_POINT_PARITY: at the frame's first deferred lighting draw (CapturePoint.cpp).
	void CheckCapturePoint();
	// CS_DCLF_PERSISTENT_PARITY: the kept stores against rebuilds (KeptChecks.cpp), the view masks (ViewMasks.cpp).
	void MirrorExtras(const ExtrasSending& a_sending);
	void CheckExtras(const ExtrasSent& a_sent);
	void CheckObjectRecords(const ObjectRecordsUpdated& a_updated);
	void CheckGeometryDraws(const GeometryDrawsUpdated& a_updated);
	void CheckDrawBound(const DrawBoundUpdated& a_updated);
	void CheckKeptShadow(const ShadowBuilt& a_built);
	void CheckExclusionReuse(const ExclusionRebuilt& a_rebuilt);
	void CheckViewMasks(const DrawsBuiltAhead& a_built);
	// CS_DCLF_BINDLESS_PARITY and CS_DCLF_RESIDENT_DRAW_PARITY: the main builds' checks (BuildChecks.cpp).
	void BeginBindlessChecks(const MainBuildBegun& a_begun);
	void CheckBindless(const MainRecordAssembled& a_record);
	void CheckResidentRegion(const ResidentRegionUpdated& a_updated);
	void SumBuildChecks(const MainPayloadCommitted& a_committed);
	// The shadow epochs' checks (ShadowChecks.cpp): CS_DCLF_BUILD_PARITY's index pool; the views against the engine's (CS_DCLF_SET_PARITY
	// or CS_DCLF_PERSISTENT_PARITY); the cascade culling (CS_DCLF_SET_PARITY); the sun entry (CS_DCLF_PERSISTENT_PARITY).
	void CheckIndexPool(const IndexPoolUpdated& a_updated);
	void CheckShadowTarget(const ShadowTargetPredicted& a_predicted);
	void CheckOwnViewport(const OwnShadowViewport& a_own);
	void CheckOwnCamera(const OwnShadowCamera& a_own);
	void CheckSunEntryPlanes(const SunEntryPlanesTaken& a_taken);
	void CheckOcclusionView(const OcclusionCaptured& a_captured);
	void CheckCascadeCulling(const ShadowViewLatched& a_latched);
	void CheckSunEntry(const ShadowViewLatched& a_latched);
	// The colour epoch's probes (ColourProbes.cpp): CS_DCLF_STATS's jitter, CS_DCLF_TARGET_PROBE.
	void MeasureJitter(const ColourFrameBlock& a_block);
	void ProbeTarget(const MainCullViewProj& a_cull);
	// The reflection's face cameras (ReflectionChecks.cpp; CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY).
	void CheckReflectionFace(const ReflectionFaceDrawn& a_drawn);
	// CS_DCLF_SET_PARITY: the first-person skeleton's slots and claimed phases, logged when they change (ReflectionChecks.cpp).
	void NoteFirstPersonSkeleton(const FirstPersonSkeletonWalked& a_walked);
	// CS_DCLF_FOLIAGE_PARITY: the compare pass's program and readbacks, its passes after the colour draws (Foliage.cpp).
	void MakeFoliageCompare(const FoliageRecordMade& a_made);
	void AppendFoliagePasses(const MainGraphPasses& a_passes);
	// CS_DCLF_PERSISTENT_PARITY: the catalog's constant tables against ShaderCache's (ConstantTables.cpp).
	void CheckConstantTables(const RE::BSShader& a_lighting, const Lookups& a_lookups);
}
