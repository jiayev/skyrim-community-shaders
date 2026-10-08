#pragma once

// The counters IndirectDraws keeps for the report (DrawcallLimitFix/Report.cpp), apart from its interface.

#include <array>
#include <cstdint>
#include <string>

#include "Features/DrawcallLimitFix/Scene/Records.h"

namespace DCLF
{
	/** @brief Why an object candidate was not drawn by an epoch (IndirectDrawStats::skipped). */
	enum class DrawSkip : std::uint32_t
	{
		Pipeline,  // not in the pipeline set yet
		Geometry,  // no stable vertex / index buffer
		Texture,   // a texture the shaders read is not resolved
		Sampler,
		Constants,  // a constant buffer the shaders read is not available
		Capacity,
		NotInSet,            // bound, but not in the DCLF set this frame (SceneSet.h): the engine draws it
		CandidateOnly,       // kept for the GPU culling to reject, but not drawable, so it has no bindings
		Count
	};

	/** @brief The main epochs' counters (IndirectDraws::GetStats), per report interval unless noted. */
	struct IndirectDrawStats
	{
		std::uint32_t epochs = 0;
		std::uint32_t drawn = 0;  // last epoch
		std::array<std::uint32_t, static_cast<std::size_t>(DrawSkip::Count)> skipped{};
		std::array<std::uint32_t, 4> missingTextures{};  // registers of the last unresolved textures (diagnostics)
		std::uint32_t missingVertexConstants = 0;       // constant buffer registers without an address (bits)
		std::uint32_t missingPixelConstants = 0;
		std::uint32_t records = 0;                       // binding records built, last epoch
		std::uint64_t uploadBytes = 0;                   // last epoch
		double cpuMs = 0.0;                              // last epoch, assembly and epoch
		// Where that time goes
		// Parts 0-2 are per binding record, not per draw
		std::array<double, 8> partMs{};
		// The render thread's part of the main epochs (the commit and what surrounds it inside the epoch),
		// summed over commitEpochs since the last report: join, lookups, frame textures and patches, frame
		// blocks, payload uploads, the drawn set, the rest (shape, latch, stats).
		std::array<double, 7> commitUs{};
		std::uint32_t commitEpochs = 0;
		std::uint32_t notReady = 0;            // epochs skipped (mirrors, targets or pipelines not ready)
		std::uint32_t shortBuffers = 0;        // draws whose vertex or index slice does not cover them
		std::uint64_t drawsWaiting = 0;        // draws past the sequences that waited for their growth (Growths), since the start
		// Draws skipped because a material's textures were not in the lookups yet (Lookups.h): resolved
		// by this epoch's commit, they land next frame. Steady state 0.
		std::uint32_t deferredTextures = 0;
		// Frame textures (t16 and up) a record reads that the pass had not bound at the commit, last epoch.
		std::uint32_t frameTexturesMissing = 0;
		std::array<std::uint64_t, 2> frameTexturesMissingRegisters{};  // which registers, as a bit set
		std::uint32_t cullDrawn = 0;     // sequences BuildDraws wrote, last sampled epoch
		std::uint32_t cullRejected = 0;  // draws its culling rejected (CS_DCLF_CULL)
		std::uint32_t cullTested = 0;    // draws it tested at all: 0 means the culling did not run
		std::uint32_t cullOccluded = 0;         // rejected by the HZB rather than by the frustum
		// The second phase, which re-tests what the first rejected against the rebuilt HZB.
		std::uint32_t cullDrawnPhaseTwo = 0;      // depth draws it added
		std::uint32_t cullRescuedByPhaseTwo = 0;  // objects it brought back
		// Decals (CS_DCLF_DECALS): submitted to the second colour pass by the CPU, and what its
		// single-phase culling did with them (GPU counters, sampled like the others).
		std::uint32_t decalsDrawn = 0;
		std::uint32_t decalsCulled = 0;
		std::uint32_t decalsTested = 0;
		// The sun's bits on the GPU (kObjectSunTest), sampled with the culling counters: the colour dispatch's
		// flagged inputs and how many missed every cascade, against the CPU's count over the same inputs and
		// planes (the same test in C++).
		std::uint32_t sunTested = 0, sunMissed = 0, sunCpuTested = 0, sunCpuMissed = 0;
		// The fade test (kObjectFadeTest), sampled likewise: resident inputs the depth segment's first phase found in the
		// frustum under a fade root, and those it dropped past their fade-out distance.
		std::uint32_t fadeTested = 0, fadeHidden = 0;
		std::uint32_t fadeRoots = 0;  // the fade roots listed
		// Persistent resident draws (drawcall-limit-fix.md): the colour region's inputs, sequences, pairs at stable record
		// slots and entries it cannot draw (last epoch); new region versions, resyncs, and CS_DCLF_RESIDENT_DRAW_PARITY's
		// counts, since the start.
		std::uint32_t residentInputs = 0, residentDraws = 0, residentPairs = 0, residentUndrawable = 0;
		std::uint64_t residentVersions = 0, residentResyncs = 0, residentParityChecks = 0, residentParityMismatches = 0, residentMissing = 0;
		std::array<std::uint64_t, 5> residentResyncBy{};  // since the start, by reason (MainBuild::UpdateRegionEntries)
		std::uint64_t residentDecalRetakes = 0;           // since the start: decal group counts moved (decals re-taken, no resync)
		std::uint64_t residentPairsChecked = 0, residentPairsStale = 0;
		std::array<std::uint64_t, 2> residentLastVersion{};
		std::uint32_t extraRows = 0;  // extras rows uploaded by the last epoch
		// What the HZB held under the tested objects: all-near or all-far means the build is wrong.
		std::uint32_t hzbNear = 0, hzbFar = 0, hzbSampled = 0;
		// One rejection in full, so an implausible count can be read instead of guessed at.
		struct HzbSample
		{
			bool valid = false;
			float farthest = 0.0f, nearestZ = 0.0f;
			float uvMin[2]{}, uvMax[2]{};
			std::uint32_t mip = 0;
		} hzbSample;
		std::uint32_t buildParityChecks = 0;   // CS_DCLF_BUILD_PARITY
		std::uint32_t buildParityMismatches = 0;
		// CS_DCLF_BINDLESS_PARITY: the per-object record the DCLF_BINDLESS builds read, checked against
		// the constant group the non-bindless path packs for the same object. Counted per variable
		// component, because that is the granularity at which a layout mistake shows.
		std::uint32_t bindlessParityChecks = 0;
		std::uint32_t bindlessParityMismatches = 0;
		// Pairs whose pipeline's constant tables were not the ones their material row was packed with (they stayed native).
		std::uint32_t rowTableConflicts = 0;
		// The scene lane's builds: per job kind (colour, Z-prepass, shadow), per report interval.
		struct Async
		{
			// Per epoch, since the last report (step 6e E3b, S1): committed the installed publication's payload (built ahead), or built
			// at the epoch (CS_DCLF_BINDLESS_PARITY); and, under CS_DCLF_REVISION_PARITY, payloads for other inputs than the frame's.
			std::uint32_t used = 0, builtInline = 0, stale = 0;
		};
		std::array<Async, 3> async{};
	};

	/** @brief Why a shadow view was offered to the epoch and not drawn (ShadowDrawStats::notReadyReasons). */
	enum class ShadowViewNotReady : std::uint32_t
	{
		Setup,      // no render graph, no Utility shader, or the shadow resources could not be created
		Pipelines,  // no shadow pipeline in the set yet
		Tables,     // the scene phase has not built this frame's tables, or the view is unknown
		Depth,      // the engine's shadow map could not be imported, or the view draws into an unknown target
		Epoch,      // the epoch itself failed
		Placement,  // the view drew where no placement is (target, slice, viewport), under another state, or twice: a defect
		Count
	};

	/** @brief The shadow views' and Skylighting occlusion map's counters (IndirectDraws::GetShadowStats). */
	struct ShadowDrawStats
	{
		std::uint32_t views = 0;           // views offered (captured by the hook), per report interval
		std::uint64_t sunEntryChecks = 0, sunEntryMismatches = 0;  // CS_DCLF_PERSISTENT_PARITY: the GPU sun-entry test on the latch against the CPU verdict
		std::uint32_t viewsDrawn = 0;      // views drawn by an epoch
		std::uint32_t retainedViews = 0;   // view slots kept in an epoch's shape with no work (a view that comes and goes)
		std::uint32_t epochs = 0;          // shadow epochs run (one per frame with views)
		std::uint32_t notReady = 0;        // views skipped: resources, pipelines or the depth import not ready
		std::uint32_t notReadyWithheld = 0;  // of those, views whose casters the engine withheld this frame: holes
		std::array<std::uint32_t, static_cast<std::size_t>(ShadowViewNotReady::Count)> notReadyReasons{};
		std::uint32_t focusSkipped = 0;    // focus views, left native
		std::uint32_t uncovered = 0;       // views past DCLF's view slots (their growth outstanding) or of a mode unknown at Rebuild, left native
		std::uint32_t faceUploads = 0;        // face position regions uploaded (a head's snapshot changed), per report interval
		// The culling's GPU counters of one sampled view (the count buffer read back a few frames after
		// its epoch): what says the frustum test is doing something, and against which view.
		std::uint32_t cullDrawn = 0, cullRejected = 0, cullTested = 0;
		// Before the culling: the sampled view's inputs of its caster class, and those dropped outside the sun's entry processes, at
		// most the minimum radius, under a stood-in root fading.
		std::uint32_t cullClass = 0, cullSunEntryOut = 0, cullMinRadius = 0, cullStoodInFading = 0;
		std::uint32_t cullSampledView = ~0u, cullSampledMode = 0;
		// The set's casters each render mode's inputs held at the last epoch, and set members a mode's build could not draw (a
		// pipeline or a texture its readiness did not cover: a defect, the engine having withheld them), per report interval.
		std::array<std::uint32_t, 3> casters{};
		std::uint64_t setWaiting = 0;
		std::string setWaitingFirst;
		std::uint32_t inputs = 0;          // casters submitted, last view
		std::uint32_t skippedPipeline = 0; // casters without a ready shadow pipeline, last view
		std::uint32_t skippedTexture = 0;  // alpha-tested casters whose diffuse could not be resolved, last view
		std::uint32_t deferredTextures = 0;   // of those, ones whose diffuse the lookups had not resolved yet (next frame)
		std::uint32_t deferredPipelines = 0;  // casters whose shadow pipeline the lookups had no entry for yet
		std::uint32_t records = 0;         // material rows, last frame
		std::uint32_t waitingRows = 0;     // materials past the rows' table, waiting for it to grow, last frame
		double cpuMs = 0.0;                // per report interval, all views
		double captureMs = 0.0;            // per report interval, the hooks' captures
		double prepareMs = 0.0;            // of which the once-per-frame preparation
		double inputsMs = 0.0;             // of which building (per mode) and uploading the inputs
		double blocksMs = 0.0;             // of which the view's constant blocks and their uploads
		double executeMs = 0.0;            // of which the graph's own execution (compile, prepare, record)
		// The occlusion maps (ExecuteOcclusion), per occlusion view and report interval.
		std::array<std::uint32_t, kOcclusionViews> occlusionDrawn{};     // maps DCLF drew
		std::array<std::uint32_t, kOcclusionViews> occlusionNotReady{};  // draws asked for that it could not make
		std::array<std::uint32_t, kOcclusionViews> occlusionInputs{};    // occluders, last map
		double occlusionMs = 0.0;                                          // render thread, their epochs
		std::uint32_t occlusionEpochs = 0;
	};

	inline constexpr std::array<const char*, 8> kEpochPartNames{
		"textures/samplers", "constant groups", "record push", "per-draw tail", "per-draw prologue",
		"uploads", "epoch prologue", "graph execute"
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(ShadowViewNotReady::Count)> kShadowNotReadyNames{
		"setup", "pipelines", "tables", "depth", "epoch", "placement"
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(DrawSkip::Count)> kSkipNames{ "pipeline", "geometry", "texture", "sampler",
		"constants", "capacity", "not-in-set", "candidate-only" };
}
