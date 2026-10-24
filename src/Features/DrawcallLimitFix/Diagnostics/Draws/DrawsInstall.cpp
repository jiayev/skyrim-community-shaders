// The draws area's observers (IndirectDraws' epochs, builds and readbacks; the draw pipelines' and the frame values' parities).

#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Engine/WindowWriters.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Events.h"
#include "CullCounters.h"
#include "Observers.h"

#include <utility>

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
		// A main epoch's readbacks, in this order: the build parity (both epochs), the culling's counters (always), the set parity (the
		// colour epoch's, with the frame's Z-prepass payload) and the set class parity (the colour epoch's); the features' after (their install: Features/TreeWind's tree wind, then
		// Features/Fades' fade log, the Z-prepass's).
		if (o.builds)
			Subscribe<MainEpochDone>(&CheckBuildParity);
		Subscribe<CullLatchWritten>(&NoteCullLatch);
		Subscribe<MainEpochDone>(&ReadCullCounters);
		Subscribe<MainGraphPasses>(&AppendCullCounterPass);
		Subscribe<FrameTextureUnresolved>(&DescribeUnresolvedFrameTexture);
		Subscribe<FrameBufferUnfilled>(&LogUnfilledFrameBuffer);
		Subscribe<FadeInputsVaried>(&LogFadeInputsVaried);
		Subscribe<ShortBuffersDrawn>(&LogShortBuffers);
		Subscribe<MainCullingSetup>(&LogCullingSetup);
		if (o.claims) {
			Subscribe<MainEpochDone>(&CheckSetParity);
			Subscribe<MainEpochDone>(&CheckSetClassParity);
			// Every resident slot's shading row sampled, read without waiting for the producer: the persistent parity's wait leaves no
			// engine-read lease refused, the race that left rows unsampled.
			Subscribe<MainEpochDone>([](const MainEpochDone& a_epoch) {
				static std::uint32_t epochs = 0;
				if (!a_epoch.depthOnly && ++epochs % 60 == 0)
					CheckShadingSampled(Scene::FrameView::Get().GetTables());
			});
		}
		if (o.scene) {
			Subscribe<ExtrasSending>(&MirrorExtras);
			Subscribe<ExtrasSent>(&CheckExtras);
			Subscribe(&CheckObjectRecords);
			Subscribe(&CheckGeometryDraws);
			Subscribe(&CheckDrawBound);
			Subscribe<ShadowBuilt>(&CheckKeptShadow);
			Subscribe<ExclusionRebuilt>(&CheckExclusionReuse);
			Subscribe<DrawsBuiltAhead>(&CheckViewMasks);
			Subscribe<SnapshotSealed>(&CheckSnapshotAddresses);
			Subscribe<CommitTargetStaged>(&CheckCommitTarget);
			Subscribe<RevisionShapesMade>(&CheckShapeCapacities);
			Subscribe<FramePoint::Present>([](const On<FramePoint::Present>&) { CheckNamedResolves(); });
		}
		if (o.builds)
			Subscribe<IndexPoolUpdated>(&CheckIndexPool);
		if (o.claimsOrScene) {
			Subscribe<ShadowTargetPredicted>(&CheckShadowTarget);
			Subscribe<OwnShadowViewport>(&CheckOwnViewport);
			Subscribe<OwnShadowCamera>(&CheckOwnCamera);
		}
		if (o.scene)
			Subscribe<ShadowViewLatched>(&CheckSunEntry);
		// The graph passes (MainGraphPasses): the foliage compare.
		if (o.foliage) {
			Subscribe<FoliageRecordMade>(&MakeFoliageCompare);
			Subscribe<MainGraphPasses>(&AppendFoliagePasses);
		}
		if (!o.targetProbe.empty())
			Subscribe<MainCullViewProj>(&ProbeTarget);
		// The frame trace, the corrupt-frame tool (its colour counts are NoteCullLatch's, above).
		if (o.frameTrace) {
			Subscribe<SnapshotAdopted>(&TraceSnapshotAdopted);
			Subscribe<EpochCoverageDecided>(&TraceEpochCoverage);
			Subscribe<MainEpochOutcome>(&TraceMainEpoch);
			Subscribe<AdoptedBound>(&TraceAdoptedBound);
		}
		// The frame values' rows and palettes against the engine's now. The cell animations job's update (inside Main::Draw, after the
		// sampling: WindowWriters.h) is held from before the culls until the check is done: the rows are the engine's before it, by design.
		if (o.scene) {
			Subscribe<FramePoint::ClaimsInstalled>([](const On<FramePoint::ClaimsInstalled>&) {
				if (ParityDue(Scene::FrameView::Get().GetFrame()))
					WindowWriters::Hold();
			});
			Subscribe<FramePoint::ShadowViewsBegin>([](const On<FramePoint::ShadowViewsBegin>&) {
				if (ParityDue(Scene::FrameView::Get().GetFrame())) {
					auto& store = Scene::Coordinator::Get();
					CheckFrameValues(Scene::FrameView::Get().GetTables(), store.PeekPlacementPlan());
				}
				WindowWriters::Release();
			});
			Subscribe<FramePoint::Present>([](const On<FramePoint::Present>&) { WindowWriters::Release(); });
		}
		// The frame's constant tables against the game's shader objects', for every entry the installed lookups resolved.
		if (o.scene)
			Subscribe<FramePoint::LookupsInstalled>([](const On<FramePoint::LookupsInstalled>&) {
				if (auto* lighting = EngineShaders::Lighting(); lighting && ShaderPrograms::Get().Enabled() && ParityDue(Scene::FrameView::Get().GetFrame()))
					CheckConstantTables(*lighting, Scene::FrameView::Get().GetLookups());
			});
		// The colour epoch's capture against the frame's first deferred lighting draw.
		if (o.capturePoint)
			Subscribe<NativeLightingDraw>([](const NativeLightingDraw& a_event) {
				if (a_event.deferred && a_event.mainPassCaptured && std::exchange(capturePointFrame, a_event.frame) != a_event.frame)
					CheckCapturePoint();
			});
	}
}
