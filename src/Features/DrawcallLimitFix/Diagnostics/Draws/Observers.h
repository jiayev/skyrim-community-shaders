#pragma once

// The draws area's observers (Diagnostics/Draws/*.cpp), subscribed by InstallDraws (DrawsInstall.cpp) to the hook points of Events.h.

#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"

namespace DCLF::Diagnostics
{
	struct MainEpochDone;
	struct CullLatchWritten;
	struct FrameTextureUnresolved;
	struct FrameBufferUnfilled;
	struct TreeLodMissed;
	struct FadeInputsVaried;
	struct ShortBuffersDrawn;
	struct MainCullingSetup;
	struct FadeLogWindow;
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
	struct SunEntryPlanesTaken;
	struct OcclusionCaptured;
	struct ShadowViewLatched;
	struct MainCullViewProj;
	struct ReflectionFaceDrawn;
	struct FirstPersonSkeletonWalked;
	struct FoliageRecordMade;
	struct MainGraphPasses;

	// The culling's counters, every run (CullCounters.cpp).
	void NoteCullLatch(const CullLatchWritten& a_written);
	void ReadCullCounters(const MainEpochDone& a_epoch);
	// A frame texture the commit could not resolve, described once per register, every run (TextureCommit.cpp).
	void DescribeUnresolvedFrameTexture(const FrameTextureUnresolved& a_unresolved);
	// The main commits' warnings and setup log, every run (CommitLog.cpp).
	void LogUnfilledFrameBuffer(const FrameBufferUnfilled& a_unfilled);
	void LogTreeLodMissed(const TreeLodMissed& a_missed);
	void LogFadeInputsVaried(const FadeInputsVaried& a_varied);
	void LogShortBuffers(const ShortBuffersDrawn& a_drawn);
	void LogCullingSetup(const MainCullingSetup& a_setup);
	// CS_DCLF_BUILD_PARITY (BuildParity.cpp).
	void CheckBuildParity(const MainEpochDone& a_epoch);
	// CS_DCLF_FADE_PARITY (FadeLog.cpp).
	void NextFadeLog(const FadeLogWindow& a_window);
	void ReadFadeLog(const MainEpochDone& a_epoch);
	// CS_DCLF_SET_PARITY (SetParity.cpp).
	void CheckSetParity(const MainEpochDone& a_epoch);
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
	void CheckSunEntryPlanes(const SunEntryPlanesTaken& a_taken);
	void CheckOcclusionView(const OcclusionCaptured& a_captured);
	void CheckSunEntry(const ShadowViewLatched& a_latched);
	// CS_DCLF_TARGET_PROBE: the objects whose bound covers the pixel (ColourProbes.cpp).
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
