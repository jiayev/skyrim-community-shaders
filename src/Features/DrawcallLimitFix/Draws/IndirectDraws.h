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

namespace org::runtime
{
	class IUploadService;
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
		 * @brief The set's shadow phases DCLF can draw (SetPhase bits; SceneStore::SetCapability): the casters' and the point lights'
		 * once the shadow resources and the rasterizer state catalog are set up, and each enabled occlusion map's once its map has been
		 * captured. A capability, not what a frame drew: it changes only at setup, on a toggle or a failure, so which views the
		 * engine asks for in a frame never changes the set. Published by the render thread at the frame's start
		 * (UpdateShadowCapability); any thread.
		 */
		std::uint8_t ShadowCapability() const;
		/** @brief Render thread, at the frame's start: sets up what the capability needs and publishes it. */
		void UpdateShadowCapability();
		/** @brief The report's line: the capability, the catalog, and its changes since startup ("<- PHASES": a view it did not foresee). */
		std::string ShadowCapabilityReport() const;
		/**
		 * @brief Render thread, before ShadowViews::Rebuild: how many shadow views DCLF draws - every one (UINT32_MAX) once the view
		 * placements are set up (a view draws at its placement's slot), none before, after a failure, or on a frame without claims.
		 */
		std::uint32_t ShadowViewCapacity();
		/**
		 * @brief BeforeShadowMaps, after ShadowViews::Rebuild and before the engine draws a view (scene revisions): the frame's views are
		 * DCLF's with the selected revision's shape for the placements (one shape, whichever views come); until a revision has recorded
		 * it (startup, a growth), every one is the engine's this frame (ShadowViews::UncoverAll).
		 */
		void DecideShadowCoverage();
		/**
		 * @brief Render thread, SceneStore::CommitSet: whether the object is ready for a shadow phase (SetPhaseOfMode's): its Utility
		 * pipeline under every rasterizer state of the catalog in each of the phase's modes of the capability, and an alpha-tested
		 * caster's diffuse imported. Part of the set's readiness (SceneSet.h). a_tables: the commit's SceneStore::Tables
		 * (the coordinator's: the frame's view may hold another object in the slot).
		 */
		/** a_why (optional), when not: SceneStore::SetStats::waitingBy's index (7 shadow pipeline, 11 an occluder's fade root not serviced, 12 past
		 * the scene buffers, 13 the alpha test's diffuse not imported). */
		bool PhaseReady(const void* a_tables, std::uint32_t a_slot, std::uint8_t a_phase, std::uint32_t* a_why = nullptr) const;
		/** @brief Changes when the capability's modes or the catalog's rasterizer states that PhaseReady reads change: at setup. */
		std::uint64_t ShadowReadinessSerial() const;
		/**
		 * @brief SceneStore::CommitSet: whether the object is within the scene buffers (and, for the main builds, the material and
		 * pipeline rows) the builds ahead are made against (the fit the
		 * frame's start posted with their context, PostAheadContext): the main builds' (a_shadow false) or the shadow build's. One past
		 * them has no input until their growth (ObjectFits), so a member must wait for it: claimed, it would be drawn by nobody. The
		 * buffers only grow, and a build ahead takes a context posted at or after the commit's. a_tables: the caller's
		 * SceneStore::Tables (the coordinator's own in CommitSet and the revocation, not the frame's view).
		 */
		bool FitsScene(const void* a_tables, std::uint32_t a_slot, bool a_shadow) const;
		/** @brief Changes when FitsScene's fits do (a growth of the scene buffers): the slots waiting for one are taken again. */
		std::uint64_t SceneFitSerial() const;

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

		/** @brief Render thread, the frame's start: the main material and shared lookups refreshed (step 6e C), against the frame's tables. */
		void RefreshMainLookups();
		/**
		 * @brief The coordinator, publishing the scene (SceneStore::PublishScene, step 6e E3b): the publication's stream views and the
		 * Z-prepass and colour payloads, built from a_tables (its SceneStore::Tables) with what the frame's start posted
		 * (PostAheadContext) and staged for the buffers as the publication before leaves them. The frames that install it commit
		 * them: no build, no join in the frame.
		 */
		std::shared_ptr<const void> BuildAhead(std::shared_ptr<const void> a_tables);
		/** @brief Render thread: whether a publication's draws (BuildAhead's) are done, so the frame may install it. */
		bool DrawsReady(const std::shared_ptr<const void>& a_draws) const;
		/** @brief Render thread, the frame's start (the coordinator idle): what the next builds ahead take from the frame (resources, masks). */
		void PostAheadContext();
		/**
		 * @brief Render thread, the frame's start, after InstallDraws: the payload ring entry the frame's epochs read, made to hold
		 * the installed payloads, and what fills it (FrameValues' job runs it before the frame's signal, step 6e E4). Empty: none.
		 */
		std::function<void(org::runtime::IUploadService&)> PrepareFrameUploads();
		/** @brief Render thread: the frame's producer did not run (FrameValues::Kick refused): its epochs read no ring entry. */
		void DropFrameUploads();
		/** @brief Render thread, the frame's start: the installed publication's draws (SceneStore::InstalledDraws), the frame's. */
		void InstallDraws(std::shared_ptr<const void> a_draws);

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
		 * SetCapability). ExecuteReflection: render thread, once a frame at BeforeShadowMaps, after the frame's reflection updates
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

		/**
		 * @brief Render thread, at the scene work's join (SceneStore::FinishSceneWork): the main segments' shapes as a scene revision
		 * made now would have them (R3c), with the capacities reserved for the tables as they are; compared with the commits' own
		 * (Impl::ShapeParity). Counts only: nothing draws with them yet.
		 */
		void MakeRevisionShapes();
		/**
		 * @brief Render thread, first at BeginSceneFrame, every frame: the graph's build point. Its builds are explicit
		 * (PersistentGraphHost::SetExplicitBuilds): an extension added or removed during a frame waits for this point, so the graph a
		 * frame's revision was recorded on runs through that frame.
		 */
		void BuildPoint();
		/** @brief Render thread, BeginSceneFrame: the newest complete scene revision selected (Impl::SceneRevisions; counts only). */
		void SelectRevision();
		/**
		 * @brief Render thread, BeginSceneFrame, after SelectRevision: whether the selected revision covers the frame's main epochs - it has
		 * their recordings, of the graph as built now (strict epochs). False: the frame has no claims (SceneStore::WithdrawSet) and DCLF
		 * draws nothing. Also decides the other epochs' coverage (Impl::EpochCovered). True without revisions.
		 */
		bool DecideCoverage();
		/**
		 * @brief Render thread, BeginSceneFrame, after SelectRevision: whether the set committed at a_commitFrame may become the
		 * frame's claims (SceneStore::ApplySet) - the selected revision was made at or after it, so its shapes and pipelines hold
		 * every claim. True without revisions, or when none was made for that commit (no resources yet).
		 */
		bool SetApplicable(std::uint32_t a_commitFrame) const;
		/** @brief Whether the frame's claims are a scene revision's: a structural change after its join revokes one. */
		bool RevisionClaims() const;
		/** @brief The set committed at a_commitFrame was applied: the frame's claims are its (RevisionHoldsClaims). */
		void NoteSetApplied(std::uint32_t a_commitFrame);
		/** @brief Since the last call: the stream views a fallback wanted while the coordinator or the builds task ran (none made). */
		std::uint64_t TakeStreamsRefused();

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
