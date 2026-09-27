#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "RenderGraph/RenderGraphRuntime.h"

namespace RE
{
	class BSGeometry;
	class NiPoint3;
}

namespace DCLF
{
	/**
	 * @brief Phase 2: DCLF's objects drawn by the render graph with one indirect command stream, into
	 * off-screen copies of the main pass's targets (the frame itself is unchanged).
	 *
	 * Where the main (deferred) pass's opaque batches start, CaptureMainPass records what the pass binds;
	 * before the deferred composite, Execute runs the graph's MainOpaque epoch. Inside it the draw data is assembled on the CPU and uploaded:
	 *   - constant blocks packed with the native shaders' constant tables: PerTechnique per pipeline,
	 *     PerMaterial per (material, pipeline), PerGeometry per object, Light Limit Fix's StrictLightData
	 *     per (room, shadow mask), the permutation per (pipeline, object flags), the alpha-test reference
	 *     per threshold, and every other bound constant buffer from its CPU mirror (ConstantMirror);
	 *   - one DrawBindings record per object (constant buffer addresses, texture and sampler heap indices);
	 *   - one DrawSequence per object.
	 * A graph pass then executes the sequences against the main depth buffer (imported from DXVK, read
	 * only, depth test EQUAL) into eight graph-owned targets with the main pass's formats. An object is
	 * drawn only when everything its pipeline's shaders read can be supplied.
	 *
	 * Render thread only.
	 */
	class IndirectDraws
	{
	public:
		enum class Skip : std::uint32_t
		{
			Pipeline,  // not in the pipeline set yet
			Geometry,  // no stable vertex / index buffer
			Texture,   // a texture the shaders read is not resolved
			Sampler,
			Constants,  // a constant buffer the shaders read is not available
			Capacity,
			NotSkippedNatively,  // Z-prepass only: the native loop still draws this object, so it owns its depth
			CandidateOnly,       // kept for the GPU culling to reject, but not drawable, so it has no bindings
			RecordCapacity,      // too many DISTINCT (material, pipeline) pairs, which is not the draw cap
			Count
		};

		void ResetCommitTimings()
		{
			stats.commitUs = {};
			stats.commitEpochs = 0;
		}

		struct Stats
		{
			std::uint32_t epochs = 0;
			std::uint32_t drawn = 0;  // last epoch
			std::array<std::uint32_t, static_cast<std::size_t>(Skip::Count)> skipped{};
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
			// Draws skipped because a material's textures were not in the lookups yet (Lookups.h): resolved
			// by this epoch's commit, they land next frame. Steady state 0.
			std::uint32_t deferredTextures = 0;
			// Frame textures (t16 and up) a record reads that the pass had not bound at the commit, last epoch.
			std::uint32_t frameTexturesMissing = 0;
			std::array<std::uint64_t, 2> frameTexturesMissingRegisters{};  // which registers, as a bit set
			std::uint32_t framePatchedRecords = 0;  // kept records the commits patched for a frame texture that changed (PersistentBindings)
			std::uint32_t cullDrawn = 0;     // sequences BuildDraws wrote, last sampled epoch
			std::uint32_t cullRejected = 0;  // draws its culling rejected (CS_DCLF_CULL)
			std::uint32_t cullTested = 0;    // draws it tested at all: 0 means the culling did not run
			// Against the engine's own culling, which the draw inputs carry per object (kObjectNativeVisible).
			std::uint32_t cullEngineCulled = 0;     // candidates the engine culled and CS_DCLF_CULL_INPUT=native dropped
			std::uint32_t cullFalseNegatives = 0;   // the engine kept it, the culling here rejected it: a defect
			std::uint32_t cullRescued = 0;          // the engine culled it, the culling here kept it
			std::uint32_t cullOccluded = 0;         // rejected by the HZB rather than by the frustum
			std::uint32_t cullOccludedVisible = 0;  // of those, ones the engine's own culling had kept
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
			// Persistent resident draws (drawcall-limit-fix.md): the colour region's inputs, sequences, pairs at stable record
			// slots and entries it cannot draw (last epoch); new region versions, resyncs, and CS_DCLF_RESIDENT_DRAW_PARITY's
			// counts, since the start.
			std::uint32_t residentInputs = 0, residentDraws = 0, residentPairs = 0, residentUndrawable = 0;
			std::uint64_t residentVersions = 0, residentResyncs = 0, residentParityChecks = 0, residentParityMismatches = 0, residentMissing = 0;
			std::array<std::uint64_t, 2> residentLastVersion{};
			std::uint32_t boneRows = 0;  // bone palette rows uploaded by the last epoch (current and previous)
			// What the HZB held under the tested objects: all-near or all-far means the build is wrong.
			std::uint32_t hzbNear = 0, hzbFar = 0, hzbSampled = 0;
			// One rejection in full, so an implausible count can be read instead of guessed at.
			struct HzbSample
			{
				bool valid = false;
				bool nativeVisible = false;
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
			// CS_DCLF_DEDUP_PARITY: a binding record rebuilt per draw against the one its (material,
			// pipeline) pair holds. Zero mismatches is what says the deduplication is sound.
			std::uint32_t recordParityChecks = 0;
			std::uint32_t recordParityMismatches = 0;
			// CS_DCLF_ASYNC: per job kind (colour, Z-prepass, shadow), per report interval.
			struct Async
			{
				std::uint32_t kicked = 0;      // jobs submitted to the worker
				std::uint32_t notKicked = 0;   // epochs whose conditions kept the build inline (no replay, not bindless, ...)
				std::uint32_t used = 0;        // epochs that committed the worker's build
				std::uint32_t builtInline = 0; // epochs that built on the render thread (no job, or the job could not be used)
				std::uint32_t late = 0;        // the job was still running at the join
				std::uint32_t failed = 0;      // the job threw
				std::uint32_t cancelled = 0;
				std::uint32_t stale = 0;       // the job's inputs differed from the epoch's
				std::uint32_t dropped = 0;     // jobs discarded without an epoch to serve
				std::uint32_t leaked = 0;      // jobs still pending at Present (a defect)
				std::uint32_t probeCompared = 0;
				std::uint32_t probeDiffer = 0;
				std::uint32_t eyeMismatches = 0;          // Z-prepass: the predicted eye pair differed from the captured one
				std::uint32_t previousEyeMismatches = 0;  // of which the previous eye alone
			};
			std::array<Async, 3> async{};
		};

		static IndirectDraws& Get();

		bool Enabled() const;

		/** @brief CS_DCLF_HYBRID=1: DCLF draws into the main pass's targets and the native loop skips its objects. */
		static bool Hybrid();

		/**
		 * @brief Whether the epoch drew this geometry in the frame before, which is what the native loop
		 * skips: the epoch runs after the native passes, so its decision is one frame old. An object that
		 * has just become ineligible is missing for one frame; one that has just become eligible is drawn
		 * twice for one frame.
		 */
		bool DrewLastFrame(const RE::BSGeometry* a_geometry, std::uint32_t a_frame) const;

		/**
		 * @brief One frame of the main camera's visibility feedback, decoded: per object in that frame's tables, the
		 * stamp of the last frame its bound was inside the frustum (BuildDraws' depth phase 1). The object was in view
		 * in this frame when its word equals `stamp`. The tag is what the consumer attached when the frame was armed.
		 */
		struct VisibilityFeedbackFrame
		{
			std::uint32_t frame = 0;
			std::uint32_t stamp = 0;
			std::uint32_t objects = 0;
			const std::uint32_t* words = nullptr;
			std::shared_ptr<void> tag;
		};
		/**
		 * @brief Any one thread at a time (DCLF's worker): hands every feedback frame whose copy the GPU has completed
		 * to a_consume, in submission order, and frees its slot. Never waits: frames still in flight are left for a
		 * later call. Returns the number decoded. See dclf-cull-job-elimination.md, "Phase 2".
		 */
		std::uint32_t DrainVisibilityFeedback(const std::function<void(const VisibilityFeedbackFrame&)>& a_consume);
		/** @brief The feedback's counters since the last call: frames armed, dropped (no free slot), abandoned, decoded. */
		struct FeedbackStats
		{
			std::uint64_t armed = 0, dropped = 0, abandoned = 0, decoded = 0;
		};
		FeedbackStats TakeFeedbackStats();

		/**
		 * @brief Publishes what DCLF owns, for the registration hook to withhold.
		 *
		 * The claim is what the colour epoch actually drew, not what DCLF would like to draw: withholding
		 * a pass means the native loop will not draw it either, so claiming something DCLF then fails to
		 * draw (a pipeline still compiling, a texture not resolved) leaves a hole. Drawing it once is the
		 * evidence that it can be drawn again.
		 */
		void PublishClaims();

		/**
		 * @brief What the main pass binds (buffers, views, targets, viewport), for this frame's colour epoch: where its opaque
		 * batches start, the engine's state applied (DrawcallLimitFix::BeforeOpaquePass).
		 */
		void CaptureMainPass();
		/**
		 * @brief CS_DCLF_CAPTURE_POINT_PARITY, at the frame's first lighting draw the engine makes: what is bound there against
		 * the capture, in everything the colour epoch takes from it.
		 */
		void CheckCapturePoint();


		/**
		 * @brief Before the deferred composite: assemble this frame's draws and execute them. Constant buffer
		 * contents are read here, once the main pass has drawn with them (the engine updates its per-frame
		 * buffers while it applies a draw's state, after the first draw's SetupGeometry).
		 */
		void Execute();

		/**
		 * @brief Hybrid path, inside the native depth pass once the world's depth is drawn (AE; at its end
		 * elsewhere): assemble this frame's draws and write their depth into the engine's main depth.
		 *
		 * It runs here, and not with the colour pass, because everything the rest of the frame does with
		 * depth - the native draws' own depth test, the sky, Terrain Blending's blended depth and every
		 * effect that reads it - is derived from the depth buffer at this point and has to see DCLF's
		 * objects. Only the vertex stage's bindings are needed, which the depth pass has bound; the pixel
		 * stage is the DCLF_DEPTH_ONLY build, which reads nothing that is not bound yet.
		 */
		void CaptureDepthPass();

		/** @brief Hybrid path, before the deferred composite: the colour pass, against the depth above. */
		void ExecuteColour();

		/**
		 * @brief CS_DCLF_ASYNC: at Prepass, after RefreshFrameConstants, submits the colour epoch's build to the
		 * worker (AsyncWorker.h). The epoch joins it; a job that cannot serve the epoch is rebuilt inline.
		 */
		void KickColourBuild();

		/**
		 * @brief CS_DCLF_ASYNC: at the end of EarlyPrepass, after the pipeline lookups, submits the Z-prepass
		 * epoch's build with a predicted eye (the main camera's, and last frame's captured eye as the previous
		 * one). The epoch checks the prediction against its capture exactly; a miss is stale and built inline.
		 */
		void KickZPrepassBuild();

		/**
		 * @brief CS_DCLF_ASYNC: at BeforeShadowMaps, after the scene phase and BeginShadowFrame, submits the shadow
		 * epoch's build for last frame's render modes. ExecuteShadowFrame joins it; a change of modes is stale.
		 */
		void KickShadowBuild();

		/** @brief At Present: a job the frame never joined is dropped and counted (Stats::Async::leaked). */
		void EndFrame();

		/** @brief Drops every job and waits for the worker: the live toggle, teardown. */
		void DrainAsync();

		/** @brief The `[DCLF] async` report lines for the interval, and resets the interval's async counters. */
		std::string AsyncReport();

		/**
		 * @brief CS_DCLF_GBUFFER_PROBE=<x>x<y>: read one texel of every main-pass target back and log it.
		 *
		 * Called either side of the colour epoch, it says what DCLF changed in the G-buffer the composite
		 * then consumes, in the same frame and at the same texel - which a comparison between two runs
		 * cannot, because the camera never lands in exactly the same place twice.
		 */
		void ProbeTargets(const char* a_label);

		/** @brief Before the deferred composite: with CS_DCLF_DEBUG_VIEW=1, replace the main pass's targets with DCLF's. */
		void ShowDebugView();

		/** @brief CS_DCLF_SHADOWS=1: the shadow views are drawn by the render graph as well. */
		static bool ShadowsEnabled();

		/**
		 * @brief BeforeShadowMaps: the shadow frame begins. The eye is the reference every record of the
		 * frame is packed relative to; each view's epoch adds its own eye delta (Utility.hlsl, DCLFEyeDelta).
		 */
		void BeginShadowFrame(const RE::NiPoint3& a_eye);

		/**
		 * @brief Inside a shadow view's FinishAccumulatingPreResolveDepth, after the native draws: captures
		 * the view - its target and slice, viewport, eye and the per-frame constants the engine drew it
		 * with - for the frame's shadow epoch. Nothing is drawn here.
		 */
		void ExecuteShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode);

		/**
		 * @brief AfterShadowMaps: one epoch that culls the frame's casters against every captured view and
		 * draws them into the views' slices of the engine's shadow maps.
		 *
		 * One epoch for all views, not one per view, because the graph's own execution costs ~0.9 ms of
		 * CPU per epoch whatever it draws; the views' slices are consumed only after Main_RenderShadowMaps
		 * returns (the shadow mask, the volumetric lighting), so drawing them all at its end is equivalent
		 * to drawing each after its native draws (engine notes: shadow maps).
		 */
		void ExecuteShadowFrame();

		/**
		 * @brief Skylighting's occlusion map, drawn by DCLF (Skylighting::RenderOcclusion's variant with DCLF running):
		 * the engine's RenderMask sets the camera and clears the map, and CaptureSkyOcclusion takes the view at its
		 * FinishAccumulating hook (render mode 0x1C); ExecuteSkyOcclusion then draws every occluder of the frame's
		 * shadow build (the objects' Skylighting::OcclusionTechnique) into it, GPU-culled, in its own epoch.
		 * SkyOcclusionReady says whether it can this frame; when it cannot, the engine's SetupMask registers them.
		 */
		void CaptureSkyOcclusion();
		bool SkyOcclusionReady() const;
		bool ExecuteSkyOcclusion(bool a_diagnose = false);
		/** @brief [TEMP] CS_DCLF_SKYLIGHT_PARITY: each diagnosed occluder's footprint in the map (texels), for the comparison. */
		struct SkyFootprint
		{
			std::int32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
			std::string label;
		};
		std::vector<SkyFootprint> skyFootprints;

		/** @brief Why a shadow view was offered to the epoch and not drawn (ShadowStats::notReadyReasons). */
		enum class ShadowNotReady : std::uint32_t
		{
			Setup,      // no render graph, no Utility shader, or the shadow resources could not be created
			Pipelines,  // no shadow pipeline in the set yet
			Tables,     // the scene phase has not built this frame's tables, or the view is unknown
			Depth,      // the engine's shadow map could not be imported, or the view draws into an unknown target
			Epoch,      // the epoch itself failed
			Capacity,   // more views this frame than the epoch holds (kMaxShadowViews)
			Count
		};

		struct ShadowStats
		{
			std::uint32_t views = 0;           // views offered (captured by the hook), per report interval
			std::uint64_t sunEntryChecks = 0, sunEntryMismatches = 0;  // CS_DCLF_PERSISTENT_PARITY: the GPU sun-entry test on the latch against the CPU verdict
			std::uint32_t viewsDrawn = 0;      // views drawn by an epoch
			std::uint32_t epochs = 0;          // shadow epochs run (one per frame with views)
			std::uint32_t notReady = 0;        // views skipped: resources, pipelines or the depth import not ready
			std::array<std::uint32_t, static_cast<std::size_t>(ShadowNotReady::Count)> notReadyReasons{};
			std::uint32_t focusSkipped = 0;    // focus views, native until S4
			std::uint32_t volumetricSkipped = 0;  // volumetric lighting copies: only volumetric-only casters, native
			std::uint32_t faceUploads = 0;        // face position regions uploaded (a head's snapshot changed), per report interval
			// The culling's GPU counters of one sampled view (the count buffer read back a few frames after
			// its epoch): what says the frustum test is doing something, and against which view.
			std::uint32_t cullDrawn = 0, cullRejected = 0, cullTested = 0;
			std::uint32_t cullSampledView = ~0u, cullSampledMode = 0;
			// CS_DCLF_SHADOW_OWNERSHIP=static: casters claimed per render mode at the last publication.
			std::array<std::uint32_t, 3> claimed{};
			double claimMs = 0.0;              // per report interval, building and publishing the claim sets
			std::uint32_t inputs = 0;          // casters submitted, last view
			std::uint32_t skippedPipeline = 0; // casters without a ready shadow pipeline, last view
			std::uint32_t skippedTexture = 0;  // alpha-tested casters whose diffuse could not be resolved, last view
			std::uint32_t deferredTextures = 0;   // of those, ones whose diffuse the lookups had not resolved yet (next frame)
			std::uint32_t deferredPipelines = 0;  // casters whose shadow pipeline the lookups had no entry for yet
			std::uint32_t records = 0;         // binding records, last frame
			double cpuMs = 0.0;                // per report interval, all views
			double captureMs = 0.0;            // per report interval, the hooks' captures
			double prepareMs = 0.0;            // of which the once-per-frame preparation
			double inputsMs = 0.0;             // of which building (per mode) and uploading the inputs
			double blocksMs = 0.0;             // of which the view's constant blocks and their uploads
			double executeMs = 0.0;            // of which the graph's own execution (compile, prepare, record)
			// Skylighting's occlusion map (ExecuteSkyOcclusion), per report interval.
			std::uint32_t skyDrawn = 0;        // maps DCLF drew
			std::uint32_t skyNotReady = 0;     // draws asked for that it could not make
			std::uint32_t skyInputs = 0;       // occluders, last map
			double skyMs = 0.0;                // render thread, its epoch
		};
		const ShadowStats& GetShadowStats() const { return shadowStats; }
		void ResetShadowStats() { shadowStats = {}; }

		const Stats& GetStats() const { return stats; }

		~IndirectDraws();

	private:
		IndirectDraws();

		/** @brief Assembles this frame's draws, uploads them and runs one epoch of the given segment. */
		void RunEpoch(RenderGraphRuntime::Segment a_segment);

		struct Impl;
		std::unique_ptr<Impl> impl;
		Stats stats;
		ShadowStats shadowStats;
		bool failed = false;
	};

	inline constexpr std::array<const char*, 8> kEpochPartNames{
		"textures/samplers", "constant groups", "record push", "per-draw tail", "per-draw prologue",
		"uploads", "epoch prologue", "graph execute"
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(IndirectDraws::ShadowNotReady::Count)> kShadowNotReadyNames{
		"setup", "pipelines", "tables", "depth", "epoch", "capacity"
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(IndirectDraws::Skip::Count)> kSkipNames{ "pipeline", "geometry", "texture", "sampler",
		"constants", "capacity", "not-skipped-natively", "candidate-only", "record-capacity" };
}
