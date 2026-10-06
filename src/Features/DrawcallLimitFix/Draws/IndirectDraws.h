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
		 * @brief The set's shadow phases DCLF draws next frame (SetPhase bits; SceneStore::SetPhasesDrawn): a mode's, once its last
		 * shadow epoch drew the mode's views and left none undrawn. A mode seen for the first time is the engine's for that frame,
		 * so its casters are never withheld from a view DCLF has no pipelines for.
		 */
		std::uint8_t ShadowPhasesDrawn() const;
		/**
		 * @brief Render thread, SceneStore::CommitSet: whether the object is ready for a shadow phase (SetPhaseOfMode's): its Utility
		 * pipeline under every rasterizer state of its caster class in each of the phase's modes the last epoch drew, and an
		 * alpha-tested caster's diffuse imported. Part of the set's readiness (SceneSet.h).
		 */
		bool PhaseReady(std::uint32_t a_slot, std::uint8_t a_phase) const;
		/** @brief Changes when the shadow modes or their views' rasterizer states that PhaseReady reads change. */
		std::uint64_t ShadowReadinessSerial() const;

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
		 * @brief Render thread, at the end of the scene tables: the fade write-back's events read back since the last kick (and any
		 * the last job did not reach), written onto their roots' nodes on the worker (the "fade write-back" job;
		 * drawcall-limit-fix.md, "The fade write-back"). A stood-in root's node is otherwise left as the engine last wrote it, and
		 * the engine's readers of it (the tree LOD's crossfade, the occlusion maps' cull, its next listing) would take that.
		 */
		void KickFadeWriteBack();
		/**
		 * @brief Render thread, at the frame's first DCLF point, after the scene events (the tree LOD mirror's drain) and before the
		 * main camera's cull: whether DCLF draws tree LOD this frame - its toggle on, its programs, pipelines and tables built, the
		 * engine's tree LOD texture there. The registrations withhold the engine's tree LOD passes on it (PassCapture), and the
		 * depth commit draws on it (UploadTreeLod).
		 */
		bool DecideTreeLod();
		/**
		 * @brief The water reflection's faces (dclf-lod.md, "Water reflections"). CaptureReflectionFace: render thread, at a face's
		 * accumulator render's end (ReflectionFaces::InFace), after the engine's draws: the face's targets, and in a plain face render
		 * the face's camera for this update's epoch. PrepareReflection: render thread, once a frame, after DecideTreeLod: the forward
		 * programs and pipelines of the LOD pipeline slots in use, and tree LOD's, for those targets (the reflection phase's
		 * readiness), and whether the faces' tree LOD is DCLF's. ReflectionDrawable: the set draws the reflection phase (SceneStore::
		 * SetPhasesDrawn). ExecuteReflection: render thread, once a frame at BeforeShadowMaps, after the frame's reflection updates
		 * (TESWaterReflections::Update runs twice a frame, a face each): one epoch drawing the frame's faces' reflection-phase
		 * members and tree LOD into the cube target.
		 */
		void CaptureReflectionFace();
		void PrepareReflection();
		bool ReflectionDrawable() const;
		void ExecuteReflection();
		std::string ReflectionReport();
		/**
		 * @brief Render thread, before anything reads the nodes or changes the tables (BeforeShadowMaps, the accumulate phase, the
		 * next scene frame's events): the job joined, or stopped; what it did not reach goes to the next one.
		 */
		void JoinFadeWriteBack();

		/**
		 * @brief CS_DCLF_ASYNC: at BeforeShadowMaps, after the scene phase and BeginShadowFrame, submits the shadow
		 * epoch's build for last frame's render modes. ExecuteShadowFrame joins it; a change of modes is stale.
		 */
		void KickShadowBuild();
		/** @brief The end of the scene phase: the shadow build kicked there, kept at BeforeShadowMaps when nothing it read moved. */
		void KickShadowBuildEarly();
		/**
		 * @brief Render thread, at the scene work's join (SceneStore::FinishSceneWork): the main segments' shapes as a scene revision
		 * made now would have them (R3c), with the capacities reserved for the tables as they are; compared with the commits' own
		 * (Impl::ShapeParity). Counts only: nothing draws with them yet.
		 */
		void MakeRevisionShapes();
		/** @brief Render thread, BeginSceneFrame: the newest complete scene revision selected (Impl::SceneRevisions; counts only). */
		void SelectRevision();
		/** @brief BeforeShadowMaps: whether the early shadow build stands (counted by cause when it does not). */
		bool KeepEarlyShadowBuild();
		/**
		 * @brief Before a placement join writes the tables (BeforeShadowMaps, or EarlyPrepass in a frame without shadow maps):
		 * the early shadow build, which reads them, is done first. Usually it is (kicked ~4.6 ms earlier); one still running
		 * is waited for up to the async budget, or dropped.
		 */
		void BeforePlacementJoin();
		/**
		 * @brief Render thread, where nothing writes the tables until the next epoch's commit (after the placements' join; after
		 * RefreshFrameConstants): the object records and bone rows staged and recorded on the worker, which that commit submits
		 * instead of uploading them itself (Impl::StreamsJob). CS_DCLF_ASYNC.
		 */
		void KickSceneStreams();
		/** @brief Since the last call: the streams' jobs kicked, taken by a commit, and dropped. */
		std::array<std::uint64_t, 3> TakeStreamsStats();

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
