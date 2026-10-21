#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "Features/DrawcallLimitFix/Api/Views.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDrawStats.h"

struct ID3D11Texture2D;

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
	struct SceneTables;
	struct ForwardTargets;
	struct TargetFormats;
	struct ToggleSet;
	struct ViewEpochState;
	namespace Draws
	{
		struct MainPayload;
		struct PassFrame;
		struct Resources;
		struct SceneBuffers;
		struct ViewFace;
	}
	namespace TreeInstances
	{
		struct DrawRow;
	}
	namespace Api
	{
		enum class DrawFamily : std::uint8_t;
	}

	/**
	 * @brief An epoch sharing another's shape (Api::EpochDef::sharesWith), submitted by its owner (IndirectDraws::SubmitSharedEpoch):
	 * its counters since the last report (TakeSharedEpochStats), and the frame it was last submitted in (the once-a-frame guard).
	 */
	struct SharedEpochStats
	{
		std::uint64_t reached = 0, epochs = 0, noSharedPayload = 0, noRecording = 0, failed = 0;
		std::uint32_t submittedFrame = ~0u;
	};
	/**
	 * @brief The owner's part of a shared epoch's submission: prepare, render thread, right before the execution, handed context, the
	 * main resources and the selected revision's shape (null: none). It prepares the owner's optional part (Features/Forward: the
	 * refraction pipeline's inputs) and says whether this execution draws it.
	 */
	struct SharedEpochSubmit
	{
		bool (*prepare)(void* a_context, const Draws::Resources* a_resources, const Draws::PassFrame* a_shape) = nullptr;
		void* context = nullptr;
	};
	/** @brief A shared epoch's submission: whether it was submitted, and whether its optional part draws (what prepare said). */
	struct SharedEpochResult
	{
		bool submitted = false;
		bool optional = false;
	};

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
	 *   - the main thread at Main::Update's entry: WriteAccumulated and the verdicts;
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
		 * @brief Render thread, before ShadowViews::BeginCasters (Core's BeginShadowViews, Shadow/ShadowPoints.h): how many shadow views
		 * DCLF draws - every one (UINT32_MAX) once the view placements are set up, none before, after a failure, or on a frame without
		 * claims.
		 */
		std::uint32_t ShadowViewCapacity();
		/**
		 * @brief Before the engine keeps its first shadow light (the shadow epochs' beforeShadowCasters): whether the frame's views are
		 * DCLF's with the selected revision's shape; until a revision has recorded it (startup, a growth), every one is the engine's this
		 * frame (ShadowViews::SetDrawable(false)). Called by
		 * Features/Shadows/Engine's shadow epochs handler (Api/ShadowEpochs.h), as are BeginShadowFrame, OcclusionReady and
		 * ExecuteShadowFrame.
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
		 *
		 * a_hostedWork: the work of the view epochs the depth epoch hosts (Api::EpochDef::hostedBy: the reflection's faces and first
		 * person's depth, their features' ViewPoint::DepthCommitted work), run by the depth commit inside its epoch's execution, after its
		 * own latch writes and before its recording's submission: a hosted epoch's SubmitViewEpoch there commits into it. Not run when the
		 * depth epoch does not commit.
		 */
		void CaptureDepthPass(const std::function<void()>& a_hostedWork = {});

		/** @brief Where the main pass's opaque batches end: the colour epoch, against the depth above. */
		void ExecuteColour();

		/**
		 * @brief Render thread, at a_epoch's trigger (its owner's hook: Features/Forward's blended draw for the forward epoch): epoch
		 * a_epoch, which shares a main epoch's shape (Api::EpochDef::sharesWith), submitted once a frame. Only after this frame's
		 * commit of the shared epoch (DCLF drawing), whose commit wrote everything its draws read: the selected revision's recording, the
		 * shared payload's owners held, a_submit's prepare, then the execution in the epoch's segment. Each early return counted.
		 */
		SharedEpochResult SubmitSharedEpoch(std::uint32_t a_epoch, const SharedEpochSubmit& a_submit);
		/** @brief Render thread, at the report: a_epoch's shared epoch counters since the last report, reset (its submitted frame kept). */
		SharedEpochStats TakeSharedEpochStats(std::uint32_t a_epoch);
		/**
		 * @brief The shared epochs' report lines (ReportPoint::AsyncWorkForward: Features/Forward's forward and refraction lines, then the
		 * forward epoch's features'), on a report where a shared epoch reached a frame since the last or its gate class is on.
		 */
		std::string SharedEpochReport();

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
		 * what it could not know ahead (graph build, targets, shadow format, main resources); then the one adopted before stands. Its
		 * publication's toggles are never stale: installed, they are the frame's (Toggles::Adopt). Returns its ScenePublication
		 * (installed by FrameView::HandOverAtFrameStart when newer), null before the first.
		 */
		std::shared_ptr<const void> AdoptSnapshot();
		/**
		 * @brief Render thread, the frame's start, after the build point and the frame's posts: AdoptSnapshot, then with a_wait (DCLF
		 * draws: its first covered frame is past, and no load screen is up), while the adopted snapshot is not one the frame can draw
		 * from (Impl::AdoptedCurrent: after a graph build, new main resources, new targets or shadow format), waits for the builder's
		 * next and adopts it - a one-off stall on a rare event, so DCLF never hands a frame back to the engine. Nothing the builder needs
		 * is posted later in the frame: the revision inputs are the build point's, the context for new main resources was posted at the
		 * Setup that made them (PostResourcesAhead), the publication gate's scene pass half is open while it waits, and the owner thread's
		 * requests (shadow view slots, a build, tickets a recording waits for) are served in the wait. Five seconds without one is a
		 * defect (stl::report_and_fail, naming what it waited for), never a fallback. Counted (the strict epochs line). Returns the adopted
		 * publication, as AdoptSnapshot.
		 */
		std::shared_ptr<const void> AdoptCurrentSnapshot(bool a_wait);
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
		 * @brief The scene's tables (null until the render thread made them, EnsureSceneBuffers; then kept until IndirectDraws goes): a
		 * feature's scene resources outside its callbacks (Extensions::SceneResourcesOf), read on the render thread or on the main thread at
		 * Main::Update's entry (Features/Fades' write-back: its root states' kick and wait, and its stores).
		 */
		const Draws::SceneBuffers* Scene() const;
		/**
		 * @brief Render thread, at the frame's start: hands the harvested, undecoded visibility readback batches to the decode task on
		 * DCLF's executor, never joined, and frees the batches the last task decoded (dclf-architecture.md, "The visibility events").
		 * Writes no engine memory.
		 */
		void KickVisibilityReadback();
		/**
		 * @brief Main thread at Main::Update's entry (no cull or job list running): makes the decoder's kAccumulated writes with atomic
		 * read-modify-writes: the stood-in roots and their geometry (while a_stoodIn says PrimaryCull still stands in), and the wide
		 * writes (other listed roots, plain entry roots, the nodes between, the cell roots). a_live false: dropped, and the next
		 * decode writes everything again. kAccumulated is the engine side effect of the main camera's cull that DCLF produces where the
		 * culls are stood in for (skyrim-engine-notes.md, "Engine side effects of the culls"; its observers are Diagnostics::Accumulated's;
		 * the actors' animation stamp is Features/Actors', ActorRate.cpp).
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
		 * @brief Render thread, the frame's first DCLF point, before the main camera's cull (FrameDriver): each taken draw family's decide
		 * (Api/DrawFamilies.h: while it is set up, on and the frame's claims installed; tree LOD's, with its pipelines from its program
		 * source on the lane, DrawFamilyFrame::Pipelines), published: DrawFamilyOwned, and PassCapture withholds the engine's passes the
		 * family's draws replace (SetFamilyOwned).
		 */
		void DecideDrawFamilies(std::uint32_t a_frame);
		/** @brief Render thread: whether a_family's draws are DCLF's this frame (DecideDrawFamilies; false before the implementation). */
		bool DrawFamilyOwned(Api::DrawFamily a_family) const;
		/**
		 * @brief The view engine (Views/ViewEngine.cpp): Core's calls for the features that take an engine view (Api/Views.h,
		 * Extensions::Views) - their state, targets, pipelines and resources at their frame's start, and their epochs' inputs and
		 * submission - each over the view's slot, so a feature's work and epochs reproduce Core's own. Render thread unless said otherwise;
		 * nothing before the implementation exists.
		 */
		/** @brief The view's frame state (Epochs/ViewEpoch.h; null before the implementation exists): its targets, resources, coverage. */
		const ViewEpochState* ViewState(Api::EngineView a_view) const;
		/** @brief The main pass's captured formats (null before the implementation exists): what a view's targets follow. */
		const TargetFormats* MainTargetFormats() const;
		/** @brief The view's targets, published for the scene lane (LaneTargets) when they changed; true then. */
		bool SetViewTargets(Api::EngineView a_view, const ForwardTargets& a_targets);
		/** @brief Posts what a scene revision reads of the frame (a view's targets, depth range or resources moved). */
		void PostViewInputs();
		/** @brief The engine's depth range for the view (a revision input: the shapes' viewport): posted when a positive range changed it. */
		void SetViewDepthRange(Api::EngineView a_view, float a_range);
		/** @brief The view's depth range: the last SetViewDepthRange's, else its shape's (Api::ViewShape::depthRange). */
		float ViewDepthRange(Api::EngineView a_view) const;
		/** @brief Publishes whether the main set's forward pipelines draw into the view's forward part (ViewSlotReady); the previous value. */
		bool ExchangeViewForwardCompatible(Api::EngineView a_view, bool a_compatible);
		/**
		 * @brief What ResolveViewPipelines found: the slots with a key of the view's, and those whose pipeline a_catalog has; with
		 * a_programs, those whose program a_catalog has, and whether a pipeline missing there is still to come (not failed).
		 */
		struct ViewPipelineCounts
		{
			std::uint32_t slots = 0, ready = 0, programs = 0;
			bool waited = false;
		};
		/**
		 * @brief The view's forward pipelines of a_wanted's slots (by pipeline slot, nonzero: wanted; empty: every slot), asked of the
		 * pipeline lane (each key once) and read from a_catalog (the frame's) for the view's targets, into its state's slot pipelines.
		 */
		ViewPipelineCounts ResolveViewPipelines(Api::EngineView a_view, const SceneTables& a_tables, const PipelineCatalog* a_catalog, std::span<const std::uint8_t> a_wanted,
			bool a_programs = false);
		/** @brief The view's state's slot pipelines dropped (the view off: none of its slots is ready, PhaseReady). */
		void DropViewPipelines(Api::EngineView a_view);
		/**
		 * @brief The draw families drawn per face by a trees view (Api::ViewShape::trees: the view engine's, from each family's tables; tree
		 * LOD's), render thread, once a frame after DecideDrawFamilies; nothing for a view without trees. ResolveViewFaces: per family, its
		 * forward pipeline asked of the pipeline lane and read from a_catalog for the view's targets while a_on and the family are
		 * (Extensions::DrawFamilyOn; none otherwise); true while one is still to come and the main view's family is DCLF's.
		 * DecideViewFaces: per family, whether the faces' draws are DCLF's this frame (the view drawable and covered, its pipeline ready, the
		 * main view's family DCLF's, the last depth commit's row and the family's pipelines there: its faces.row), published to PassCapture
		 * (SetViewFamilyOwned). SubmitViewEpoch draws their rows then.
		 */
		bool ResolveViewFaces(Api::EngineView a_view, bool a_on, const PipelineCatalog* a_catalog);
		void DecideViewFaces(Api::EngineView a_view);
		/**
		 * @brief Render thread, at a cube camera update's start (Features/Reflection's update hook): the camera's LOD roots a_view's faces
		 * draw in the engine's place (Api::ViewRootBit: the faces' roots and the view root contributions'), whose add-roots are skipped so the engine neither culls nor
		 * registers them, read of the classes' contributions and the LOD gates: the tree root while the faces' tree LOD is DCLF's, the LOD
		 * land and objects roots while the LOD swaps are gated (a show veto registered: Features/Lod's LOD gates) and both are DCLF's (Api::ObjectClass::LodObjects and LodTerrain
		 * on); none on a non-plain update, while the view is not
		 * drawable or covered, or on an update that keeps them for the residue watch (the view's residueDue: Api::ViewContribution).
		 */
		std::uint32_t ViewLodRootsOwned(Api::EngineView a_view, bool a_plain);
		/** @brief The view's graph resources, once the main resources and index pool exist; true when it has them. */
		bool SetUpView(Api::EngineView a_view);
		/**
		 * @brief The view's target imported into the graph, as its entry says (Api::ViewTargetImport): kPOST_ZPREPASS_COPY for a
		 * DepthTarget view, a_texture (the engine's) with DCLF's own depth for a ColourWithOwnDepth one; true when it is.
		 */
		bool ImportViewTarget(Api::EngineView a_view, ID3D11Texture2D* a_texture = nullptr);
		/** @brief What a view's epoch draws from (TakeViewInputs): the installed payloads it holds, and its scene list's inputs. */
		struct ViewListed
		{
			std::array<const Draws::MainPayload*, 2> owners{};
			std::uint32_t ownerCount = 0;
			std::uint32_t inputs = 0;
		};
		/**
		 * @brief Which of the frame's commits a view's epoch draws from. DepthThisFrame: the frame's scene list with this frame's depth
		 * commit (its index pool, inputs, records, geometry rows) of the adopted set, the view's target imported before (its frame start);
		 * DepthThisFrameTargetAfter: the same, the view's work setting up and importing its target after taking them (the reflection's cube,
		 * known only at its faces' capture), its frame constants its own commit's; ColourThisFrame: this frame's colour commit's payload and
		 * frame constants.
		 */
		enum class ViewInputsFrom : std::uint8_t
		{
			DepthThisFrame,
			DepthThisFrameTargetAfter,
			ColourThisFrame,
		};
		enum class ViewInputsTaken : std::uint8_t
		{
			Taken,
			Stale,     // the commit is not the frame's, or nothing is listed
			NoTarget,  // the view's resources or its imported target are missing (DepthThisFrame)
		};
		ViewInputsTaken TakeViewInputs(Api::EngineView a_view, ViewInputsFrom a_from, ViewListed& a_listed);
		enum class ViewSubmit : std::uint8_t
		{
			Submitted,
			NoRevision,  // the adopted revision has no recording or shape for the epoch
			NoOwners,    // the payloads it draws are not held
			Failed,      // the graph refused the epoch
		};
		/**
		 * @brief The view's epoch a_epoch (0 the depth or only one, 1 the colour one) submitted: its recording from the adopted revision,
		 * with a commit of a_faces (its cameras: none commits nothing) into its shape, holding a_listed's payloads; a_drawn: the faces the
		 * commit drew. A hosted epoch (Api::EpochDef::hostedBy: the reflection's and first person's depth, the depth epoch's passes) is committed into its host's
		 * commit instead, only from inside it (the view's work the depth commit runs: CaptureDepthPass), and its host submits its recording
		 * with the epoch's work; NoRevision anywhere else. A trees view's commit takes each draw family's row (tree LOD's as the last depth commit uploaded it) while the faces'
		 * family is DCLF's (DecideViewFaces) and the shape draws it, its texture bound again for the execution. A view with frame constants
		 * of its own (Draws::OwnsFrameConstants: the reflection's) commits them too, from this frame's sources, its frame textures held for
		 * the execution; a_nullTextures: the frame texture registers (t16 and up) its frame record left null, a bit each (none committed: untouched).
		 */
		ViewSubmit SubmitViewEpoch(Api::EngineView a_view, std::uint32_t a_epoch, const ViewListed& a_listed, std::span<const Draws::ViewFace> a_faces,
			std::uint32_t* a_drawn = nullptr, std::array<std::uint64_t, 2>* a_nullTextures = nullptr);
		/** @brief Any thread: whether the view's epochs draw its members this frame (its coverage, decided at the frame's start). */
		bool ViewCovered(Api::EngineView a_view) const;
		/**
		 * @brief The scene lane's commit (Coordinator::SetCapability, under the commit's toggles) and the frame's view decisions (under
		 * the frame's): whether the view's phase can be drawn - its toggle in a_toggles, and its targets published (a capability: each
		 * member then waits for its own pipelines, ViewSlotReady).
		 */
		bool ViewDrawable(Api::EngineView a_view, const ToggleSet& a_toggles) const;
		/**
		 * @brief The scene lane's commit: whether a_slot's pipelines for the view are in a_catalog (the lane's), as the view's membership
		 * reads it, from nothing the render thread writes per frame. CategoryRoot: a_why (the set parity's observer) 1 no catalog or
		 * targets, 2 a forward key the forward part cannot draw, 3 no key of the view's, 4 its pipeline not in a_catalog, 5 no Z-prepass
		 * group pipeline. LodTechniques: the slot's forward pipeline for the view's published targets (a_why untouched).
		 */
		bool ViewSlotReady(Api::EngineView a_view, const void* a_tables, const void* a_lookups, const PipelineCatalog* a_catalog, std::uint32_t a_slot,
			std::uint32_t* a_why = nullptr) const;

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
		 * @brief Render thread, after AdoptCurrentSnapshot: whether the snapshot covers the main epochs (not stale, recordings of the
		 * graph as built now). False only before DCLF's first covered frame (no claims: FrameView::NotDrawingYet) or under a load screen;
		 * after it, a defect (FrameDriver::BeginFrame fails). Also decides the other epochs' coverage.
		 */
		bool DecideCoverage();
		/**
		 * @brief Render thread, after DecideCoverage: its inputs and verdicts as text (the adopted snapshot, each epoch's coverage), for
		 * a defect's report.
		 */
		std::string CoverageState() const;
		/** @brief Whether DCLF cannot draw (set once, by the render thread): the frame then has no claims. */
		bool Failed() const { return failed.load(std::memory_order_relaxed); }

		/** @brief Waits for the builds ahead, the root states' task (Features/Fades' write-back) and the visibility decode; lets go of the frame's stream views. */
		void DrainAsync();

		/** @brief The `[DCLF] async` report lines for the interval; resets its counters. */
		std::string AsyncReport();

		/** @brief A named main-pass point (around the Z-prepass and colour epoch) for Diagnostics::TargetPoint (CS_DCLF_TARGET_DUMP). */
		void ProbeTargets(const char* a_label);

		/**
		 * @brief BeforeShadowMaps: the shadow frame begins; views are captured from here (CaptureShadowView). And counts a frame whose
		 * views DCLF would draw but does not (ShadowViews::Unrecorded).
		 */
		void BeginShadowFrame();

		/**
		 * @brief Inside a shadow view's FinishAccumulatingPreResolveDepth, after the native draws: captures the view (target and
		 * slice, viewport, eye, the per-frame constants) for the shadow epoch. Nothing is drawn here.
		 */
		void CaptureShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode);

		/**
		 * @brief AfterShadowMaps: one epoch culling the casters against every captured view into the views' slices, and the shadow build's
		 * occluders into the occlusion maps in a_maps (taken at the shadow maps: OcclusionReady; their views set up since: OcclusionView);
		 * returns the maps drawn. One epoch, not one per view (each graph execution costs CPU); the slices are consumed only after
		 * Main_RenderShadowMaps returns, and the maps not before the Prepass, so it is equivalent.
		 */
		std::uint32_t ExecuteShadowFrame(std::uint32_t a_maps);

		/**
		 * @brief The occlusion maps (kOcclusionViews: Skylighting's sky map, the precipitation mask), render thread. CaptureOcclusion,
		 * at FinishAccumulating (render mode 0x1C): the observers'. OcclusionView, once the map's camera is set up, before its RenderMask:
		 * the view as DCLF computes it (its layout predicts the shadow shape's view of the map). OcclusionReady, at the shadow maps before
		 * the engine draws them: whether the frame's shadow epoch can draw the map. When it does not, the engine's SetupMask registers the
		 * map's scene at its own occlusion point.
		 */
		void CaptureOcclusion(std::uint32_t a_view);
		void OcclusionView(std::uint32_t a_view, const RE::NiCamera& a_camera);
		bool OcclusionReady(std::uint32_t a_view) const;

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
