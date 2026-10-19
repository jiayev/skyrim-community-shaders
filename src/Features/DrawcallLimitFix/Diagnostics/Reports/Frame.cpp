// The render thread's frame report (Reports::RenderThread, from FrameDriver::EndFrame at Present): the features' render-thread lines
// (the sun accumulation's), the primary cull lines, the leak line, GPU timings and, with CS_DCLF_STATS, the frame's statistics.
#include "Schedule.h"
#include "Scene.h"
#include "Engine.h"
#include "Draws.h"

#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/RegistrationCensus.h"

#include "Deferred.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Reports
{
	/** @brief The frame's own report lines (the culling, the leaks, the GPU timings, and with CS_DCLF_STATS the full statistics). */
	void Stats(std::uint32_t frame, FrameDriver::Counters& c);

	void RenderThread(std::uint32_t a_frame)
	{
		DCLF::Extensions::ReportLinesAt(DCLF::Api::ReportPoint::RenderThread, a_frame);
		PrimaryCullLines(a_frame);
		Stats(a_frame, FrameDriver::Get().ReportCounters());
	}
}

void DCLF::Reports::Stats(std::uint32_t frame, FrameDriver::Counters& c)
{
	auto& store = DCLF::Scene::Coordinator::Get();
	auto& leaks = Leaks();
	auto& timing = c.timing;
	const bool due = Due(frame);
	// CS_DCLF_CENSUS: the passes the engine kept since the last frame, classified here (Present), reported every interval.
	if (DCLF::RegistrationCensus::Enabled()) {
		DCLF::RegistrationCensus::Collect();
		if (due) {
			std::istringstream censusLines(DCLF::RegistrationCensus::TakeReport());
			for (std::string line; std::getline(censusLines, line);)
				logger::info("{}", line);
		}
	}
	// DCLF's render-thread time, by what the asynchronous scene leaves on it (RenderThreadBudget): always reported.
	if (due)
		if (const auto line = DCLF::RenderThreadBudget::Get().Report(); !line.empty())
			logger::info("{}", line);
	// The culling's counters, reported whether or not the full statistics are on: they are what says
	// whether GPU culling is running and how much it rejects.
	if (due) {
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
	if (due) {
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		if (draws.rowTableConflicts)
			logger::warn("[DCLF] main rows: {} (material, pipeline) pairs met a pipeline whose constant tables differ from their material row's; those draws stay native",
				draws.rowTableConflicts);
	}
	if (due) {
		// The engine's side of the set: what the registrations withheld, and any native draw of a member (LEAK). The set's own lines and
		// the claims its passes took back are the coordinator's (Coordinator::ReportCoordinator, logged from its pass).
		const auto& capture = DCLF::PassCapture::Get().GetStats();
		std::string samples;
		for (const auto& sample : leaks.samples)
			samples += fmt::format("{}{}", samples.empty() ? "" : ", ", sample);
		const auto leaked = leaks.leakedInDepth + leaks.leakedInOpaque;
		// By taken draw family: its engine shapes' passes withheld (tree LOD's).
		std::string familiesWithheld;
		for (std::size_t f = 0; f < DCLF::Extensions::kDrawFamilies; ++f)
			if (const auto& family = DCLF::Extensions::DrawFamilies()[f]; family.taken)
				familiesWithheld += fmt::format("; {} {} passes withheld", capture.familyWithheld[f], family.label);
		logger::info("[DCLF] main camera since the last report: {} native passes drawn, {} of them set members' ({} depth, {} opaque){}{}; last frame's registrations: {} "
					 "member passes withheld{}",
			leaks.offered, leaked, leaks.leakedInDepth, leaks.leakedInOpaque, leaked ? " <- LEAK" : " <- OK", samples.empty() ? "" : "; first: " + samples,
			capture.mainWithheld, familiesWithheld);
		leaks = {};
	}
	// What the GPU spent on each segment, measured by the graph itself; the menu shows the latest window.
	if (due)
		RenderGraphRuntime::Get().ReportGpuTimings(kInterval, DCLF::Diagnostics::Observe().stats);
	if (due && !DCLF::Diagnostics::Observe().stats)
		timing = {};
	if (DCLF::Diagnostics::Observe().stats && due) {
		// The scene's statistics (the walk's, the set's, the tables', the materials', the scene report's) are the coordinator's, logged
		// from its pass as its frame crosses the interval (Coordinator::ReportCoordinator); the lines here read the frame's own state.
		const double frames = std::max(1u, timing.frames);
		const auto& shaders = DCLF::ShaderPrograms::Get().GetStats();
		logger::info("[DCLF] SPIR-V programs: {} requested, {} ready ({} stages from cache), {} failed; shadow (Utility): {} requested, {} ready, {} failed",
			shaders.requested, shaders.ready, shaders.fromCache, shaders.failed, shaders.shadowRequested, shaders.shadowReady, shaders.shadowFailed);
		const auto& indirect = DCLF::DrawPipelines::Get().GetStats();
		// Each taken draw family's lane ("tree LOD (lane): ..."), in family order.
		std::string familyLanes;
		for (const auto& family : indirect.drawFamilies)
			if (family.label)
				familyLanes += fmt::format("{} (lane): {} requested, {} built, {} failed{}, frames waited {}; ", family.label, family.requested, family.ready, family.failed,
					family.failed ? " <- FAILED" : "", family.waits);
		logger::info("[DCLF] indirect pipelines: {} requested, {} in the set, {} failed ({} Z-prepass pipelines, {} without a pixel stage; {} state classes, the largest of {}); "
					 "target changes {}; shadow: {} requested, {} in the set, {} failed; "
					 "set versions published {} (waited {} frames), shadow {} (waited {}); {}forward (lane): {} requested, {} built, {} failed{}, frames waited {}",
			indirect.requested, indirect.ready, indirect.failed, indirect.zPipelines, indirect.zDepthOnly, indirect.stateClasses, indirect.largestClass, indirect.targetChanges,
			indirect.shadowRequested, indirect.shadowReady, indirect.shadowFailed,
			indirect.setPublishes, indirect.setWaits, indirect.shadowSetPublishes, indirect.shadowSetWaits, familyLanes, indirect.forwardRequested, indirect.forwardReady,
			indirect.forwardFailed, indirect.forwardFailed ? " <- FAILED" : "", indirect.forwardWaits);
		// The constant tables the builds pack by are the lane's (DCLF's modules reflected); ShaderCache's are the observers'.
		DCLF::Reports::ConstantTableLines();
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
		logger::info("[DCLF] indirect draws (last frame): {} candidates built from {} material rows, skipped:{}; {:.1f} MB written by the build (the catch-up: payload pools), {:.3f} ms CPU; {} epochs, {} not ready; textures {} live in {} registry slots, {} cleanup pending, rejected {}/{}/{}, {} samplers; imports: {} made off the render thread; requests (the scene work's): {} answered, {} not answered yet",
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
			DCLF::IndirectDraws::Get().ResetCommitTimings();
		}
		std::istringstream reportLines(DCLF::IndirectDraws::Get().AsyncReport() + DCLF::Reports::SceneFrameLines());
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
			// The features' lines here (Features/OcclusionMaps: the occlusion maps'), before the shadow counters' reset.
			DCLF::Extensions::ReportLinesAt(DCLF::Api::ReportPoint::ShadowViews, frame);
			DCLF::IndirectDraws::Get().ResetShadowStats();
		}
		if (auto* stats = DCLF::Draws::PassStats::live.load())
			stats->LogDecals();  // TEMPORARY (decal loss)
		if (draws.decalsDrawn || draws.decalsTested)
			logger::info("[DCLF] decals (last frame; the candidates are the coordinator's line): {} submitted to the second pass, {} of {} tested were culled",
				draws.decalsDrawn, draws.decalsCulled, draws.decalsTested);
		if (draws.sunTested || draws.sunCpuTested)
			logger::info("[DCLF] sun on the GPU (sampled frame): {} synthetic draws tested, {} missed every cascade; the CPU over the same inputs: {} tested, {} missed{}",
				draws.sunTested, draws.sunMissed, draws.sunCpuTested, draws.sunCpuMissed,
				(draws.sunTested == draws.sunCpuTested && draws.sunMissed == draws.sunCpuMissed) ? " <- OK" : " <- DIFFERS");
		if (draws.terrainNearTested || draws.terrainCpuTested)
			logger::info("[DCLF] terrain under Terrain Blending (sampled frame): {} group 1 inputs, {} near (its terrain depth, and offset into the main depth){}",
				draws.terrainNearTested, draws.terrainNear,
				!draws.terrainCpu ? std::string() :
				fmt::format("; the CPU over the same inputs: {} tested, {} near{}", draws.terrainCpuTested, draws.terrainCpuNear,
					(draws.terrainNearTested == draws.terrainCpuTested && draws.terrainNear == draws.terrainCpuNear) ? " <- OK" : " <- TB NEAR"));
		// The forward epoch's class, the colour segment's culling of it, and (CS_DCLF_SET_PARITY) what the engine's registration
		// handed in for it that frame: every blended pass of a member the engine would draw is a forward input in view (its cull has no
		// occlusion and a tighter bound than the box the GPU tests), so fewer in view than registered is a member DCLF dropped.
		if (draws.forwardTested || draws.forwardDrawn || draws.forwardEngine)
			logger::info("[DCLF] forward (T7d, sampled frame): {} forward inputs tested, {} in the frustum, {} occluded, {} draws sorted for the forward epoch{}",
				draws.forwardTested, draws.forwardInView, draws.forwardOccluded, draws.forwardDrawn,
				!draws.forwardObserved ? std::string() :
				fmt::format("; the engine registered {} blended passes of members{}", draws.forwardEngine,
					draws.forwardInView >= draws.forwardEngine ? " <- OK" : " <- FORWARD"));
		// The colour draws dropped for want of a state class sub-range: none may be (each one an object not drawn), and none is ever drawn
		// with another class's fixed-function state.
		if (indirect.stateClasses)
			logger::info("[DCLF] state classes (sampled frame): {} colour draws without a class sub-range, {} past their sub-range's capacity{}", draws.classUnplanned,
				draws.classOverflow, draws.classUnplanned || draws.classOverflow ? " <- SET CLASS DROP" : " <- OK");
		// CS_DCLF_SET_PARITY: every published set version's classes against their members (DrawPipelines' CheckClasses: class words, each
		// member's state against its class's initial pipeline's, the handles written, each pipeline in one class, no two classes one state).
		// Cumulative; each failure was logged when found ("<- SET CLASS").
		if (indirect.classParityChecks)
			logger::info("[DCLF] set classes (CPU, published versions so far): {} checks, {} failed{}", indirect.classParityChecks, indirect.classParityMismatches,
				indirect.classParityMismatches ? " <- SET CLASS" : " <- OK");
		// Refraction, likewise: every refraction pass (hint 4) of a member the engine registered is a refraction input in view.
		if (draws.refractionTested || draws.refractionDrawn || draws.refractionEngine)
			logger::info("[DCLF] refraction (T7d, sampled frame): {} refraction inputs tested, {} in the frustum, {} occluded or under a faded root, {} draws "
						 "into the refraction normals{}",
				draws.refractionTested, draws.refractionInView, draws.refractionOccluded, draws.refractionDrawn,
				!draws.refractionObserved ? std::string() :
				fmt::format("; the engine registered {} refraction passes of members{}", draws.refractionEngine,
					draws.refractionInView >= draws.refractionEngine ? " <- OK" : " <- REFRACTION"));
		if (draws.residentInputs || draws.residentResyncs)
			logger::info("[DCLF] persistent draws (last frame, colour): {} kept inputs, {} sequences, {} pairs, {} not drawable; since the start {} region "
						 "versions uploaded, {} resyncs (log {}, segment {}, shrunk {}, scope {}, fit {}), {} decal count moves",
				draws.residentInputs, draws.residentDraws, draws.residentPairs, draws.residentUndrawable, draws.residentEpochs, draws.residentResyncs,
				draws.residentResyncBy[0], draws.residentResyncBy[1], draws.residentResyncBy[2], draws.residentResyncBy[3], draws.residentResyncBy[4], draws.residentDecalRetakes);
		if (draws.fadeTested)
			logger::info("[DCLF] fade on the GPU (sampled frame): {} draws under a fade root in view, {} dropped by their root's fade; {} roots",
				draws.fadeTested, draws.fadeHidden, draws.fadeRoots);
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
		// The render thread's scene time a frame (the scene passes are the coordinator's: its "scene pump" and "scene pass CPU" lines).
		logger::info("[DCLF] render thread scene CPU per frame (T6b3d): Present and the frame start's requests and captures {:.3f} ms; BeforeShadowMaps' views {:.3f} "
					 "(max {:.3f}); EarlyPrepass's posts and material serving {:.3f} (max {:.3f})",
			timing.eventsMs / frames, timing.sceneMs / frames, timing.sceneMaxMs, timing.buildMs / frames, timing.buildMaxMs);
		// The joins' frame inputs: render thread (EarlyPrepass) -> the next scene pass, latest wins.
		if (const auto inputs = store.TakeAccumulateInputStats(); inputs.ancestorMapsPosted || inputs.pipelineFramesPosted || inputs.pipelineBlocksMade || inputs.pipelineBlocksWaited)
			logger::info("[DCLF] accumulate frame inputs (T6b2c step 8): room map {} posted, {} taken; pipeline frame {} posted, {} taken; {} new pipelines' blocks made by "
						 "the coordinator, {} waited for a pipeline frame",
				inputs.ancestorMapsPosted, inputs.ancestorMapsTaken, inputs.pipelineFramesPosted, inputs.pipelineFramesTaken, inputs.pipelineBlocksMade, inputs.pipelineBlocksWaited);
		{
			const auto installs = DCLF::Scene::FrameView::Get().TakePublicationStats();
			// A publication is installed with the snapshot that carries it (the snapshots' own counters: IndirectDraws' report).
			logger::info("[DCLF] scene publications (T6b3b: installed with their snapshot): {} installed ({} skipped past by the builder), {} frames whose snapshot brought "
						 "no newer one",
				installs.installed, installs.skipped, installs.kept);
		}
		// The features' lines here (Features/HiddenNodes: CS_DCLF_HIDDEN_WATCH's watch of the hidden bit stores).
		DCLF::Extensions::ReportLinesAt(DCLF::Api::ReportPoint::Statistics, frame);
		timing = {};
	}
}
