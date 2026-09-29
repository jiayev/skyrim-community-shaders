#include "Features/DrawcallLimitFix.h"

#include "Deferred.h"
#include "CaptureParity.h"
#include "Features/DrawcallLimitFix/Draws/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Draws/GpuResources.h"
#include "Features/DrawcallLimitFix/Draws/GpuTextures.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
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

void DrawcallLimitFix::ReportStats(std::uint32_t frame)
{
	auto& store = DCLF::SceneStore::Get();
	// The culling's counters, reported whether or not the full statistics are on: they are what says
	// whether GPU culling is running and how much it rejects.
	if ((frame % kReportInterval) == 0) {
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		if (draws.cullDrawn || draws.cullRejected || draws.cullOccluded) {
			const std::uint32_t rejected = draws.cullRejected + draws.cullOccluded;
			logger::info("[DCLF] culling: {} draws written, {} of {} tested were rejected ({:.1f}%: {} outside the frustum, {} occluded, {} of those the engine had kept); against the engine: {} it culled were gated out, {} it culled were kept, {} it kept the frustum test rejected{}",
				draws.cullDrawn, rejected, draws.cullTested,
				draws.cullTested ? 100.0 * rejected / draws.cullTested : 0.0,
				draws.cullRejected, draws.cullOccluded, draws.cullOccludedVisible,
				draws.cullEngineCulled, draws.cullRescued, draws.cullFalseNegatives,
				draws.cullFalseNegatives ? " <- FALSE NEGATIVES" : "");
			if (draws.cullRescuedByPhaseTwo || draws.cullDrawnPhaseTwo)
				logger::info("[DCLF] two-phase culling: phase 2 brought back {} objects the stale HZB had rejected, and drew depth for {} of them",
					draws.cullRescuedByPhaseTwo, draws.cullDrawnPhaseTwo);
			if (draws.hzbSampled)
				logger::info("[DCLF] HZB: {} footprints sampled, {} came back all-near (~0), {} all-far (~1)",
					draws.hzbSampled, draws.hzbNear, draws.hzbFar);
			if (draws.hzbSample.valid) {
				const auto& sample = draws.hzbSample;
				logger::info("[DCLF] HZB rejection sample: farthest {:.6f} against nearest {:.6f}, uv ({:.4f} {:.4f})-({:.4f} {:.4f}), mip {}, engine {}",
					sample.farthest, sample.nearestZ, sample.uvMin[0], sample.uvMin[1], sample.uvMax[0], sample.uvMax[1],
					sample.mip, sample.nativeVisible ? "kept it" : "culled it");
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
	// What the GPU spent on each segment, measured by the graph itself; the menu shows the latest window.
	if ((frame % kReportInterval) == 0)
		RenderGraphRuntime::Get().ReportGpuTimings(kReportInterval, DCLF::SwitchEnabled(DCLF::Switch::Stats));
	if ((frame % kReportInterval) == 0 && !DCLF::SwitchEnabled(DCLF::Switch::Stats)) {
		timing = {};
		store.ResetTimes();
	}
	if (DCLF::SwitchEnabled(DCLF::Switch::Stats) && (frame % kReportInterval) == 0) {
		const auto& stats = store.GetStats();
		std::string reasons;
		for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
			if (stats.ineligible[i])
				reasons += fmt::format(" {}={}({} drawn)", DCLF::kIneligibleNames[i], stats.ineligible[i], stats.ineligibleDrawn[i]);
		}
		const double frames = std::max(1u, timing.frames);
		// The per-part breakdown appears only under CS_DCLF_PROFILE=1, because that is the only time it is
		// measured. Timing from inside the loop perturbs it, so compare a profiled run's parts against an
		// unprofiled run's total rather than treating the parts as free.
		std::string parts;
		if (DCLF::SceneStore::ProfileEnabled()) {
			for (std::size_t i = 0; i < stats.partMs.size(); ++i)
				parts += fmt::format("{}{} {:.3f}", parts.empty() ? "; by part: " : ", ", DCLF::kBuildPartNames[i], stats.partMs[i] / frames);
			for (std::size_t i = 0; i < stats.accumulatePartMs.size(); ++i)
				parts += fmt::format("{}{} {:.3f}", i ? ", " : "; accumulate by part: ", DCLF::kBuildPartNames[i], stats.accumulatePartMs[i] / frames);
		}
		const auto& shaders = DCLF::ShaderPrograms::Get().GetStats();
		logger::info("[DCLF] SPIR-V programs: {} requested, {} ready ({} stages from cache), {} failed; shadow (Utility): {} requested, {} ready, {} failed",
			shaders.requested, shaders.ready, shaders.fromCache, shaders.failed, shaders.shadowRequested, shaders.shadowReady, shaders.shadowFailed);
		const auto& indirect = DCLF::DrawPipelines::Get().GetStats();
		logger::info("[DCLF] indirect pipelines: {} requested, {} in the set, {} failed; target changes {}; shadow: {} requested, {} in the set, {} failed; "
					 "set versions published {} (waited {} frames), shadow {} (waited {})",
			indirect.requested, indirect.ready, indirect.failed, indirect.targetChanges, indirect.shadowRequested, indirect.shadowReady, indirect.shadowFailed,
			indirect.setPublishes, indirect.setWaits, indirect.shadowSetPublishes, indirect.shadowSetWaits);
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		std::string skipped;
		for (std::size_t i = 0; i < draws.skipped.size(); ++i) {
			if (draws.skipped[i])
				skipped += fmt::format(" {}={}", DCLF::kSkipNames[i], draws.skipped[i]);
		}
		const auto& textures = DCLF::GpuTextures::Get().GetStats();
		logger::info("[DCLF] indirect draws (last frame): {} candidates built from {} binding records, skipped:{} (missing t{} t{} t{} t{}, VS b{:04X} PS b{:04X}); {:.1f} MB uploaded, {:.3f} ms CPU; {} epochs, {} not ready; textures {} live in {} registry slots, {} cleanup pending, rejected {}/{}/{}, {} samplers",
			draws.drawn, draws.records, skipped, draws.missingTextures[0], draws.missingTextures[1], draws.missingTextures[2], draws.missingTextures[3], draws.missingVertexConstants,
			draws.missingPixelConstants, draws.uploadBytes / 1048576.0,
			draws.cpuMs, draws.epochs, draws.notReady, textures.cached, textures.registrySlots, textures.cleanupPending,
			textures.rejected[1], textures.rejected[2], textures.rejected[3], textures.samplers);
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
		// Under CS_DCLF_ASYNC the build's parts are measured on the worker when it built the payload, and
		// "graph execute" is then everything the render thread spent on the epoch.
		logger::info("[DCLF] indirect epoch CPU by part{}: {}", DCLF::AsyncModeSetting() != DCLF::AsyncMode::Off ? " (build parts on the worker when it built)" : "", epochParts);
		if (draws.commitEpochs) {
			static constexpr std::array<const char*, 7> kCommitParts{ "join", "lookups", "frame textures and patches", "frame blocks",
				"payload uploads", "drawn set", "rest" };
			std::string commitParts;
			for (std::size_t i = 0; i < kCommitParts.size(); ++i)
				commitParts += fmt::format("{}{} {:.1f}", commitParts.empty() ? "" : ", ", kCommitParts[i], draws.commitUs[i] / draws.commitEpochs);
			logger::info("[DCLF] main epoch commit on the render thread, us per epoch over {} epochs: {}", draws.commitEpochs, commitParts);
			DCLF::IndirectDraws::Get().ResetCommitTimings();
		}
		std::istringstream reportLines((DCLF::AsyncModeSetting() != DCLF::AsyncMode::Off ? DCLF::IndirectDraws::Get().AsyncReport() : std::string()) +
									   DCLF::SceneStore::Get().SceneReport());
		for (std::string line; std::getline(reportLines, line);)
			logger::info("{}", line);
		if (DCLF::ActiveToggles().shadows) {
			const auto& shadow = DCLF::IndirectDraws::Get().GetShadowStats();
			std::string notReadyReasons;
			for (std::size_t r = 0; r < shadow.notReadyReasons.size(); ++r) {
				if (shadow.notReadyReasons[r])
					notReadyReasons += fmt::format(" {}={}", DCLF::kShadowNotReadyNames[r], shadow.notReadyReasons[r]);
			}
			logger::info("[DCLF] shadow views: {} offered, {} drawn in {} epochs, {} not ready ({}), {} focus views left native; last mode {} inputs ({} without a pipeline, {} without a texture), {} records; CPU {:.3f} ms per frame ({:.3f} capturing, {:.3f} preparing, {:.3f} inputs, {:.3f} blocks, {:.3f} graph, {:.3f} claiming)",
				shadow.views, shadow.viewsDrawn, shadow.epochs, shadow.notReady, notReadyReasons.empty() ? "-" : notReadyReasons.c_str() + 1, shadow.focusSkipped, shadow.inputs, shadow.skippedPipeline,
				shadow.skippedTexture, shadow.records, (shadow.cpuMs + shadow.captureMs) / frames, shadow.captureMs / frames, shadow.prepareMs / frames, shadow.inputsMs / frames, shadow.blocksMs / frames,
				shadow.executeMs / frames, shadow.claimMs / frames);
			if (shadow.cullTested)
				logger::info("[DCLF] shadow culling (view {} mode {:#x}, sampled): {} tested, {} drawn, {} rejected by the frustum",
					shadow.cullSampledView, shadow.cullSampledMode, shadow.cullTested, shadow.cullDrawn, shadow.cullRejected);
			if (shadow.sunEntryChecks)
				logger::info("[DCLF] sun entry on the GPU: {} inputs checked against the CPU's verdict (sampled frames), {} differ{}", shadow.sunEntryChecks,
					shadow.sunEntryMismatches, shadow.sunEntryMismatches ? " <- DIFFER" : " <- OK");
			if (DCLF::PassCapture::ShadowWithholdingEnabled()) {
				const auto& captured = DCLF::PassCapture::Get().GetStats();
				logger::info("[DCLF] shadow ownership: withheld plain {} / clamped {} / paraboloid {} passes, {} volumetric-only passes and {} of hints 11, 7 and 3 (last frame); claimed {} / {} / {} casters; {} face regions uploaded; {} views not ready under ownership{}",
					captured.shadowWithheld[0], captured.shadowWithheld[1], captured.shadowWithheld[2], captured.volumetricWithheld, captured.directWithheld, shadow.claimed[0], shadow.claimed[1],
					shadow.claimed[2], shadow.faceUploads, shadow.notReady, shadow.notReady ? " <- HOLES" : "");
			}
			if (shadow.skyDrawn || shadow.skyNotReady || skyNativeFrames)
				logger::info("[DCLF] Skylighting occlusion: DCLF drew {} maps ({} occluders, last), {} it could not draw, {} left to the engine; render thread {:.3f} ms per map",
					shadow.skyDrawn, shadow.skyInputs, shadow.skyNotReady, skyNativeFrames, shadow.skyDrawn ? shadow.skyMs / shadow.skyDrawn : 0.0);
			skyNativeFrames = 0;
			DCLF::IndirectDraws::Get().ResetShadowStats();
		}
		if (stats.projectedUV || stats.landBlend)
			logger::info("[DCLF] projected UV / terrain (last frame): {} projected candidates, {} terrain candidates, projected textures {}",
				stats.projectedUV, stats.landBlend, store.GetProjectedTextures().valid ? "captured" : "not seen yet");
		if (const auto [fading, fadingFrames] = store.TakeFadingDrawn(); fading)
			logger::info("[DCLF] fading: {} screen-door fading objects drawn by DCLF over {} frames", fading, fadingFrames);
		if (stats.skinned || draws.boneRows)
			logger::info("[DCLF] skinned (last frame): {} candidates, {} palette rows in the tables, {} rows uploaded by the epoch", stats.skinned, stats.boneRows, draws.boneRows);
		if (stats.decals[0] || stats.decals[1] || draws.decalsDrawn)
			logger::info("[DCLF] decals (last frame): {} candidates ({} in the opaque group, {} in the blended group), {} submitted to the second pass, {} of {} tested were culled",
				stats.decals[0] + stats.decals[1], stats.decals[0], stats.decals[1], draws.decalsDrawn, draws.decalsCulled, draws.decalsTested);
		if (draws.sunTested || draws.sunCpuTested)
			logger::info("[DCLF] sun on the GPU (sampled frame): {} synthetic draws tested, {} missed every cascade; the CPU over the same inputs: {} tested, {} missed{}",
				draws.sunTested, draws.sunMissed, draws.sunCpuTested, draws.sunCpuMissed,
				(draws.sunTested == draws.sunCpuTested && draws.sunMissed == draws.sunCpuMissed) ? " <- OK" : " <- DIFFERS");
		if (draws.residentInputs || draws.residentResyncs)
			logger::info("[DCLF] persistent draws (last frame, colour): {} kept inputs, {} sequences, {} pairs, {} not drawable; since the start {} region "
						 "versions uploaded, {} resyncs; parity {} entries checked, {} differ, {} missing{}",
				draws.residentInputs, draws.residentDraws, draws.residentPairs, draws.residentUndrawable, draws.residentVersions, draws.residentResyncs,
				draws.residentParityChecks, draws.residentParityMismatches, draws.residentMissing,
				draws.residentParityChecks ? (draws.residentParityMismatches || draws.residentMissing ? " <- RESIDENT DRAW PARITY" : " <- OK") : "");
		if (draws.fadeTested)
			logger::info("[DCLF] fade on the GPU (sampled frame): {} resident draws under a fade root in view, {} dropped past their fade-out distance",
				draws.fadeTested, draws.fadeHidden);
		logger::info("[DCLF] skip (last frame): {} of {} native passes left to the indirect draws ({} in the depth pass, {} in the opaque pass)",
			skipStats.skipped, skipStats.offered, skipStats.skippedInDepth, skipStats.skippedInOpaque);
		if (!skipSamples.empty()) {
			std::string names;
			for (const auto& name : skipSamples)
				names += fmt::format("{}'{}'", names.empty() ? "" : ", ", name);
			logger::info("[DCLF] skip: passes left to the indirect draws include {}", names);
		}
		if (skipStats.notInTables)
			logger::warn("[DCLF] skip (last frame): {} native passes were kept because their geometry left the tables", skipStats.notInTables);
		if (skipStats.undrawable)
			logger::info("[DCLF] skip (last frame): {} native passes were kept because DCLF could not draw their object this frame", skipStats.undrawable);
		if (draws.shortBuffers)
			logger::warn("[DCLF] {} draws of the last epoch reach past their vertex or index buffer slice", draws.shortBuffers);
		const auto& gpu = DCLF::GpuResources::Get().GetStats();
		logger::info("[DCLF] game buffers for the render graph: {} stable, {} rejected; {} resolved so far in {:.1f} ms (slowest {:.3f} ms)", gpu.cached, gpu.rejected,
			gpu.resolvedTotal, gpu.resolveMsTotal, gpu.resolveMsMax);
		// Two halves, because they mean different things: outside kRuntimePassBits the derivation is meant
		// to be exact and any difference is a defect; inside it the derivation is guessing at what
		// GetRenderPasses computes per frame, and that number is what decides whether GetRenderPasses can
		// be skipped outright rather than merely withheld from the batch renderer.
		std::string bitBreakdown;
		// Only when CS_DCLF_DERIVE_PROBE measured it. Printed unconditionally this line read
		// "0 objects compared, 0 differ" with the probe off, which scans as a passing check rather
		// than as one that never ran.
		if (DCLF::SwitchEnabled(DCLF::Switch::DeriveProbe)) {
			for (std::uint32_t bit = 0; bit < 32; ++bit) {
				if (!stats.derivationBitCounts[bit])
					continue;
				bitBreakdown += std::format("{} bit {}{}={}", bitBreakdown.empty() ? "" : ",", bit,
					((1u << bit) & DCLF::kRuntimePassBits) ? "*" : "", stats.derivationBitCounts[bit]);
			}
			logger::info("[DCLF] derivation (last frame): {} objects compared, {} would stay native; property bits: {} differ ({:08X}); runtime bits: {} differ ({:08X}); per bit (* = runtime):{}",
				stats.derivationChecked, stats.derivationNative, stats.derivationDiffers, stats.derivationBits,
				stats.derivationRuntimeDiffers, stats.derivationRuntimeBits, bitBreakdown.empty() ? std::string(" none") : bitBreakdown);
		}
		const auto& capture = DCLF::PassCapture::Get().GetStats();
		logger::info("[DCLF] pass capture: {} registrations from {} threads ({} overflowed); against the accumulator: {} compared, {} missing, {} extra, {} technique differs, {} subPass differs{}",
			capture.captured, capture.threads, capture.overflowed, capture.compared, capture.missing, capture.extra,
			capture.techniqueDiffers, capture.subPassDiffers,
			(capture.missing || capture.techniqueDiffers || capture.subPassDiffers) ? "" : " <- OK");
		if (DCLF::ActiveToggles().ownership)
			logger::info("[DCLF] static ownership: {} passes withheld from the batch renderer, {} objects claimed, {} claimed but not drawn{}",
				capture.withheld, capture.claimed, capture.holes, capture.holes ? " <- HOLES" : "");
		if (DCLF::ActiveToggles().ownership)
			logger::info("[DCLF] claim churn: +{} -{} ({} of the drops still had an engine pass, so the native loop takes them back)",
				capture.claimsAdded, capture.claimsDropped, capture.droppedAfterCull);
		logger::info("[DCLF] derived cache (last frame): {} served, {} recomputed and compared, {} differ{}; slots alive {} geometries / {} pipelines / {} materials, {} swept, {} geometries refreshed in place, {} slot violations{}",
			stats.derivedHits, stats.derivedChecked, stats.derivedDiffers, stats.derivedDiffers ? " <- STALE" : "",
			stats.geometriesAlive, stats.pipelinesAlive, stats.materialsAlive, stats.slotsSwept, stats.geometriesRefreshed,
			stats.slotViolations, stats.slotViolations ? " <- SLOT VIOLATION" : "");
		if (stats.classifyHits || stats.classifyChecked)
			logger::info("[DCLF] classification cache (last frame): {} served from the cache, {} recomputed and compared, {} differ{}; RTTI casts walked {}",
				stats.classifyHits, stats.classifyChecked, stats.classifyDiffers, stats.classifyDiffers ? " <- STALE" : "", stats.castResolved);
		// The Stage 4c gate. A pipeline's per-frame lighting template must come from an object the engine
		// itself kept; anything else hands a culled object's scene light list to the visible objects drawn
		// on that pipeline, which is the blown-out interior lighting defect.
		{
			static constexpr const char* kTechniqueNames[20] = { "none", "envmap", "glowmap", "parallax", "facegen",
				"facegenRGBTint", "hair", "parallaxOcc", "MTLand", "LODLand", "snow", "multilayerParallax", "treeAnim",
				"LODObjects", "multiIndexSparkle", "LODObjectHD", "eye", "cloud", "LODLandNoise", "MTLandLODBlend" };
			std::string techniques;
			for (std::size_t t = 0; t < stats.techniqueRejects.size(); ++t) {
				if (!stats.techniqueRejects[t])
					continue;
				const char* name = t == 63 ? "refraction" : (t < 20 ? kTechniqueNames[t] : "?");
				techniques += fmt::format("{}{}({})={}", techniques.empty() ? "" : " ", name, t, stats.techniqueRejects[t]);
			}
			if (!techniques.empty())
				logger::info("[DCLF] left native by technique: {}", techniques);
		}
		if (!stats.propertyRejects.empty()) {
			std::vector<std::pair<const RE::NiRTTI*, std::uint32_t>> sorted(stats.propertyRejects.begin(), stats.propertyRejects.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			std::string byProperty;
			for (const auto& [rtti, count] : sorted)
				byProperty += fmt::format(" {}={}", rtti && rtti->name ? rtti->name : "?", count);
			logger::info("[DCLF] left native by property type:{}; {} alpha blended, {} opaque ({} alpha tested)",
				byProperty, stats.rejectedBlended, stats.rejectedOpaque, stats.rejectedOpaqueAlphaTest);
		}
		logger::info("[DCLF] pipeline templates: {} of {} drawn only by culled candidates, {} taken over this frame; {} visible objects on a culled template{}",
			stats.pipelinesCulledOnly, stats.pipelines, stats.templateUpgrades, stats.templateDefects,
			stats.templateDefects ? " <- CULLED TEMPLATE" : "");
		// The material cache and its standing alarm. materialCacheStale must be 0: it is the count of
		// entries that were re-evaluated live and disagreed with what the cache would have served.
		logger::info("[DCLF] materials (last frame): {} evaluated; {} written ({} re-evaluated, {} dropped), {} frame samples; slots {} alive (+{} retired on last reference); validated {}, stale {}{}",
			stats.materialsEvaluated, stats.materialWrites, stats.materialsRewritten, stats.materialsDropped,
			stats.frameMaterialSamples,
			stats.materialCacheEntries, stats.materialCacheEvicted, stats.materialsValidated, stats.materialCacheStale,
			stats.materialCacheStale ? " <- STALE MATERIAL" : "");
		if (stats.materialCacheStale)
			logger::warn("[DCLF] material cache staleness is in: {}{}{}{}{}{}",
				(stats.materialDiffMask & 1) ? "vs " : "", (stats.materialDiffMask & 2) ? "ps " : "",
				(stats.materialDiffMask & 4) ? "textures " : "", (stats.materialDiffMask & 8) ? "address " : "",
				(stats.materialDiffMask & 16) ? "filter " : "", (stats.materialDiffMask & 32) ? "written" : "");
		logger::info("[DCLF] tracked {} under {} category nodes: {} objects ({} the engine also kept, {} of them with the sun's shadow mask; {} with derived descriptors), {} geometries, {} pipelines, {} materials; left native:{}; events +{} -{}, validation drops {}; CPU per frame: events {:.3f} ms, tables {:.3f} ms (scene {:.3f}, max {:.3f}; accumulate {:.3f}, max {:.3f}){}",
			stats.tracked, stats.categoryNodes, stats.objects, stats.nativeVisible, stats.nativeShadowMasked, stats.derivedDescriptors, stats.geometries, stats.pipelines, stats.materials, reasons,
			stats.attachedEvents, stats.detachedEvents, stats.validationDrops, timing.eventsMs / frames,
			(timing.sceneMs + timing.buildMs) / frames, timing.sceneMs / frames, timing.sceneMaxMs, timing.buildMs / frames, timing.buildMaxMs,
			parts);
		{
			// The "scene tables" zone by sub-zone (ScenePart). The four event parts also count Present's ProcessEvents,
			// which is outside the zone, so "other" (the zone less its parts: the rescan after a load, the claims'
			// selection) goes negative by about that call's cost.
			const double tablesFrames = std::max(1u, timing.sceneTablesFrames);
			std::string sceneParts;
			double covered = 0.0;
			for (std::size_t i = 0; i < stats.scenePartMs.size(); ++i) {
				if (i != static_cast<std::size_t>(DCLF::ScenePart::PlacementJoin))
					covered += stats.scenePartMs[i];
				if (stats.scenePartMs[i] > 0.0)
					sceneParts += fmt::format("{}{} {:.3f} (max {:.2f})", sceneParts.empty() ? "" : ", ", DCLF::kScenePartNames[i], stats.scenePartMs[i] / tablesFrames,
						stats.scenePartMaxMs[i]);
			}
			logger::info("[DCLF] scene tables CPU per frame: {:.3f} ms (max {:.3f}) over {} frames; by part: {}; other {:.3f}",
				timing.sceneTablesMs / tablesFrames, timing.sceneTablesMaxMs, timing.sceneTablesFrames, sceneParts, (timing.sceneTablesMs - covered) / tablesFrames);
			{
				static constexpr std::array<const char*, 8> kTraitNames{ "face", "actor", "switch", "skin", "animated shading", "moves", "root moves", "?" };
				std::string traits;
				for (std::size_t i = 0; i < stats.lightByTrait.size(); ++i)
					if (stats.lightByTrait[i])
						traits += fmt::format("{}{} {:.0f}", traits.empty() ? "" : ", ", kTraitNames[i], stats.lightByTrait[i] / tablesFrames);
				logger::info("[DCLF] scene tables' light path per frame: kept by trait: {}; placed {:.0f} ({:.0f} changed), not placed for want of a move event {:.0f} "
							 "({:.0f} move events{}), kept skins {:.0f} ({:.0f} changed their palette)",
					traits.empty() ? "-" : traits, stats.lightPlaced / tablesFrames, stats.lightPlacedChanged / tablesFrames, stats.lightGated / tablesFrames,
					stats.moveEvents / tablesFrames, DCLF::SceneStore::MoveEventsLive() ? "" : ", not installed", stats.lightSkins / tablesFrames,
					stats.lightSkinsChanged / tablesFrames);
				std::string placedBy;
				for (std::size_t i = 0; i < stats.lightPlacedBy.size(); ++i)
					if (stats.lightPlacedBy[i])
						placedBy += fmt::format("{}{} {:.0f} ({:.0f} changed)", placedBy.empty() ? "" : ", ", DCLF::SceneStore::kMoveReasonNames[i],
							stats.lightPlacedBy[i] / tablesFrames, stats.lightChangedBy[i] / tablesFrames);
				if (!placedBy.empty())
					logger::info("[DCLF] scene tables' light path placed per frame, by why: {}; the per-frame set looked up again on {} of {} frames", placedBy,
						stats.perFrameRelookups, timing.sceneTablesFrames);
				if (stats.verdictsChecked || stats.verdictsSkipped)
					logger::info("[DCLF] actor frame verdicts per frame: {:.0f} taken again, {:.0f} left for want of a hidden event ({:.0f} hidden events{}); {} changed with no event{}{}",
						stats.verdictsChecked / tablesFrames, stats.verdictsSkipped / tablesFrames, stats.hiddenEvents / tablesFrames,
						DCLF::SceneStore::HiddenEventsLive() ? "" : ", not installed", stats.verdictsMissed, stats.verdictsMissed ? " <- MISSED; first: " : " <- OK",
						stats.firstVerdictMissed);
				if (const auto line = store.PlacementReport(); !line.empty())
					logger::info("{}", line);
			}
			if (DCLF::SceneStore::ProfileEnabled()) {
				std::string kinds;
				for (std::size_t i = 0; i < stats.evaluateKindMs.size(); ++i)
					kinds += fmt::format("{}{} {:.0f} entries {:.3f} ms", kinds.empty() ? "" : ", ", DCLF::kEvaluateKindNames[i],
						stats.evaluateKindCount[i] / tablesFrames, stats.evaluateKindMs[i] / tablesFrames);
				logger::info("[DCLF] scene tables' first evaluation round per frame, by entry: {}", kinds);
			}
		}
		{
			std::string rejects;
			for (std::size_t r = 1; r < stats.shadowRejects.size(); ++r) {
				if (stats.shadowRejects[r])
					rejects += fmt::format(" {}={}", DCLF::ShadowRejectName(static_cast<DCLF::ShadowReject>(r)), stats.shadowRejects[r]);
			}
			logger::info("[DCLF] shadow casters (last frame): {} of {} records would be drawn into a shadow map; not casters:{}; {} views this frame",
				stats.shadowCasters, stats.objects, rejects.empty() ? " none" : rejects, DCLF::ShadowViews::Get().All().size());
		}
		if (stats.accumulatedWithoutRecord)
			logger::warn("[DCLF] two-phase frame: {} objects the engine accumulated had no record from the scene phase, so they stayed native",
				stats.accumulatedWithoutRecord);
		timing = {};
		store.ResetTimes();
	}
}
