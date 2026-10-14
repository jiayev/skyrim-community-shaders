#pragma once

#include <array>
#include <atomic>
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
	namespace Diagnostics
	{
		struct DrawsView;
	}
	struct Lookups;
	struct LookupsResolveState;
	struct PipelineCatalog;

	/**
	 * @brief DCLF's draws: the installed scene publication's objects drawn by the render graph with GPU-culled indirect draws; the
	 * engine is told not to draw what DCLF claims.
	 *
	 * Each DCLF draw point is an epoch (Revision/Epochs.h): Z-prepass (depth, HZB, two-phase occlusion culling), colour (EQUAL
	 * against the Z-prepass), forward (blended, back to front), shadow views and occlusion maps, water reflection faces, first
	 * person. An epoch submits the selected scene revision's recording; its commit writes only what the frame has (constant blocks,
	 * frame textures, the latch) into the revision's shape, and its draws read the installed payload (PayloadVersions, by value).
	 * Nothing is built in the frame.
	 *
	 * Threads:
	 *   - the coordinator (scene lane): PhaseReady, FitsScene, the slot readiness checks, ResolveLookups, PostSnapshotWork;
	 *   - the snapshot builder (DCLF's preparation pool, one pass at a time): a publication's payloads and scene revision, the
	 *     complete snapshot posted for the frame's start (AdoptSnapshot);
	 *   - the main thread at Main::Update's entry: WriteFadeStores, WriteAccumulated and the verdicts;
	 *   - the render thread: everything else.
	 * The hand-offs between them are latest-wins slots and atomics.
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
		 * @brief The shadow phases DCLF can draw (SetPhase bits; Coordinator::SetCapability). A capability, changed only at setup, on a
		 * toggle or a failure, so the views a frame asks for never change the set. Published by the render thread; read from any thread.
		 */
		std::uint8_t ShadowCapability() const;
		/** @brief Render thread, at the frame's start: sets up what the capability needs and publishes it. */
		void UpdateShadowCapability();
		/** @brief The report's line: the capability, the catalog, and its changes since startup ("<- PHASES": a view it did not foresee). */
		std::string ShadowCapabilityReport() const;
		/**
		 * @brief Render thread, before ShadowViews::Rebuild: how many shadow views DCLF draws - every one (UINT32_MAX) once the view
		 * placements are set up, none before, after a failure, or on a frame without claims.
		 */
		std::uint32_t ShadowViewCapacity();
		/**
		 * @brief BeforeShadowMaps, after ShadowViews::Rebuild: the frame's views are DCLF's with the selected revision's shape; until a
		 * revision has recorded it (startup, a growth), every one is the engine's this frame (ShadowViews::UncoverAll).
		 */
		void DecideShadowCoverage();
		/**
		 * @brief Scene lane (Coordinator::CommitSet): whether the object is ready for a shadow phase: its Utility pipeline under every
		 * state a_lookups resolved for the phase's modes (never the render thread's readyModes/readyStates), and an alpha-tested
		 * caster's diffuse imported. a_tables: the coordinator's. a_why: SetStats::waitingBy's index (7 shadow pipeline, 12 past the
		 * scene buffers, 13 diffuse not imported).
		 */
		bool PhaseReady(const void* a_tables, const Lookups& a_lookups, std::uint32_t a_slot, std::uint8_t a_phase, std::uint32_t* a_why = nullptr) const;
		/**
		 * @brief Coordinator::CommitSet: whether the object fits the scene buffers (and main rows) the main (a_shadow false) or shadow
		 * builds ahead are made against. One past them has no input until a growth, so claimed it would be drawn by nobody; it waits.
		 * Reads the coordinator's copy of the fits (TakeAheadContext), never the render thread's.
		 */
		bool FitsScene(const void* a_tables, std::uint32_t a_slot, bool a_shadow) const;
		/** @brief Changes when FitsScene's fits do (a growth of the scene buffers): the slots waiting for one are taken again. */
		std::uint64_t SceneFitSerial() const;
		/** @brief Coordinator, at each pass's start: takes the latest ahead context and fits the frame's start posted. */
		void TakeAheadContext();

		/** @brief Where the main pass's opaque batches start: captures its bindings and viewport for the colour epoch. */
		void CaptureMainPass();

		/**
		 * @brief Inside the native depth pass, once the world's depth is drawn: the Z-prepass epoch, writing DCLF's depth into the
		 * engine's main depth.
		 *
		 * It runs here, not with the colour pass, because everything later that uses depth (the native draws' depth test, the sky,
		 * Terrain Blending's blended depth, every effect reading it) derives from the depth buffer at this point. Only the vertex
		 * stage's bindings are needed, which the depth pass has bound; the pixel stage is the DCLF_DEPTH_ONLY build.
		 */
		void CaptureDepthPass();

		/** @brief Where the main pass's opaque batches end: the colour epoch, against the depth above. */
		void ExecuteColour();

		/**
		 * @brief The forward epoch (CS_DCLF_FORWARD), at the engine's blended draw (FUN_1414b39d0, after the deferred composite and
		 * the water): the visible forward objects, sorted back to front by the colour epoch, drawn before the engine's own blended
		 * draws. Render thread; only after this frame's colour epoch, whose commit wrote everything these draws read.
		 */
		void ExecuteForward();
		/** @brief The forward epoch's report line, its counters reset. */
		std::string ForwardReport();
		/**
		 * @brief Whether this frame's forward epoch drew the refraction normals (and marked them begun for the engine). Render thread,
		 * at Main::Draw's refraction point (Main_Draw_RefractionCheck).
		 */
		bool RefractionDrawnThisFrame() const;

		/**
		 * @brief Render thread, the frame's start: posts what the scene lane's lookups need that only the frame has (import context,
		 * shadow capability, shadow modes, states and formats), latest wins.
		 */
		void PostLookupInputs();
		/**
		 * @brief Scene lane: a_lookups' pipeline entries and shadow pipelines resolved from a_tables and a_catalog alone, asking the
		 * pipeline lane for missing keys. Reads no engine state and no render-thread service.
		 */
		void ResolveLookups(const void* a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups, LookupsResolveState& a_state);
		/**
		 * @brief Coordinator::PublishScene: hands a_publication (a ScenePublication) to the snapshot builder with the last ahead
		 * context, latest wins. The builder makes its payloads and scene revision and posts the complete snapshot (AdoptSnapshot).
		 */
		void PostSnapshotWork(std::shared_ptr<const void> a_publication);
		/**
		 * @brief Coordinator::ScenePass, at its end (whether it published or not): the pass of frame a_frame's inputs is done, so any
		 * publication it made was posted (PostSnapshotWork comes first). The snapshot builder publishes a payload only for a snapshot the
		 * frame will adopt: not before the pass of the frame that took the last one has ended (its publication supersedes a held one).
		 */
		void NoteScenePassDone(std::uint32_t a_frame);
		/**
		 * @brief Any thread: the sequence of the newest publication the snapshot builder has taken from its work slot (0: none; ~0 without a
		 * builder). The coordinator publishes again only once the last is taken, or for the frame's pass (publication pacing): one posted
		 * meanwhile would only replace it in the slot.
		 */
		std::uint64_t SnapshotWorkTaken() const;
		/**
		 * @brief Render thread, the frame's start (a publication installed): the age of the oldest event it and the skipped
		 * publications carried (ms and frames), for the snapshot report.
		 */
		void NoteEventAdoption(double a_ms, std::uint32_t a_frames);
		/**
		 * @brief Render thread, the frame's start, after the build point: adopts the newest posted snapshot whole, unless stale for
		 * what it could not know ahead (graph build, targets, shadow format, toggles older than a_togglesGeneration, main resources);
		 * then the frame has no claims until the next. Returns its ScenePublication (installed by FrameView::HandOverAtFrameStart when
		 * newer), null before the first.
		 */
		std::shared_ptr<const void> AdoptSnapshot(std::uint32_t a_togglesGeneration);
		/**
		 * @brief Render thread, at Present (after the frame's last submission): binds the adopted set's pooled versions
		 * (NamedVersions::Bind) and the installed payload's backings (PayloadVersions::Bind) to SubmittedRetirementPoint(), so no
		 * backing the frame read is reused before its GPU work retires.
		 */
		void BindAdopted();
		/**
		 * @brief Render thread, the frame's start: posts what the next builds ahead take from the frame (resources, masks) and the
		 * fits, through a latest-wins slot (TakeAheadContext).
		 */
		void PostAheadContext();

		/**
		 * @brief Render thread, the frame's start: hands the fade events read back since the last kick to a task on DCLF's executor
		 * that judges them into the stores WriteFadeStores makes (drawcall-limit-fix.md, "The fade write-back"). Without it a stood-in
		 * root's node stays as the engine last wrote it for its readers (tree LOD crossfade, occlusion cull, animation gate).
		 */
		void KickFadeWriteBack();
		/**
		 * @brief Main thread at Main::Update's entry (no list cull, cell animation or animation job runs): makes the last task's
		 * stores on the roots DCLF writes (a_written: PrimaryCull::FadeWrittenByDclf; other roots are left to the engine's cull).
		 * a_live false (DCLF does not run): dropped.
		 */
		void WriteFadeStores(bool a_live, const std::function<bool(const void*)>& a_written);
		/**
		 * @brief Render thread, at the frame's start: hands the harvested, undecoded visibility readback batches to the decode task on
		 * DCLF's executor, never joined, and frees the batches the last task decoded (drawcall-limit-fix.md, "visibility events").
		 * Writes no engine memory.
		 */
		void KickVisibilityReadback();
		/**
		 * @brief Main thread at Main::Update's entry (no cull or job list running): makes the decoder's kAccumulated writes with atomic
		 * read-modify-writes: the stood-in roots and their geometry (while a_stoodIn says PrimaryCull still stands in), and the wide
		 * writes (other listed roots, plain entry roots, the nodes between, the cell roots). a_live false: dropped, and the next
		 * decode writes everything again.
		 */
		void WriteAccumulated(bool a_live, const std::function<bool(const void*)>& a_stoodIn);
		/**
		 * @brief Main thread after WriteAccumulated: DCLF's main-camera verdict of a stood-in root as last written; visible when not
		 * written yet. Reads no engine memory.
		 */
		bool AccumulatedVerdict(const void* a_root) const;
		/**
		 * @brief Main thread after WriteAccumulated: DCLF's verdict of any root it lists (a fade root or plain entry root, from the
		 * wide writes: TreeAnimation's tree list); false when it has none. Reads no engine memory.
		 */
		bool AccumulatedVerdictOf(const void* a_root, bool& a_visible) const;
		/** @brief Render thread: a camera cut in a_frame (PrimaryCull): every stood-in root visible until a batch of a_frame or later. */
		void NoteCameraCut(std::uint32_t a_frame);
		/** @brief Any thread: the next decode writes every stood-in root and geometry, not only the changes (the engine wrote them). */
		void RequestAccumulatedRefresh();
		/** @brief The visibility readback's line, and the <- ACCUMULATED and <- ANIM RATE observers' since the last call. */
		std::string VisibilityReport();
		/**
		 * @brief Render thread, the frame's first DCLF point, before the main camera's cull: whether DCLF draws tree LOD this frame
		 * (toggle, pipelines, tables, the engine's tree LOD texture). If so PassCapture withholds the engine's tree LOD passes.
		 */
		bool DecideTreeLod();
		/**
		 * @brief The water reflection's faces (dclf-lod.md, "Water reflections"), render thread. CaptureReflectionFace: at a face's
		 * accumulator render's end, its targets. PrepareReflection: once a frame after DecideTreeLod, the LOD slots' and tree LOD's
		 * forward pipelines. ExecuteReflection: in the depth hook right after the Z-prepass epoch (its commit), after the frame's updates (TESWaterReflections::Update runs twice
		 * a frame, a face each): one epoch drawing the faces' members and tree LOD into the cube target.
		 */
		void CaptureReflectionFace();
		/**
		 * @brief Render thread, once the engine has oriented the cube camera to a face, before its cull and draws: the face's view as
		 * DCLF computes it (SunViews' arithmetic; the face's slice of the cube target).
		 */
		void ReflectionFaceCamera(const RE::NiCamera& a_camera, std::uint32_t a_face);
		/**
		 * @brief Render thread, at a reflection update's start: the cube camera's LOD roots DCLF draws (ReflectionFaces::RootBits),
		 * whose add-roots are skipped so the engine neither culls nor registers them.
		 */
		std::uint32_t ReflectionRootsOwned(bool a_plain);
		void PrepareReflection();
		bool ReflectionDrawable() const;
		/**
		 * @brief Scene lane's commit: whether the slot's faces' forward pipeline is in a_catalog (the lane's) for the faces' targets:
		 * the reflection phase's readiness, read from nothing the render thread writes per frame.
		 */
		bool ReflectionSlotReady(const void* a_tables, const PipelineCatalog* a_catalog, std::uint32_t a_slot) const;
		void ExecuteReflection();
		std::string ReflectionReport();
		/**
		 * @brief The first-person view (CS_DCLF_FIRST_PERSON; drawcall-limit-fix.md, "First person as a view bit"). Render thread.
		 * BeginFirstPersonFrame, before DecideCoverage: the latch (Main::Draw's cVar23 recomputed: first-person root shown, main menu
		 * closed, camera not the free one), depth range, targets, pipelines. ExecuteFirstPersonDepth: at RenderDepth's first-person
		 * draw (camera set up by FUN_1414cb020): the depth epoch. ExecuteFirstPersonColour: at FUN_1414cb6d0's accumulator render.
		 */
		void BeginFirstPersonFrame();
		void ExecuteFirstPersonDepth(bool a_controlDriven);
		void ExecuteFirstPersonColour();
		bool FirstPersonDrawable() const;
		bool FirstPersonSlotReady(const void* a_tables, const void* a_lookups, const PipelineCatalog* a_catalog, std::uint32_t a_slot, std::uint32_t* a_why = nullptr) const;
		bool FirstPersonCovered() const;
		std::string FirstPersonReport();

		/**
		 * @brief Render thread: posts what a scene revision reads of the frame (Impl::RevisionInputs) when it moved; called where such
		 * a value is captured outside IndirectDraws (after the shadow views' Rebuild).
		 */
		void NoteRevisionInputs();
		/**
		 * @brief Render thread, first at every frame's start: the graph's build point. Builds are explicit, so the graph a frame's
		 * revision was recorded on runs through that frame. Also serves the builder's requests (shadow view slots).
		 */
		void BuildPoint();
		/**
		 * @brief Render thread, after AdoptSnapshot: whether the snapshot covers the main epochs (not stale, recordings of the graph as
		 * built now). False: the frame has no claims (FrameView::WithdrawSet). Also decides the other epochs' coverage.
		 */
		bool DecideCoverage();
		/** @brief Whether DCLF cannot draw (set once, by the render thread): the frame then has no claims. */
		bool Failed() const { return failed.load(std::memory_order_relaxed); }

		/** @brief Waits for the builds ahead, the fade write-back and the visibility decode; lets go of the frame's stream views. */
		void DrainAsync();

		/** @brief The `[DCLF] async` report lines for the interval; resets its counters. */
		std::string AsyncReport();

		/** @brief A named main-pass point (around the Z-prepass and colour epoch) for Diagnostics::TargetPoint (CS_DCLF_TARGET_DUMP). */
		void ProbeTargets(const char* a_label);

		/** @brief BeforeShadowMaps: the shadow frame begins; views are captured from here (CaptureShadowView). */
		void BeginShadowFrame();

		/**
		 * @brief Inside a shadow view's FinishAccumulatingPreResolveDepth, after the native draws: captures the view (target and
		 * slice, viewport, eye, the per-frame constants) for the shadow epoch. Nothing is drawn here.
		 */
		void CaptureShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode);

		/**
		 * @brief AfterShadowMaps: one epoch culling the casters against every captured view into the views' slices. One epoch, not one
		 * per view (each graph execution costs CPU); the slices are consumed only after Main_RenderShadowMaps returns, so it is
		 * equivalent.
		 */
		void ExecuteShadowFrame();

		/**
		 * @brief The occlusion maps (kOcclusionViews: Skylighting's sky map, the precipitation mask), render thread. CaptureOcclusion,
		 * at FinishAccumulating (render mode 0x1C): the map's layout. ExecuteOcclusion: the shadow build's occluders into the views in
		 * a_views, in one epoch; returns those drawn. When OcclusionReady is false the engine's SetupMask registers them.
		 */
		void CaptureOcclusion(std::uint32_t a_view);
		/** @brief Render thread, once the map's camera is set up, before its RenderMask: the view as DCLF computes it. */
		void OcclusionView(std::uint32_t a_view, const RE::NiCamera& a_camera);
		bool OcclusionReady(std::uint32_t a_view) const;
		std::uint32_t ExecuteOcclusion(std::uint32_t a_views);

		const ShadowStats& GetShadowStats() const { return shadowStats; }
		void ResetShadowStats() { shadowStats = {}; }

		const Stats& GetStats() const { return stats; }

		~IndirectDraws();

	private:
		friend struct Diagnostics::DrawsView;
		IndirectDraws();

		/** @brief One main epoch (Z-prepass if a_depthOnly, else colour), committed into the selected revision and submitted. */
		void RunEpoch(bool a_depthOnly);

		struct Impl;
		std::unique_ptr<Impl> impl;
		Stats stats;
		ShadowStats shadowStats;
		// Set once by the render thread, read by the scene lane too: atomic.
		std::atomic<bool> failed{ false };
	};

}
