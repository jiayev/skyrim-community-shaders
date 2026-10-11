#include "Features/DrawcallLimitFix.h"
#include "Features/DrawcallLimitFix/Diagnostics/HiddenWatch.h"

#include "Deferred.h"
#include "CaptureParity.h"
#include "Features/DrawcallLimitFix/Draws/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Draws/GpuResources.h"
#include "Features/DrawcallLimitFix/Draws/GpuTextures.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Common/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Engine/ImportTimings.h"
#include "Features/DrawcallLimitFix/Engine/PassCapture.h"
#include "Features/DrawcallLimitFix/Engine/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "Features/DrawcallLimitFix/Draws/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Engine/ShadowViews.h"
#include "Features/DrawcallLimitFix/Engine/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Common/Toggles.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "State.h"

namespace DCLF::Scene
{
	std::string TakeHiddenSiteReport();
}

void DrawcallLimitFix::ReportStats(std::uint32_t frame)
{
	auto& store = DCLF::SceneStore::Get();
	// DCLF's render-thread time, by what the asynchronous scene leaves on it (RenderThreadBudget): always reported.
	if ((frame % kReportInterval) == 0)
		if (const auto line = DCLF::RenderThreadBudget::Get().Report(); !line.empty())
			logger::info("{}", line);
	// The culling's counters, reported whether or not the full statistics are on: they are what says
	// whether GPU culling is running and how much it rejects.
	if ((frame % kReportInterval) == 0) {
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		if (draws.cullDrawn || draws.cullRejected || draws.cullOccluded) {
			const std::uint32_t rejected = draws.cullRejected + draws.cullOccluded;
			logger::info("[DCLF] culling: {} draws written, {} of {} tested were rejected ({:.1f}%: {} outside the frustum, {} occluded)",
				draws.cullDrawn, rejected, draws.cullTested,
				draws.cullTested ? 100.0 * rejected / draws.cullTested : 0.0,
				draws.cullRejected, draws.cullOccluded);
			if (draws.cullRescuedByPhaseTwo || draws.cullDrawnPhaseTwo)
				logger::info("[DCLF] two-phase culling: phase 2 brought back {} objects the stale HZB had rejected, and drew depth for {} of them",
					draws.cullRescuedByPhaseTwo, draws.cullDrawnPhaseTwo);
			if (draws.hzbSampled)
				logger::info("[DCLF] HZB: {} footprints sampled, {} came back all-near (~0), {} all-far (~1)",
					draws.hzbSampled, draws.hzbNear, draws.hzbFar);
			if (draws.hzbSample.valid) {
				const auto& sample = draws.hzbSample;
				logger::info("[DCLF] HZB rejection sample: farthest {:.6f} against nearest {:.6f}, uv ({:.4f} {:.4f})-({:.4f} {:.4f}), mip {}",
					sample.farthest, sample.nearestZ, sample.uvMin[0], sample.uvMin[1], sample.uvMax[0], sample.uvMax[1],
					sample.mip);
			}
		}
	}
	// The bindless record's own gate, reported whenever it is on: it says whether the table the shaders
	// read holds what the constant buffer path would have given them, which no screenshot can say.
	if ((frame % kReportInterval) == 0) {
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		if (draws.rowTableConflicts)
			logger::warn("[DCLF] main rows: {} (material, pipeline) pairs met a pipeline whose constant tables differ from their material row's; those draws stay native",
				draws.rowTableConflicts);
		if (draws.bindlessParityChecks) {
			if (draws.bindlessParityMismatches)
				logger::warn("[DCLF] bindless record parity MISMATCH: {} of {} components differ", draws.bindlessParityMismatches, draws.bindlessParityChecks);
			else
				logger::info("[DCLF] bindless record parity OK: {} components match the constant groups", draws.bindlessParityChecks);
		}
	}
	if ((frame % kReportInterval) == 0) {
		// The engine's side of the set: what the registrations withheld, and any native draw of a member (LEAK). The set's own lines and
		// the claims its passes took back are the coordinator's (T6b3d: SceneStore::ReportCoordinator, logged from its pass).
		const auto& capture = DCLF::PassCapture::Get().GetStats();
		std::string samples;
		for (const auto& sample : leaks.samples)
			samples += fmt::format("{}{}", samples.empty() ? "" : ", ", sample);
		const auto leaked = leaks.leakedInDepth + leaks.leakedInOpaque;
		const auto fadeTotals = DCLF::PassCapture::Get().TakeFadeTotals();
		logger::info("[DCLF] main camera since the last report: {} native passes drawn, {} of them set members' ({} depth, {} opaque){}{}; last frame's registrations: {} "
					 "member passes withheld, {} cross-fade copies and {} fades DCLF does not model left to the engine ({} and {} since the last report); {} tree LOD passes withheld",
			leaks.offered, leaked, leaks.leakedInDepth, leaks.leakedInOpaque, leaked ? " <- LEAK" : " <- OK", samples.empty() ? "" : "; first: " + samples,
			capture.mainWithheld, capture.mainCrossfadeCopies, capture.mainUnmodelledFades, fadeTotals.first, fadeTotals.second, capture.treeLodWithheld);
		leaks = {};
	}
	// What the GPU spent on each segment, measured by the graph itself; the menu shows the latest window.
	if ((frame % kReportInterval) == 0)
		RenderGraphRuntime::Get().ReportGpuTimings(kReportInterval, DCLF::SwitchEnabled(DCLF::Switch::Stats));
	if ((frame % kReportInterval) == 0 && !DCLF::SwitchEnabled(DCLF::Switch::Stats))
		timing = {};
	if (DCLF::SwitchEnabled(DCLF::Switch::Stats) && (frame % kReportInterval) == 0) {
		// T6b3d: the scene's statistics (the walk's, the set's, the tables', the materials', the scene report's) are the coordinator's, logged
		// from its pass as its frame crosses the interval (SceneStore::ReportCoordinator); the lines here read the frame's own state.
		const double frames = std::max(1u, timing.frames);
		const auto& shaders = DCLF::ShaderPrograms::Get().GetStats();
		logger::info("[DCLF] SPIR-V programs: {} requested, {} ready ({} stages from cache), {} failed; shadow (Utility): {} requested, {} ready, {} failed",
			shaders.requested, shaders.ready, shaders.fromCache, shaders.failed, shaders.shadowRequested, shaders.shadowReady, shaders.shadowFailed);
		const auto& indirect = DCLF::DrawPipelines::Get().GetStats();
		logger::info("[DCLF] indirect pipelines: {} requested, {} in the set, {} failed ({} Z-prepass pipelines, {} without a pixel stage); target changes {}; shadow: {} requested, {} in the set, {} failed; "
					 "set versions published {} (waited {} frames), shadow {} (waited {}); tree LOD (lane): {} requested, {} built, {} failed{}, frames waited {}; forward (lane): {} "
					 "requested, {} built, {} failed{}, frames waited {}",
			indirect.requested, indirect.ready, indirect.failed, indirect.zPipelines, indirect.zDepthOnly, indirect.targetChanges, indirect.shadowRequested, indirect.shadowReady, indirect.shadowFailed,
			indirect.setPublishes, indirect.setWaits, indirect.shadowSetPublishes, indirect.shadowSetWaits, indirect.treeLodRequested, indirect.treeLodReady, indirect.treeLodFailed,
			indirect.treeLodFailed ? " <- FAILED" : "", indirect.treeLodWaits, indirect.forwardRequested, indirect.forwardReady, indirect.forwardFailed,
			indirect.forwardFailed ? " <- FAILED" : "", indirect.forwardWaits);
		// T6b2c: the constant tables the builds pack by are the lane's (DCLF's modules reflected); ShaderCache's are the parity's.
		if (const auto ct = DCLF::DrawPipelines::Get().TakeConstantTableParity(); ct.checks)
			logger::info("[DCLF] constant tables (T6b2c: DCLF's modules' against ShaderCache's): {} checks, {} entries ({} without ShaderCache's shaders yet), {} "
						 "variables in both, {} at another offset{}{}; in one table only: {} DCLF's (a variable the game's shader lacks{}), {} ShaderCache's (DCLF_BINDLESS's "
						 "own blocks, a block the stage never reads)",
				ct.checks, ct.entries, ct.uncached, ct.variables, ct.differ, ct.differ ? " <- CONSTANT TABLE; first: " : " <- OK", ct.first, ct.catalogOnly,
				ct.catalogOnly ? "; first: " + ct.catalogOnlyFirst : std::string(), ct.cacheOnly);
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		std::string skipped;
		for (std::size_t i = 0; i < draws.skipped.size(); ++i) {
			if (draws.skipped[i])
				skipped += fmt::format(" {}={}", DCLF::kSkipNames[i], draws.skipped[i]);
		}
		// A sample of what the texture and constants skips were missing, when there were any.
		std::string missing;
		if (draws.skipped[static_cast<std::size_t>(DCLF::DrawSkip::Texture)])
			missing += fmt::format(" t{} t{} t{} t{}", draws.missingTextures[0], draws.missingTextures[1], draws.missingTextures[2], draws.missingTextures[3]);
		if (draws.skipped[static_cast<std::size_t>(DCLF::DrawSkip::Constants)])
			missing += fmt::format(" VS b{:04X} PS b{:04X}", draws.missingVertexConstants, draws.missingPixelConstants);
		if (!missing.empty())
			skipped += " (missing" + missing + ")";
		const auto& textures = DCLF::GpuTextures::Get().GetStats();
		logger::info("[DCLF] indirect draws (last frame): {} candidates built from {} material rows, skipped:{}; {:.1f} MB uploaded, {:.3f} ms CPU; {} epochs, {} not ready; textures {} live in {} registry slots, {} cleanup pending, rejected {}/{}/{}, {} samplers; imports: {} made off the render thread; requests (the scene work's): {} answered, {} not answered yet",
			draws.drawn, draws.records, skipped, draws.uploadBytes / 1048576.0,
			draws.cpuMs, draws.epochs, draws.notReady, textures.cached, textures.registrySlots, textures.cleanupPending,
			textures.rejected[1], textures.rejected[2], textures.rejected[3], textures.samplers, textures.importedAsync,
			textures.requestsAnswered, textures.requestsPending);
		// A register a pipeline reads that the commit does not supply rejects every pair of that pipeline: its members are
		// withheld from the engine and drawn by nobody.
		if (const auto constants = draws.skipped[static_cast<std::size_t>(DCLF::DrawSkip::Constants)])
			logger::warn("[DCLF] {} pairs read constant registers no frame slot supplies (VS b{:04X} PS b{:04X}): their members are not drawn",
				constants, draws.missingVertexConstants, draws.missingPixelConstants);
		if (draws.frameTexturesMissing) {
			std::string registers;
			for (std::uint32_t t = 0; t < 128; ++t) {
				if ((draws.frameTexturesMissingRegisters[t >> 6] >> (t & 63)) & 1)
					registers += fmt::format(" t{}", t);
			}
			logger::warn("[DCLF] frame textures (last colour epoch): {} reads of registers the commit could not resolve, which read zero:{}", draws.frameTexturesMissing, registers);
		}
		std::string epochParts;
		for (std::size_t i = 0; i < draws.partMs.size(); ++i)
			epochParts += fmt::format("{}{} {:.3f} ms", epochParts.empty() ? "" : ", ", DCLF::kEpochPartNames[i], draws.partMs[i]);
		// The build's parts are measured where the payload was built, and "graph execute" is everything the render thread spent on
		// the epoch.
		logger::info("[DCLF] indirect epoch CPU by part (build parts where built): {}", epochParts);
		if (draws.commitEpochs) {
			static constexpr std::array<const char*, 7> kCommitParts{ "join", "lookups", "frame textures and patches", "frame blocks",
				"payload uploads", "drawn set", "rest" };
			std::string commitParts;
			for (std::size_t i = 0; i < kCommitParts.size(); ++i)
				commitParts += fmt::format("{}{} {:.1f}", commitParts.empty() ? "" : ", ", kCommitParts[i], draws.commitUs[i] / draws.commitEpochs);
			logger::info("[DCLF] main epoch commit on the render thread, us per epoch over {} epochs: {}", draws.commitEpochs, commitParts);
			if (const auto refused = DCLF::IndirectDraws::Get().TakeStreamsRefused())
				logger::info("[DCLF] scene stream views a fallback wanted while the coordinator or the builds task ran (none made): {}", refused);
			DCLF::IndirectDraws::Get().ResetCommitTimings();
		}
		std::istringstream reportLines(DCLF::IndirectDraws::Get().AsyncReport() + DCLF::SceneStore::Get().FrameSceneReport());
		for (std::string line; std::getline(reportLines, line);)
			logger::info("{}", line);
		if (DCLF::ActiveToggles().shadows) {
			const auto& shadow = DCLF::IndirectDraws::Get().GetShadowStats();
			std::string notReadyReasons;
			for (std::size_t r = 0; r < shadow.notReadyReasons.size(); ++r) {
				if (shadow.notReadyReasons[r])
					notReadyReasons += fmt::format(" {}={}", DCLF::kShadowNotReadyNames[r], shadow.notReadyReasons[r]);
			}
			logger::info("[DCLF] shadow views: {} offered, {} drawn in {} epochs ({} slots kept with no work), {} not ready ({}), {} focus views and {} uncovered left native; last mode {} inputs ({} without a pipeline, {} without a texture), {} records; CPU {:.3f} ms per frame ({:.3f} capturing, {:.3f} preparing, {:.3f} inputs, {:.3f} blocks, {:.3f} graph)",
				shadow.views, shadow.viewsDrawn, shadow.epochs, shadow.retainedViews, shadow.notReady, notReadyReasons.empty() ? "-" : notReadyReasons.c_str() + 1, shadow.focusSkipped, shadow.uncovered, shadow.inputs, shadow.skippedPipeline,
				shadow.skippedTexture, shadow.records, (shadow.cpuMs + shadow.captureMs) / frames, shadow.captureMs / frames, shadow.prepareMs / frames, shadow.inputsMs / frames, shadow.blocksMs / frames,
				shadow.executeMs / frames);
			if (shadow.views)
				logger::info("{}", DCLF::IndirectDraws::Get().ShadowCapabilityReport());
			if (shadow.cullTested || shadow.cullClass)
				logger::info("[DCLF] shadow culling (view {} mode {:#x}, sampled): {} of the caster class, {} outside the sun's entry, {} small, {} under a fading root; "
							 "{} tested, {} drawn, {} rejected by the frustum",
					shadow.cullSampledView, shadow.cullSampledMode, shadow.cullClass, shadow.cullSunEntryOut, shadow.cullMinRadius, shadow.cullRootFading,
					shadow.cullTested, shadow.cullDrawn, shadow.cullRejected);
			if (shadow.sunEntryChecks)
				logger::info("[DCLF] sun entry on the GPU: {} inputs checked against the CPU's verdict (sampled frames), {} differ{}", shadow.sunEntryChecks,
					shadow.sunEntryMismatches, shadow.sunEntryMismatches ? " <- DIFFER" : " <- OK");
			if (DCLF::PassCapture::ShadowWithholdingEnabled()) {
				const auto& captured = DCLF::PassCapture::Get().GetStats();
				logger::info("[DCLF] shadow views by the set: withheld plain {} / clamped {} / paraboloid {} passes, {} volumetric-only passes and {} of hints 11, 7 and 3 (last frame); the set's casters {} / {} / {} (last epoch); {} members a shadow build could not draw{}{}; {} face regions uploaded; {} views not ready, {} of them with withheld casters{}",
					captured.shadowWithheld[0], captured.shadowWithheld[1], captured.shadowWithheld[2], captured.volumetricWithheld, captured.directWithheld, shadow.casters[0], shadow.casters[1],
					shadow.casters[2], shadow.setWaiting, shadow.setWaiting ? " <- SET SHADOW" : "", shadow.setWaitingFirst.empty() ? "" : " (first: " + shadow.setWaitingFirst + ")", shadow.faceUploads, shadow.notReady, shadow.notReadyWithheld, shadow.notReadyWithheld ? " <- HOLES" : "");
			}
			for (std::uint32_t v = 0; v < DCLF::kOcclusionViews; ++v)
				if (shadow.occlusionDrawn[v] || shadow.occlusionNotReady[v] || occlusionNativeFrames[v])
					logger::info("[DCLF] {} occlusion: DCLF drew {} maps ({} of the set's occluders, last), {} it could not draw, {} left to the engine whole; the engine "
								 "registered the map's other occluders on {} frames ({} not members now); {} member passes withheld (last frame)",
						v == DCLF::kOcclusionSky ? "Skylighting" : "precipitation", shadow.occlusionDrawn[v], shadow.occlusionInputs[v], shadow.occlusionNotReady[v],
						occlusionNativeFrames[v], occlusionEngineFrames[v], store.SetLacking(v == DCLF::kOcclusionSky ? DCLF::kSetOccluderSky : DCLF::kSetOccluderPrecipitation),
						DCLF::PassCapture::Get().GetStats().occlusionWithheld);
			if (shadow.occlusionEpochs)
				logger::info("[DCLF] occlusion maps: {} epochs, render thread {:.3f} ms per epoch", shadow.occlusionEpochs, shadow.occlusionMs / shadow.occlusionEpochs);
			occlusionNativeFrames = {};
			occlusionEngineFrames = {};
			DCLF::IndirectDraws::Get().ResetShadowStats();
		}
		if (draws.decalsDrawn || draws.decalsTested)
			logger::info("[DCLF] decals (last frame; the candidates are the coordinator's line): {} submitted to the second pass, {} of {} tested were culled",
				draws.decalsDrawn, draws.decalsCulled, draws.decalsTested);
		if (draws.sunTested || draws.sunCpuTested)
			logger::info("[DCLF] sun on the GPU (sampled frame): {} synthetic draws tested, {} missed every cascade; the CPU over the same inputs: {} tested, {} missed{}",
				draws.sunTested, draws.sunMissed, draws.sunCpuTested, draws.sunCpuMissed,
				(draws.sunTested == draws.sunCpuTested && draws.sunMissed == draws.sunCpuMissed) ? " <- OK" : " <- DIFFERS");
		if (draws.residentInputs || draws.residentResyncs)
			logger::info("[DCLF] persistent draws (last frame, colour): {} kept inputs, {} sequences, {} pairs, {} not drawable; since the start {} region "
						 "versions uploaded, {} resyncs (log {}, segment {}, shrunk {}, scope {}, fit {}), {} decal count moves; parity {} entries checked, {} differ, {} missing{}",
				draws.residentInputs, draws.residentDraws, draws.residentPairs, draws.residentUndrawable, draws.residentVersions, draws.residentResyncs,
				draws.residentResyncBy[0], draws.residentResyncBy[1], draws.residentResyncBy[2], draws.residentResyncBy[3], draws.residentResyncBy[4], draws.residentDecalRetakes,
				draws.residentParityChecks, draws.residentParityMismatches, draws.residentMissing,
				draws.residentParityChecks ? fmt::format("; pairs {} checked, {} resolved with no event{}", draws.residentPairsChecked, draws.residentPairsStale,
												 draws.residentParityMismatches || draws.residentMissing || draws.residentPairsStale ? " <- RESIDENT DRAW PARITY" : " <- OK") :
											 std::string());
		if (draws.fadeTested)
			logger::info("[DCLF] fade on the GPU (sampled frame): {} draws under a fade root in view, {} dropped by their root's fade; {} roots",
				draws.fadeTested, draws.fadeHidden, draws.fadeRoots);
		if (draws.drawsWaiting)
			logger::info("[DCLF] {} draws so far waited for the sequences' growth (past the current ones, dropped by BuildDraws)", draws.drawsWaiting);
		if (draws.shortBuffers)
			logger::warn("[DCLF] {} draws of the last epoch reach past their vertex or index buffer slice", draws.shortBuffers);
		// The scene work's registry and the render thread's (tree LOD's meshes), each its owner's; their counters read here.
		const auto gpu = DCLF::GpuResources::Scene().GetStats();
		const auto treeGpu = DCLF::GpuResources::Frame().GetStats();
		logger::info("[DCLF] game buffers for the render graph: {} stable, {} rejected; {} resolved so far in {:.1f} ms (slowest {:.3f} ms); parity: {} prefetched "
					 "descriptions taken, {} of buffers the game had released by then (held by the prefetch's reference); tree LOD's (render thread): {} stable, "
					 "{} rejected, {} resolved in {:.1f} ms (slowest {:.3f} ms)",
			gpu.cached, gpu.rejected, gpu.resolvedTotal, gpu.resolveMsTotal, gpu.resolveMsMax, gpu.prefetchTaken, gpu.prefetchReleased, treeGpu.cached,
			treeGpu.rejected, treeGpu.resolvedTotal, treeGpu.resolveMsTotal, treeGpu.resolveMsMax);
		const auto& capture = DCLF::PassCapture::Get().GetStats();
		logger::info("[DCLF] pass capture: {} registrations from {} threads ({} overflowed)", capture.captured, capture.threads, capture.overflowed);
		// The render thread's scene time a frame (T6b3d: the scene passes are the coordinator's: its "scene pump" and "scene pass CPU" lines).
		logger::info("[DCLF] render thread scene CPU per frame (T6b3d): Present and the frame start's requests and captures {:.3f} ms; BeforeShadowMaps' views {:.3f} "
					 "(max {:.3f}); EarlyPrepass's posts and material serving {:.3f} (max {:.3f})",
			timing.eventsMs / frames, timing.sceneMs / frames, timing.sceneMaxMs, timing.buildMs / frames, timing.buildMaxMs);
		// The joins' frame inputs (T6b2c step 8): render thread (EarlyPrepass) -> the next scene pass (T6b3c), latest wins.
		if (const auto inputs = store.TakeAccumulateInputStats(); inputs.roomMapsPosted || inputs.pipelineFramesPosted || inputs.pipelineBlocksMade || inputs.pipelineBlocksWaited)
			logger::info("[DCLF] accumulate frame inputs (T6b2c step 8): room map {} posted, {} taken; pipeline frame {} posted, {} taken; {} new pipelines' blocks made by "
						 "the coordinator, {} waited for a pipeline frame",
				inputs.roomMapsPosted, inputs.roomMapsTaken, inputs.pipelineFramesPosted, inputs.pipelineFramesTaken, inputs.pipelineBlocksMade, inputs.pipelineBlocksWaited);
		if (const auto [violations, first] = store.TakeFrameAccessViolations(); violations)
			logger::error("[DCLF] step 6c: {} reads of the coordinator's state from the frame while the scene work ran (first: {}) <- FRAME ACCESS", violations,
				first ? first : "?");
		{
			const auto installs = store.TakePublicationStats();
			// T6b3b: a publication is installed with the snapshot that carries it (the snapshots' own counters: IndirectDraws' report).
			logger::info("[DCLF] scene publications (T6b3b: installed with their snapshot): {} installed ({} skipped past by the builder), {} frames whose snapshot brought "
						 "no newer one",
				installs.installed, installs.skipped, installs.kept);
			if (DCLF::SceneStore::TimelineEnabled()) {
				// T6b0: the frames from an event to the set, and from the set's commit to the frame that installs it.
				auto histogram = [](const auto& a_buckets) {
					std::string text;
					for (std::size_t b = 0; b < a_buckets.size(); ++b)
						if (a_buckets[b])
							text += fmt::format("{}{}:{}", text.empty() ? "" : " ", DCLF::SceneStore::kAgeBucketNames[b], a_buckets[b]);
					return text.empty() ? std::string("-") : text;
				};
				logger::info("[DCLF] latency (T6b0, frames; the event's stages are the coordinator's line): commit to installation (by geometries joined) {}",
					histogram(installs.installDelay));
			}
		}
		// CS_DCLF_HIDDEN_WATCH: the render thread's watch of the hidden bit stores.
		if (const auto watch = DCLF::HiddenWatch::TakeReport(frame); !watch.empty()) {
			std::istringstream watchLines(watch);
			for (std::string line; std::getline(watchLines, line);)
				logger::info("{}", line);
		}
		timing = {};
	}
}
