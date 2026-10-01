#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "IndirectDrawStats.h"

namespace RE
{
	class BSGeometry;
	class NiPoint3;
}

namespace DCLF
{
	struct FadeChange;  // Scene/Records.h

	/**
	 * @brief DCLF's objects drawn by the render graph with indirect command streams, into the main pass's own
	 * targets and depth; the native loop skips what DCLF drew.
	 *
	 * Inside the native depth pass, CaptureDepthPass runs the ZPrepass epoch (DCLF's depth, then the HZB and the
	 * two-phase occlusion cull). Where the main (deferred) pass's opaque batches start, CaptureMainPass records
	 * what the pass binds, and where they end ExecuteColour runs the MainOpaque epoch. The draw data is
	 * assembled on the CPU (on the worker where it can be, AsyncWorker.h) and uploaded:
	 *   - constant blocks packed with the native shaders' constant tables: PerTechnique per pipeline,
	 *     PerMaterial per (material, pipeline), PerGeometry per object, Light Limit Fix's StrictLightData
	 *     per (room, shadow mask), the permutation per (pipeline, object flags), the alpha-test reference
	 *     per threshold, and every other bound constant buffer from its CPU mirror (ConstantMirror);
	 *   - one DrawBindings record per object (constant buffer addresses, texture and sampler heap indices);
	 *   - one DrawSequence per object.
	 * The colour pass executes the sequences against the Z-prepass's depth (depth test EQUAL) into the main
	 * pass's targets, imported from DXVK. An object is drawn only when everything its pipeline's shaders read
	 * can be supplied.
	 *
	 * Render thread only.
	 */
	class IndirectDraws
	{
	public:
		using Skip = DrawSkip;
		using Stats = IndirectDrawStats;
		using ShadowNotReady = ShadowViewNotReady;
		using ShadowStats = ShadowDrawStats;

		void ResetCommitTimings()
		{
			stats.commitUs = {};
			stats.commitEpochs = 0;
		}

		static IndirectDraws& Get();

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
			// FadeStateCS's changes that frame (Records.h, FadeChange): the header's count of appends, and the changes copied.
			std::uint32_t fadeAppended = 0, fadeHeld = 0;
			const FadeChange* fadeChanges = nullptr;
		};
		/**
		 * @brief Any one thread at a time (DCLF's worker): hands every feedback frame whose copy the GPU has completed
		 * to a_consume, in submission order, and frees its slot. Never waits: frames still in flight are left for a
		 * later call. Returns the number decoded. See dclf-cull-job-elimination.md, "Phase 2".
		 */
		std::uint32_t DrainVisibilityFeedback(const std::function<void(const VisibilityFeedbackFrame&)>& a_consume);
		/**
		 * @brief Render thread: FadeStateCS's changes of a frame did not all reach the nodes (more than the buffer held:
		 * a_needed, the appends): the buffer grows, and the next update sends every write-back root.
		 */
		void NoteFadeChangesLost(std::uint32_t a_needed);
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
		 * @brief Inside the native depth pass, once the world's depth is drawn: assemble this frame's draws and
		 * write their depth into the engine's main depth.
		 *
		 * It runs here, and not with the colour pass, because everything the rest of the frame does with
		 * depth - the native draws' own depth test, the sky, Terrain Blending's blended depth and every
		 * effect that reads it - is derived from the depth buffer at this point and has to see DCLF's
		 * objects. Only the vertex stage's bindings are needed, which the depth pass has bound; the pixel
		 * stage is the DCLF_DEPTH_ONLY build, which reads nothing that is not bound yet.
		 */
		void CaptureDepthPass();

		/** @brief Where the main pass's opaque batches end: the colour pass, against the depth above. */
		void ExecuteColour();

		/**
		 * @brief CS_DCLF_ASYNC: at Prepass, after RefreshFrameConstants, submits the colour epoch's build to the
		 * worker (AsyncWorker.h). The epoch joins it; a job that cannot serve the epoch is rebuilt inline.
		 */
		void KickColourBuild();
		/**
		 * @brief Prepass, before RefreshFrameConstants: the colour build kicked early (behind the Z-prepass's) is done before the
		 * tables it reads are written. KickColourBuild then keeps it, unless what it read changed.
		 */
		void BeforeFrameConstants();

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
		/** @brief The end of the scene phase: the shadow build kicked there, kept at BeforeShadowMaps when nothing it read moved. */
		void KickShadowBuildEarly();
		/** @brief BeforeShadowMaps: whether the early shadow build stands (counted by cause when it does not). */
		bool KeepEarlyShadowBuild();
		/**
		 * @brief Before a placement join writes the tables (BeforeShadowMaps, or EarlyPrepass in a frame without shadow maps):
		 * the early shadow build, which reads them, is done first. Usually it is (kicked ~4.6 ms earlier); one still running
		 * is waited for up to the async budget, or dropped.
		 */
		void BeforePlacementJoin();

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

		/** @brief BeforeShadowMaps: the shadow frame begins; the views are captured from here (CaptureShadowView). */
		void BeginShadowFrame();

		/**
		 * @brief Inside a shadow view's FinishAccumulatingPreResolveDepth, after the native draws: captures
		 * the view - its target and slice, viewport, eye and the per-frame constants the engine drew it
		 * with - for the frame's shadow epoch. Nothing is drawn here.
		 */
		void CaptureShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode);

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
		 * @brief The occlusion maps (Records.h, kOcclusionViews: Skylighting's sky map, the precipitation mask), drawn by DCLF
		 * (Skylighting::RenderOcclusion's variant with DCLF running): the engine's RenderMask sets each view's camera and
		 * clears its map, and CaptureOcclusion takes the view at its FinishAccumulating hook (render mode 0x1C);
		 * ExecuteOcclusion then draws every occluder of the frame's shadow build (the objects' Skylighting::OcclusionTechnique
		 * for that map) into the views a_views names (a bit per view), GPU-culled, in one epoch, and returns the ones it drew.
		 * OcclusionReady says whether a view can be drawn this frame; when it cannot, the engine's SetupMask registers them.
		 */
		void CaptureOcclusion(std::uint32_t a_view);
		bool OcclusionReady(std::uint32_t a_view) const;
		std::uint32_t ExecuteOcclusion(std::uint32_t a_views);

		const ShadowStats& GetShadowStats() const { return shadowStats; }
		void ResetShadowStats() { shadowStats = {}; }

		const Stats& GetStats() const { return stats; }

		~IndirectDraws();

	private:
		IndirectDraws();

		/** @brief Assembles this frame's draws, uploads them and runs one epoch: the Z-prepass's (a_depthOnly) or the main pass's. */
		void RunEpoch(bool a_depthOnly);

		struct Impl;
		std::unique_ptr<Impl> impl;
		Stats stats;
		ShadowStats shadowStats;
		bool failed = false;
	};

}
