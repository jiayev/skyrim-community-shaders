#pragma once

// The draws area's observers (Diagnostics/Draws/*.cpp), subscribed by InstallDraws (DrawsInstall.cpp) to the hook points of Events.h.

#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"

namespace DCLF::Draws
{
	struct CounterReadback;
}

namespace DCLF::Diagnostics
{
	struct MainEpochDone;
	struct CullLatchWritten;
	struct FrameTextureUnresolved;
	struct FrameBufferUnfilled;
	struct FadeInputsVaried;
	struct ShortBuffersDrawn;
	struct MainCullingSetup;
	struct ExtrasSending;
	struct ExtrasSent;
	template <class Store>
	struct StoreUpdated;
	struct ShadowBuilt;
	struct ExclusionRebuilt;
	struct DrawsBuiltAhead;
	struct SnapshotSealed;
	struct CommitTargetStaged;
	struct RevisionShapesMade;
	struct IndexPoolUpdated;
	struct ShadowTargetPredicted;
	struct OwnShadowViewport;
	struct OwnShadowCamera;
	struct ShadowViewLatched;
	struct MainCullViewProj;
	struct FoliageRecordMade;
	struct MainGraphPasses;
	struct SnapshotAdopted;
	struct EpochCoverageDecided;
	struct MainEpochOutcome;
	struct AdoptedBound;

	// The culling's counters, every run (CullCounters.cpp).
	void NoteCullLatch(const CullLatchWritten& a_written);
	void ReadCullCounters(const MainEpochDone& a_epoch);
	// A frame texture the commit could not resolve, described once per register, every run (TextureCommit.cpp).
	void DescribeUnresolvedFrameTexture(const FrameTextureUnresolved& a_unresolved);
	// The main commits' warnings and setup log, every run (CommitLog.cpp).
	void LogUnfilledFrameBuffer(const FrameBufferUnfilled& a_unfilled);
	void LogFadeInputsVaried(const FadeInputsVaried& a_varied);
	void LogShortBuffers(const ShortBuffersDrawn& a_drawn);
	void LogCullingSetup(const MainCullingSetup& a_setup);
	// CS_DCLF_BUILD_PARITY (BuildParity.cpp).
	void CheckBuildParity(const MainEpochDone& a_epoch);
	// CS_DCLF_SET_PARITY (SetParity.cpp).
	void CheckSetParity(const MainEpochDone& a_epoch);
	// CS_DCLF_SET_PARITY: the colour epoch's state class sub-ranges against its shape's class plan (SetClassParity.cpp).
	void CheckSetClassParity(const MainEpochDone& a_epoch);
	// CS_DCLF_PERSISTENT_PARITY: the frame values' rows and palettes against the engine's (FrameValuesParity.cpp; render thread, waits for
	// the frame's producer). a_fresh: the plan the walk made this frame (its slots' rows are the next frame's).
	void CheckFrameValues(const SceneTables& a_tables, const PlacementPlan* a_fresh);
	// CS_DCLF_CAPTURE_POINT_PARITY: at the frame's first deferred lighting draw (CapturePoint.cpp).
	void CheckCapturePoint();
	// CS_DCLF_PERSISTENT_PARITY: the kept stores against rebuilds (KeptChecks.cpp), the view masks (ViewMasks.cpp).
	void MirrorExtras(const ExtrasSending& a_sending);
	void CheckExtras(const ExtrasSent& a_sent);
	void CheckObjectRecords(const StoreUpdated<Draws::ObjectRecordStore>& a_updated);
	void CheckGeometryDraws(const StoreUpdated<Draws::GeometryStore>& a_updated);
	void CheckDrawBound(const StoreUpdated<Draws::DrawBoundStore>& a_updated);
	void CheckKeptShadow(const ShadowBuilt& a_built);
	void CheckExclusionReuse(const ExclusionRebuilt& a_rebuilt);
	void CheckViewMasks(const DrawsBuiltAhead& a_built);
	// CS_DCLF_PERSISTENT_PARITY: the sealed set (NamedParity.cpp): every address a snapshot's draws embed against its sealed set; the
	// commits' targets named and current in the adopted set; the recordings' resolves of buffers their set does not name; and every
	// shape's ranges and counts within the named versions of the buffers it indexes.
	void CheckSnapshotAddresses(const SnapshotSealed& a_sealed);
	void CheckCommitTarget(const CommitTargetStaged& a_staged);
	void CheckShapeCapacities(const RevisionShapesMade& a_made);
	void CheckNamedResolves();
	// The shadow epochs' checks (ShadowChecks.cpp): CS_DCLF_BUILD_PARITY's index pool; the views against the engine's (CS_DCLF_SET_PARITY
	// or CS_DCLF_PERSISTENT_PARITY); the sun entry (CS_DCLF_PERSISTENT_PARITY).
	void CheckIndexPool(const IndexPoolUpdated& a_updated);
	void CheckShadowTarget(const ShadowTargetPredicted& a_predicted);
	void CheckOwnViewport(const OwnShadowViewport& a_own);
	void CheckOwnCamera(const OwnShadowCamera& a_own);
	void CheckSunEntry(const ShadowViewLatched& a_latched);
	// CS_DCLF_TARGET_PROBE: the objects whose bound covers the pixel (ColourProbes.cpp).
	void ProbeTarget(const MainCullViewProj& a_cull);
	// CS_DCLF_FOLIAGE_PARITY: the compare pass's program and readbacks, its passes after the colour draws (Foliage.cpp).
	void MakeFoliageCompare(const FoliageRecordMade& a_made);
	void AppendFoliagePasses(const MainGraphPasses& a_passes);
	// CS_DCLF_FRAME_TRACE: every frame's adoption, coverage, main epochs, retirement point and the F10 marker (FrameTrace.cpp); the colour
	// commit's slot's last colour counts, from NoteCullLatch.
	void TraceSnapshotAdopted(const SnapshotAdopted& a_adopted);
	void TraceEpochCoverage(const EpochCoverageDecided& a_decided);
	void TraceMainEpoch(const MainEpochOutcome& a_outcome);
	void TraceAdoptedBound(const AdoptedBound& a_bound);
	void TraceColourCounts(const Draws::CounterReadback& a_readback, std::uint32_t a_slot);
	// TEMPORARY (decal loss): CS_DCLF_DECAL_DUMP=<frame>: every decal's pixel-stage inputs, logged once (DecalDump.cpp). InstallDecalDump
	// subscribes DumpDecals (MainEpochDone) and the Present count when the switch is set.
	void DumpDecals(const MainEpochDone& a_epoch);
	void InstallDecalDump();
	// CS_DCLF_PERSISTENT_PARITY: the catalog's constant tables against ShaderCache's (ConstantTables.cpp).
	void CheckConstantTables(const RE::BSShader& a_lighting, const Lookups& a_lookups);
}
