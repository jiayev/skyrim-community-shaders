#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Draws/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Draws/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Draws/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EngineShaders.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Events.h"
#	include "Observers.h"
#endif

#include <utility>

// The draws area's observers (IndirectDraws' epochs, builds and readbacks; the draw pipelines' and the frame values' parities).

namespace DCLF::Diagnostics
{
	namespace
	{
		/** @brief CS_DCLF_CAPTURE_POINT_PARITY: the frame whose first deferred lighting draw was checked against the colour epoch's capture. */
		std::uint32_t capturePointFrame = ~0u;
	}

	void InstallDraws()
	{
		const auto& o = Observe();
#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
		// A main epoch's readbacks, in this order: the build parity (both epochs), the culling's counters (always), the tree wind,
		// the fade log (the Z-prepass's), the set parity (the colour epoch's, with the frame's Z-prepass payload).
		if (o.builds)
			Subscribe<MainEpochDone>(&CheckBuildParity);
		Subscribe<CullLatchWritten>(&NoteCullLatch);
		Subscribe<MainEpochDone>(&ReadCullCounters);
		if (o.scene)
			Subscribe<MainEpochDone>(&ReadTreeWind);
		if (o.fade) {
			Subscribe<FadeLogWindow>(&NextFadeLog);
			Subscribe<MainEpochDone>(&ReadFadeLog);
		}
		if (o.claims)
			Subscribe<MainEpochDone>(&CheckSetParity);
		if (o.scene) {
			Subscribe<ExtrasSending>(&MirrorExtras);
			Subscribe<ExtrasSent>(&CheckExtras);
			Subscribe<ObjectRecordsUpdated>(&CheckObjectRecords);
			Subscribe<GeometryDrawsUpdated>(&CheckGeometryDraws);
			Subscribe<DrawBoundUpdated>(&CheckDrawBound);
			Subscribe<ShadowBuilt>(&CheckKeptShadow);
			Subscribe<ExclusionRebuilt>(&CheckExclusionReuse);
			Subscribe<DrawsBuiltAhead>(&CheckViewMasks);
		}
		if (o.bindless) {
			Subscribe<MainBuildBegun>(&BeginBindlessChecks);
			Subscribe<MainRecordAssembled>(&CheckBindless);
		}
		if (o.residentDraws)
			Subscribe<ResidentRegionUpdated>(&CheckResidentRegion);
		if (o.bindless || o.residentDraws)
			Subscribe<MainPayloadCommitted>(&SumBuildChecks);
		if (o.builds)
			Subscribe<IndexPoolUpdated>(&CheckIndexPool);
		if (o.claimsOrScene) {
			Subscribe<ShadowTargetPredicted>(&CheckShadowTarget);
			Subscribe<OwnShadowViewport>(&CheckOwnViewport);
			Subscribe<OwnShadowCamera>(&CheckOwnCamera);
			Subscribe<SunEntryPlanesTaken>(&CheckSunEntryPlanes);
			Subscribe<OcclusionCaptured>(&CheckOcclusionView);
		}
		if (o.scene)
			Subscribe<ShadowViewLatched>(&CheckSunEntry);
		if (o.claims)
			Subscribe<ShadowViewLatched>(&CheckCascadeCulling);
		// The graph passes (MainGraphPasses), in their order at each point: the G-buffer probe's copies, then the foliage compare.
		if (!o.gbufferProbe.empty())
			Subscribe<MainGraphPasses>(&AppendProbePasses);
		if (o.foliage) {
			Subscribe<FoliageRecordMade>(&MakeFoliageCompare);
			Subscribe<MainGraphPasses>(&AppendFoliagePasses);
		}
		if (o.stats)
			Subscribe<ColourFrameBlock>(&MeasureJitter);
		if (!o.targetProbe.empty())
			Subscribe<MainCullViewProj>(&ProbeTarget);
		if (o.claimsOrScene)
			Subscribe<ReflectionFaceDrawn>(&CheckReflectionFace);
		if (o.claims)
			Subscribe<FirstPersonSkeletonWalked>(&NoteFirstPersonSkeleton);
		if (!o.gbufferProbe.empty()) {
			Subscribe<MainPassCaptured>(&NoteProbeTargets);
			Subscribe<ProbePoint>(&ProbeGBuffer);
		}
		// The frame values' rows and palettes against the engine's now.
		if (o.scene)
			Subscribe<ShadowViewsBegin>([](const ShadowViewsBegin& a_event) {
				if (ParityDue(a_event.frame)) {
					auto& store = Scene::Coordinator::Get();
					CheckFrameValues(Scene::FrameView::Get().GetTables(), store.PeekPlacementPlan());
				}
			});
		// The frame's constant tables against the game's shader objects', for every entry the installed lookups resolved.
		if (o.scene)
			Subscribe<LookupsInstalled>([](const LookupsInstalled& a_event) {
				if (auto* lighting = EngineShaders::Lighting(); lighting && ShaderPrograms::Get().Enabled() && ParityDue(a_event.frame))
					CheckConstantTables(*lighting, Scene::FrameView::Get().GetLookups());
			});
		// The colour epoch's capture against the frame's first deferred lighting draw.
		if (o.capturePoint)
			Subscribe<NativeLightingDraw>([](const NativeLightingDraw& a_event) {
				if (a_event.deferred && a_event.mainPassCaptured && std::exchange(capturePointFrame, a_event.frame) != a_event.frame)
					CheckCapturePoint();
			});
#endif
	}
}
