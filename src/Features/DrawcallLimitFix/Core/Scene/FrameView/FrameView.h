#pragma once

// The render thread's view of the scene: the publication the frame installed (its tables, lookups, catalog and claims), the publication
// log walked up to it, the frame's captures and the requests it serves for the coordinator. Render thread only (and the frame's builds,
// which read the installed publication); it reaches the coordinator only through Scene::Channels.

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Channels.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/FrameTables.h"

namespace DCLF
{
	struct FrameGlobals;
	struct EntrySpace;
	struct PipelineCatalog;
	namespace Diagnostics
	{
		struct SceneView;
	}
}

namespace DCLF::Scene
{
	class FrameView
	{
		// The scene area's observers and reports read the private state through it.
		friend struct Diagnostics::SceneView;

	public:
		using Tables = SceneTables;
		static FrameView& Get();
		/** @brief The render thread's frame number (BeginFrame). */
		std::uint32_t GetFrame() const { return frame; }

		/**
		 * @brief The four textures a ProjectedUV draw binds (pixel slots 3, 8, 10, 11: diffuse, normal, detail, noise). Engine
		 * globals changed only by the ReloadProjectedUVTextures console command; refreshed by every native projected draw, and
		 * posted on change (SharedBindings::projectedPosted).
		 */
		struct ProjectedTextures
		{
			static constexpr std::array<std::uint32_t, 4> kSlots{ 3, 8, 10, 11 };
			std::array<ID3D11ShaderResourceView*, 4> views{};
			bool valid = false;
		};
		/** @brief The frame's globals the ports read (RefreshFrameConstants, at Prepass), latched by each commit into its frame blocks. */
		struct FrameCapture
		{
			// FrameLighting's rows (LightingConstants.h), read by DCLF_BINDLESS draws from the frame block (PS b13), merged from the
			// frame's evaluations over last frame's. A change of these alone versions no pipeline.
			std::array<float, 24> lighting{};
			// FrameFog: read by the DCLF_BINDLESS vertex stage (VS b13).
			std::array<float, 12> fog{};
			// The character light's noise (kCharacterLightRegister), kept while the sample has none (a cell loading).
			ID3D11ShaderResourceView* characterLightView = nullptr;
		};
		struct PublicationStats
		{
			// Publications installed (and skipped past), and frames whose snapshot brought no newer one.
			std::uint64_t installed = 0, kept = 0, skipped = 0;
		};
		/**
		 * @brief Render thread, at Present and frame start (menus too): whether a load screen is up. On the first sighting raises
		 * Channels::loadingSeen and withdraws the set (PassCapture::PublishSet(nullptr)); the first frame after catches up every
		 * switch and captures the category nodes. Also records the switch events' thread (PushSwitch).
		 */
		bool NoteLoadingScreen();
		/**
		 * @brief Render thread, frame start, not during a load: switch catch-ups, the category capture, the mirror's capture and
		 * fade node requests, tree LOD's mirror. a_running false (DCLF idle): switch catch-ups and tree LOD only.
		 */
		void ServeFrameRequests(bool a_running);
		/**
		 * @brief NiSwitchNode::OnVisible's catch-up (AE 0x140d29700), outside the cull: when the selected child was not updated since
		 * the switch's last update pass (childRevID[index] != revID), its revision is marked current and it takes UpdateDownwardPass
		 * with the switch's saved time. True when it ran. Render thread only: engine code writing the subtree's transforms.
		 */
		static bool CatchUpSwitch(RE::NiSwitchNode& a_switch);
		/**
		 * @brief Render thread, before the culls: catches up switch events whose selection changed and switches under attached
		 * subtrees, only in the world (reached from Main::WorldRootNode; a subtree a loader still assembles is skipped). After a
		 * load, every switch in the world.
		 */
		void CatchUpSwitches(std::span<RE::NiAVObject* const> a_attached, std::span<const Scene::SwitchEvent> a_switches);
		/**
		 * @brief PrimaryCull, before the list jobs: the switch nodes whose selection changed since the last call, as keys (never
		 * dereferenced). True when the caller must reread every switch (full walk, dropped events, or overflow).
		 */
		bool TakeSwitchChanges(std::vector<const RE::NiAVObject*>& a_out);
		/**
		 * @brief Whether a load screen is up (the scene graph is being rebuilt). Nothing may walk the scene graph while it holds, and
		 * run-comparable frame counters should not advance across it.
		 */
		static bool IsLoadingScreenUp();
		/**
		 * @brief Latches the main camera's accumulator. Call where `globals::game::currentAccumulator` is set: it is a *currently
		 * rendering* pointer, null before the main pass although the accumulator is built before the shadow maps. Earlier readers
		 * (diagnostics) use the latch.
		 */
		void LatchAccumulator();
		/**
		 * @brief Prepass: re-evaluates the per-pipeline per-frame constants against the main camera. Earlier, the renderer's shadow
		 * state is the shadow camera's (EyePosition is relative to posAdjust). The Z-prepass epoch runs before and uses last frame's
		 * values, harmless only because World is patched per object with the epoch's eye and Terrain LOD's HighDetailRange is a
		 * technique row's. Evaluates the frame's fog, not the technique rows (RefreshTechniqueRows).
		 */
		void RefreshFrameConstants();
		void NoteProjectedTextures();
		/**
		 * @brief The tables the frame reads: the installed publication's (immutable), the newest taken while none is installed, empty
		 * before the first; never the coordinator's. Render thread and the frame's builds.
		 */
		const Tables& GetTables() const { return View(); }
		/** @brief The frame's per-frame engine values: render thread, and builds kicked after its writes. */
		const FrameTables& GetFrameTables() const { return frameTables; }
		/** @brief Render thread, frame start, before any build: the frame's values sized and keyed to the frame's tables. */
		void SyncFrameTables()
		{
			const Tables& view = View();
			frameTables.SyncPipelines(view.pipelines, view.pipelineBindingVersion, frameTablesGeneration, view.pipelinesStamp);
		}
		/**
		 * @brief Render thread, EarlyPrepass: posts the accumulate phase's inputs (PostAncestorMap), drains the capture (DrainCapture) and
		 * notifies Diagnostics::RegistrationsDone. No scene work: the next scene pass takes them (TakeAccumulateInputs).
		 */
		void PostAccumulateInputs();
		/**
		 * @brief Render thread, Prepass: the frame's pipeline sample with a sun, posted latest-wins for the coordinator's new
		 * pipelines' PerGeometry blocks (MakeNewPipelineConstants).
		 */
		void PostPipelineFrame(const GeometryPort::PipelineFrame& a_frame);
		/**
		 * @brief Render thread, after HandOverAtFrameStart and a covered coverage decision: the installed publication's claims made the
		 * frame's (the first installation applies them whole: the engine's hooks hold none before it; NotDrawingYet ends).
		 */
		void InstallClaims();
		/**
		 * @brief Any thread: the frame's claims - the installed set, null during a load or before the first installation (NotDrawingYet).
		 * Immutable, swapped whole; PassCapture::PublishSet holds the same object for the engine-facing hooks.
		 */
		std::shared_ptr<const SetSnapshot> FrameClaims() const { return std::atomic_load(&frameClaims); }
		bool HasInstalled() const { return installed != nullptr; }
		/**
		 * @brief Render thread: no claims were installed yet - every frame before DCLF's first covered one (its tables, resources and first
		 * revision being made), whose epochs draw nothing. The only claims-less state besides a load screen: once installed, the claims
		 * are never withdrawn (a frame waits for a snapshot it can draw from instead, IndirectDraws::AdoptCurrentSnapshot).
		 */
		bool NotDrawingYet() const { return !drawingStarted; }
		/**
		 * @brief Render thread, frame start after the coverage decision (InstallClaims, or nothing while NotDrawingYet), before the culls
		 * (Extensions::ComponentsAdopt): the claims the frame installed (null while NotDrawingYet), and by component the deltas of the
		 * publications installed since the last adoption, oldest first; cleared after it (ClearAdoptedDeltas).
		 */
		const SetSnapshot* AdoptedClaims() const;
		std::span<const std::vector<std::shared_ptr<const void>>> AdoptedDeltas() const { return frameComponentDeltas; }
		void ClearAdoptedDeltas();
		PublicationStats TakePublicationStats() { return std::exchange(publicationStats, PublicationStats{}); }
		/**
		 * @brief Render thread, frame start (reads no coordinator state): installs a_adopted (IndirectDraws::AdoptSnapshot's
		 * publication) when newer - walks the publication log up to it, makes its tables, lookups and catalog the frame's
		 * (DrawPipelines::HoldCatalog), returns the replaced one, takes its candidates and delivers held PrimaryCull notes.
		 */
		void HandOverAtFrameStart(std::shared_ptr<const void> a_adopted);
		const std::shared_ptr<const Tables>& AcceptedTables() const { return acceptedTables; }
		/** @brief Render thread: the frame's capture (latched by every commit). */
		const FrameCapture& GetFrameCapture() const { return frameCapture; }
		/** @brief The frame's tables' generation (0 before the first); moves on a slot table reset or InvalidateVerdicts. */
		std::uint32_t GetTablesGeneration() const { return frameTablesGeneration; }
		/** @brief The sun entries DCLF can take out of the cascade culls (UpdateSunCandidates), with their generation. */
		std::shared_ptr<const EntrySpace> GetSunCandidates() const { return frameSunCandidates; }
		/**
		 * @brief The entries DCLF can take out of the point lights' culls (UpdateLightCandidates): light entries (LightEntryOf: an
		 * actor's root, a reference's, a terrain block's multibound node) whose every tracked geometry is a table object
		 * (BuildSunExclusion), or one the light's registration takes nothing from (Eligibility::Light).
		 */
		std::shared_ptr<const EntrySpace> GetLightCandidates() const { return frameLightCandidates; }
		/**
		 * @brief Render thread: count of light entries that gained their first tracked geometry. An entry LocalLightCull's filter cut
		 * for holding none is judged again when this moves.
		 */
		std::uint64_t GetLightEntriesAppeared() const { return frameLightEntriesAppeared; }
		/**
		 * @brief Whether a node is a light entry (LightDependentsOf's rule: a key with tracked geometries, not a category node). The
		 * render thread's set, kept from the publication log and written only at frame start.
		 */
		bool IsLightEntry(const RE::NiAVObject* a_node) const
		{
			return frameLightEntrySet.contains(a_node) && !IsCategoryNode(a_node);
		}
		/** @brief Whether a node is one of the cells' category nodes DCLF tracks (RefreshCategoryNodes; the newest publication's set). */
		bool IsCategoryNode(const RE::NiAVObject* a_node) const
		{
			return frameCategoryNodes && frameCategoryNodes->contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_node)));
		}
		/** @brief The frame's lookups (Lookups.h), chosen like the frame's tables. Made by the scene lane, immutable. */
		const Lookups& GetLookups() const { return frameLookups ? *frameLookups : EmptyLookups(); }
		static const Lookups& EmptyLookups()
		{
			static const Lookups empty;
			return empty;
		}
		/** @brief Whether the slot is a resident record in the frame's tables. */
		bool IsMember(std::int32_t a_object) const
		{
			const auto& resident = View().residentSlot;
			return a_object >= 0 && static_cast<std::size_t>(a_object) < resident.size() && resident[a_object] != 0;
		}
		/**
		 * @brief Render thread, frame start: the frame number, the frame's globals, the frame inputs posted (waking the pump). Then:
		 * snapshot adopted and installed, claims installed, ServeFrameRequests, the filters. The frame reads only its own state,
		 * immutable publications and the log; everything else is a message (released engine references: at Present, EngineReleases).
		 */
		void BeginFrame();
		/**
		 * @brief Render thread, EarlyPrepass: serves the material captures the last joins asked for (ServeMaterialRequests). Reads
		 * nothing of the coordinator's (the scene pass may run).
		 */
		void PrepareAccumulatePhase();
		/** @brief An object slot's SetPhase bits, 0 for a non-member. Render thread, or any thread between commits. */
		std::uint8_t SetPhasesOf(std::int32_t a_object) const
		{
			const auto& phases = View().setPhases;
			return a_object >= 0 && static_cast<std::size_t>(a_object) < phases.size() ? phases[a_object] : std::uint8_t{ 0 };
		}
		/**
		 * @brief Objects in a phase that are not yet its members (joining). An occlusion map DCLF draws leaves them out until they
		 * join (FrameDriver::OcclusionNeedsEngine counts those frames); the engine's cull is no fallback for them.
		 */
		std::uint32_t SetLacking(std::uint8_t a_phase) const
		{
			// The counts are by Extensions::ViewBitTable::lackingPhases: the occlusion maps', then the LodTechniques views' phases' (the
			// reflection's).
			const auto& lacking = Extensions::ResolvedViewBits().lackingPhases;
			const bool lod = a_phase && (a_phase & lacking[2]) == a_phase;
			return a_phase && a_phase == lacking[0] ? frameLackingCount[0] : a_phase && a_phase == lacking[1] ? frameLackingCount[1] : lod ? frameLackingCount[2] : 0u;
		}
		/**
		 * @brief Render thread (PrimaryCull): its admitted roots, a whole set, posted latest-wins; the coordinator's next scene pass hands them
		 * to the components (Api::CoordinatorComponent::Events::admitted).
		 */
		void SetStandInRoots(const std::vector<Api::StandInRoot>& a_roots)
		{
			// No wake: the frame's next pass takes it.
			channels.standInRootsPosted.Post(std::make_unique<std::vector<Api::StandInRoot>>(a_roots));
		}
		/**
		 * @brief Render thread: whether the installed publication claims the geometry, which keeps it alive (the retirement chain
		 * keeps what a held publication names). PrimaryCull's liveness check (NoteSetChanges).
		 */
		bool HeldByInstalled(const RE::BSGeometry* a_geometry) const
		{
			return installed && installed->claims && installed->claims->phases.contains(a_geometry);
		}
		/**
		 * @brief Render thread, at an execution's commit: import leases of cleared geometry slots, held until the GPU retires it.
		 * In-flight frames read those buffers by device address, invisible to DXVK, which would otherwise reuse the memory.
		 */
		std::vector<std::shared_ptr<const void>> TakeRetiredImports() { return std::exchange(frameRetiredImports, {}); }
		/** @brief Render thread: a GPU object the frame's executions may still read, released with the next main commit's (TakeRetiredImports). */
		void RetireImport(std::shared_ptr<const void> a_owner) { frameRetiredImports.push_back(std::move(a_owner)); }
		/** @brief Render thread, frame start: a coordinator component's seeds requested since the last take (its feature's frame start). */
		std::vector<FadeSeedItem> TakeComponentSeeds(Api::CoordinatorComponentId a_component)
		{
			return a_component.index < frameComponentSeeds.size() ? std::exchange(frameComponentSeeds[a_component.index], {}) : std::vector<FadeSeedItem>{};
		}
		/** @brief Render thread: the frame's engine globals (FrameValues binds them for the seeds' LOD scale). */
		const std::shared_ptr<const FrameGlobals>& FrameGlobalsOfFrame() const { return frameGlobals; }
		/** @brief Render thread, frame start: the slots named since the last take, once (from the publication log). */
		std::vector<ShadingItem> TakeShadingItems() { return std::exchange(frameShading, {}); }
		/**
		 * @brief Render thread: the newest publication's geometry by object slot (empty without one), held by it until the next install
		 * (the shading fields' captures: Extensions::CaptureShadingFields).
		 */
		std::span<RE::BSGeometry* const> NewestObjectGeometry() const;
		/** @brief Render thread, frame start: the walks' plans since the last take, oldest first (FrameValues draws with the newest). */
		std::vector<std::shared_ptr<const PlacementPlan>> TakePlacementPlans() { return std::exchange(framePlans, {}); }
		/**
		 * @brief Render thread: asks the coordinator to drop every cached classification verdict and derivation at its next scene
		 * pass (InvalidateVerdicts) - a classification toggle changed (Toggles.h) or the feature came back on.
		 */
		void RequestVerdictsInvalidated() { ++verdictsRequested; }
		/** @brief Render thread (diagnostics): the slots the publication log holds past the frame's cursor, not taken. */
		std::vector<ShadingItem> PeekShadingItems() const;
		/** @brief Render thread (frame start, an inline pass's end): delivers the coordinator's notes to PrimaryCull. */
		void DeliverPrimaryNotes();

	private:
		Channels& channels = Channels::Get();
		void CaptureCategories(bool a_force);
		/** @brief A cheap hash of everything the category capture reads to find category nodes. */
		std::uint64_t CategorySignature() const;
		/**
		 * @brief Render thread, frame start: captures what the coordinator found the mirror lacking (Channels::mirrorCaptureRequests)
		 * with its chain up to the world root (SceneCapture::CaptureAttached) onto the tracker's stack, mirror-only.
		 */
		void CaptureMirrorRequests();
		/** @brief Render thread, frame start (ServeFrameRequests): the fade node requests answered. */
		void ServeFadeNodeRequests();
		/**
		 * @brief Render thread, EarlyPrepass: drains the capture every frame (its fixed-capacity buffer and withholding counters).
		 * The registrations are the observers' alone (Diagnostics::CaptureDrained); the normal path reads none of it.
		 */
		void DrainCapture();
		/** @brief Render thread, BeginFrame (coordinator idle): the engine's BSLightingShader instance, once. */
		void CaptureLightingShader();
		/** @brief Render thread (PostAccumulateInputs): the ancestor value's map (Light Limit Fix's room map) filled and posted when its generation moves. */
		void PostAncestorMap();
		/** @brief Render thread, after BeginFrame's capture: the frame's inputs posted for the coordinator. */
		void PostFrameInputs();
		/**
		 * @brief Render thread, Prepass: FrameCapture::characterLightView from the frame's sample. The records' frame components are
		 * the scene work's (RefreshMaterialSignatures).
		 */
		void RefreshCharacterLightView();
		void PostPipelineConstants(std::uint32_t a_slot);
		/** @brief The frame's tables: the accepted publication's, empty immutable tables before the first (never the coordinator's). */
		const Tables& View() const { return acceptedTables ? *acceptedTables : EmptyTables(); }
		static const Tables& EmptyTables()
		{
			static const Tables empty;
			return empty;
		}
		void PublishFrameClaims(std::shared_ptr<const SetSnapshot> a_claims);
		/** @brief Render thread: a replaced or skipped publication back to the coordinator, which drops it (Channels::retiredPublications). */
		void ReturnPublication(std::shared_ptr<const ScenePublication>&& a_publication);
		/**
		 * @brief Render thread, EarlyPrepass: the captures the joins asked for (Channels::materialRequests), read off the held
		 * property onto MaterialPort::captures; every request answered (Channels::materialRequestsAnswered).
		 */
		void ServeMaterialRequests();
		/** @brief Render thread: the installed publication, null before the first. */
		std::shared_ptr<const ScenePublication> Newest() const { return installed; }
		FrameCapture frameCapture;  // GetFrameCapture
		FrameTables frameTables;    // GetFrameTables: the frame's values, out of the tables (render thread)
		std::shared_ptr<const CategoryCapture> categoryCapture;  // render thread: the newest made
		// Captures not yet superseded by an applied one, oldest first; their engine references are dropped here (render thread).
		std::deque<std::shared_ptr<const CategoryCapture>> categoryCapturesHeld;
		std::uint64_t categoryCaptures = 0;  // render thread: the last capture's generation
		// Render thread, since the last report: capture time (FrameGlobals, CaptureCategories), category captures made, frames.
		std::uint64_t captureNs = 0, categoryCaptureNs = 0, categoryCapturesMade = 0, captureFrames = 0;
		std::uint64_t categoryMirrorCaptures = 0, categoryMirrorRecords = 0;  // CaptureCategories' captures for the mirror
		// The frame's engine globals (FrameGlobals::Capture at BeginFrame; FrameValues binds them for its seeds).
		std::shared_ptr<const FrameGlobals> frameGlobals;
		std::uint64_t ancestorMapPostedGeneration = ~0ull;  // render thread: the ancestor map's generation last posted
		// Render thread: the first frame after a load catches up every switch in the world (CatchUpSwitches) and forces its category
		// capture (as after a detach: categoryDetachSeen).
		bool worldCatchUpPending = false;
		bool categoryCapturePending = false;
		std::vector<RE::NiAVObject*> attachedRoots;  // the ingestion's attached subtrees, for CatchUpSwitches (scratch)
		// The frame's copies, taken at frame start from the newest publication and the log (HandOverAtFrameStart).
		std::shared_ptr<const EntrySpace> frameSunCandidates, frameLightCandidates;
		std::uint64_t frameLightEntriesAppeared = 0;
		std::uint32_t frameTablesGeneration = 0;  // GetTablesGeneration
		std::vector<const RE::NiAVObject*> frameSwitchChanges;
		bool frameSwitchResync = true;
		bool switchResyncNext = false;  // NoteLoadingScreen: PrimaryCull rereads every switch from the next frame start
		std::vector<std::shared_ptr<const void>> frameRetiredImports;
		std::vector<std::shared_ptr<const PlacementPlan>> framePlans;
		std::vector<ShadingItem> frameShading;
		std::vector<std::vector<FadeSeedItem>> frameComponentSeeds;  // by coordinator component
		ankerl::unordered_dense::set<const RE::NiAVObject*> frameLightEntrySet;
		std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> frameCategoryNodes;  // the installed publication's (IsCategoryNode)
		// The main camera's accumulator, latched non-null.
		RE::BSGraphics::BSShaderAccumulator* latchedAccumulator = nullptr;
		// HandOverAtFrameStart: the installed publication's lookups and catalog, the newest published while none is installed.
		std::shared_ptr<const Lookups> frameLookups;
		std::shared_ptr<const PipelineCatalog> frameCatalog;
		std::shared_ptr<const Tables> acceptedTables;
		ProjectedTextures projectedTextures;
		bool loadingPosted = false;  // render thread: the load's marker is posted
		bool loadSinceCapture = false;  // render thread: a load came since the last category capture (the job before it is dropped)
		// Render thread: by coordinator component, the deltas of the publications installed since the last adoption (AdoptedDeltas).
		std::vector<std::vector<std::shared_ptr<const void>>> frameComponentDeltas;
		// The installed publication (the adopted snapshot's, HandOverAtFrameStart).
		std::shared_ptr<const ScenePublication> installed;
		// Cursors into the log, both up to the installed publication: frame inputs taken and claims' changes taken (never past
		// the frame inputs'). Null: the log's first node.
		PublicationLog<PublicationDeltas>::Node* deltasCursor = nullptr;
		PublicationLog<PublicationDeltas>::Node* notesCursor = nullptr;
		// Installed claim changes not yet told to PrimaryCull, whether one is waiting, the claims it was told of, the lacking counts.
		std::vector<std::pair<std::vector<const RE::BSGeometry*>, std::vector<const RE::BSGeometry*>>> installNotes;
		bool installPending = false;
		std::shared_ptr<const SetSnapshot> notedClaims;
		// FrameClaims: the render thread writes through PublishFrameClaims (which also tells the registration hooks), any thread reads.
		std::shared_ptr<const SetSnapshot> frameClaims;
		std::array<std::uint32_t, 3> frameLackingCount{};
		PublicationStats publicationStats;
		bool drawingStarted = false;  // the first claims installed (InstallClaims; NotDrawingYet until then), never cleared
		// The frame sample the held pipeline blocks were made from (RefreshFrameConstants' last with a sun; also PostPipelineFrame).
		std::optional<GeometryPort::PipelineFrame> geometryPortFrame;
		std::uint32_t geometryPortFrameNumber = ~0u;
		// The first RefreshFrameConstants evaluates every pipeline in full, seeding the frame lighting with variables the sample's
		// pipeline does not write (e.g. AmbientSpecularTintAndFresnelPower); later pipelines (WriteObject) publish no lighting.
		bool constantsRefreshed = false;
		std::uint32_t verdictsRequested = 0;  // render thread: bumped by each request to drop the cached verdicts
		std::uint32_t frame = 0;
		// RefreshFrameConstants' stats: full evaluations, frame samples, pipelines changed, frame lighting versions.
		struct GeometryStats
		{
			std::uint64_t frames = 0, full = 0, samples = 0, changed = 0, lightingVersions = 0;
		} geometryStats;
		// Since the last report: pipeline blocks posted to the coordinator (PostPipelineConstants), Prepass frames.
		std::atomic<std::uint64_t> pipelinesPosted{ 0 };  // taken by the coordinator's report
		std::uint64_t shadingFrames = 0;
	};
}
