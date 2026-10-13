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

#include "Features/DrawcallLimitFix/Scene/Model/ActorValueIndex.h"
#include "Features/DrawcallLimitFix/Scene/Channels.h"
#include "Features/DrawcallLimitFix/Scene/Model/FrameTables.h"
#include "Features/DrawcallLimitFix/Scene/Ports/TreeLod.h"

namespace DCLF
{
	struct FrameGlobals;
	struct SunCandidates;
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
		 * @brief The four textures the engine binds for a ProjectedUV draw (pixel slots 3, 8, 10 and 11:
		 * the projected diffuse, normal and detail maps and the projection noise), as SetupGeometry left
		 * them at a native draw. They are globals of the engine, changed only by the ReloadProjectedUVTextures
		 * console command, so one capture stands; it is refreshed by every native projected draw seen. A capture whose views changed is
		 * posted to the scene work with a reference on each (SharedBindings::projectedPosted), which asks for their bindings.
		 */
		struct ProjectedTextures
		{
			static constexpr std::array<std::uint32_t, 4> kSlots{ 3, 8, 10, 11 };
			std::array<ID3D11ShaderResourceView*, 4> views{};
			bool valid = false;
		};
		/**
		 * @brief The frame's globals the ports read (RefreshFrameConstants, at Prepass): no column of the tables, a
		 * capture of the frame each commit latches into its frame blocks.
		 */
		struct FrameCapture
		{
			// The frame's lighting (DirLightDirection, DirLightColor, DirectionalAmbient, AmbientSpecularTintAndFresnelPower):
			// FrameLighting's rows (LightingConstants.h), which the DCLF_BINDLESS draws read from their own frame block (PS b13)
			// instead of each pipeline's PerGeometry block. Merged from the frame's evaluations over the last frame's (a component
			// none of them writes keeps its value); the pipelines' geometryConstants still hold the values, for the constant-buffer
			// path and the parity checks, but a change of them alone versions no pipeline.
			std::array<float, 24> lighting{};
			// The frame's fog (FrameFog, LightingConstants.h): what the DCLF_BINDLESS vertex stage reads (VS b13) instead of each
			// technique row's, which keep the fog they were made with.
			std::array<float, 12> fog{};
			// The character light's noise this frame (MaterialSources::FrameCharacterLightView, from the frame's FrameGlobals sample at
			// Prepass): the frame record's kCharacterLightRegister. The last one kept while the sample has none (a cell loading).
			ID3D11ShaderResourceView* characterLightView = nullptr;
		};
		struct PublicationStats
		{
			// Publications installed with an adopted snapshot (and those the builder skipped past), frames whose snapshot brought no newer one.
			std::uint64_t installed = 0, kept = 0, skipped = 0;
			// The frames from a publication's commit to its installation, by the geometries it joined (kAgeBuckets).
			std::array<std::uint64_t, 8> installDelay{};
		};
		struct GateReleaseStats
		{
			std::uint64_t released = 0, refused = 0, withdrawn = 0;
			std::vector<double> flipToReleaseMs, requestToReleaseMs;
			std::vector<std::uint32_t> flipToReleaseFrames;
			bool any = false;
		};
		/**
		 * @brief Render thread, at Present and the frame's start (in menus too): whether a load screen is up. Nothing may walk the
		 * scene graph while one is: the first time a load is seen a marker is posted (the coordinator drops what the load invalidates:
		 * ApplyLoading; its passes drain the queues into the mirror's carry, the tracking discarding them), the set is withdrawn
		 * (PassCapture::PublishSet(nullptr)), and the first frame after the load catches every switch up and captures every category
		 * node. Also where the switch events' thread is recorded (PushSwitch).
		 */
		bool NoteLoadingScreen();
		/**
		 * @brief Render thread, the frame's start, not while a load screen is up: the coordinator's requests served and the render
		 * thread's captures posted - the switch catch-ups (engine writes: the attached roots and switch events the passes drained, the world
		 * after a load), the category capture (when its signature moved or a pass saw a detach), the mirror's capture requests (captured
		 * with their chains, onto the tracker's stack, mirror-only), the mirror parity's probe (CS_DCLF_MIRROR_PARITY), tree LOD's mirror.
		 * a_running false (DCLF does not run: no frame start of its own): the switch catch-ups and tree LOD's mirror alone, as Present's
		 * ingestion served them before.
		 */
		void ServeFrameRequests(bool a_running);
		/**
		 * @brief NiSwitchNode::OnVisible's catch-up (AE 0x140d29700), outside the cull: when the selected child has not
		 * been updated since the switch's last update pass (childRevID[index] != revID), its revision is marked current
		 * and it takes UpdateDownwardPass with the switch's saved time. True when it ran. Render thread, the node in the
		 * scene: engine code that writes the subtree's transforms, so never the scene work's (CatchUpSwitches).
		 */
		static bool CatchUpSwitch(RE::NiSwitchNode& a_switch);
		/**
		 * @brief Render thread, at ingestion: the catch-ups the scene work used to run itself (ApplySwitchEvents,
		 * AddSubtree), for what the drain brought - every switch event whose selection changed and every switch under an attached
		 * subtree, each in the world (reached from Main::WorldRootNode: a subtree a loader still assembles is not walked) - and,
		 * on the first ingestion after a load, every switch in the world (the rescan's AddSubtree did that). Before the culls
		 * when called from the frame's start.
		 */
		void CatchUpSwitches(std::span<RE::NiAVObject* const> a_attached, std::span<const Scene::SwitchEvent> a_switches);
		/**
		 * @brief PrimaryCull, before the list jobs (render thread): the switch nodes whose selection the walks applied since
		 * the last call, as keys (never dereferenced). True when the caller must read every switch again instead (a full
		 * walk, dropped events, or more changes than the list keeps).
		 */
		bool TakeSwitchChanges(std::vector<const RE::NiAVObject*>& a_out);
		/**
		 * @brief Whether a load screen is up, i.e. the scene graph is being rebuilt under us.
		 *
		 * Nothing may walk the scene graph while this holds, and frame counters meant to be comparable
		 * between runs should not advance across it.
		 */
		static bool IsLoadingScreenUp();
		/**
		 * @brief Latches the main camera's accumulator, from a point in the frame where it is identifiable.
		 *
		 * Call where `globals::game::currentAccumulator` is set - it is a *currently rendering* pointer, so
		 * it is null before the main pass begins even though the accumulator itself has been fully built
		 * since before the shadow maps. BuildFrame runs earlier than that now and reads the latch instead.
		 * A change of accumulator is logged once; the pointer is stable in practice.
		 */
		void LatchAccumulator();
		/**
		 * @brief Re-evaluates the per-pipeline per-frame constants against the main camera's state.
		 *
		 * BuildFrame runs at EarlyPrepass so both DCLF epochs share a table generation, but there the
		 * renderer's shadow state still belongs to the shadow-map camera just drawn. Most of what
		 * SetupGeometry writes is per frame, and one of those - EyePosition - is relative to posAdjust,
		 * so evaluating it that early produced the shadow camera's eye and capture parity failed on that
		 * one variable across every draw. The object, geometry and material tables are camera-independent
		 * and stay where they are built; only this is deferred.
		 *
		 * Call from Prepass. The Z-prepass epoch runs in between and so uses the previous frame's values
		 * for these - harmless only for what the vertex position does not depend on: World is patched per
		 * object at epoch time with that epoch's own eye. Terrain LOD's HighDetailRange moves vertices: it is a technique
		 * row's (the coordinator's), so both epochs of a frame draw the one value of the frame's tables.
		 *
		 * The technique rows are not evaluated here (RefreshTechniqueRows); the frame's fog is, from the frame's sample.
		 */
		void RefreshFrameConstants();
		void NoteProjectedTextures();
		/**
		 * @brief The tables as the frame reads them: the installed publication's (immutable), the newest taken while none is
		 * installed, empty before the first (never the coordinator's). Render thread and the frame's builds.
		 */
		const Tables& GetTables() const { return View(); }
		/** @brief The frame's per-frame engine values (FrameTables): render thread, and the frame's builds kicked after its writes. */
		const FrameTables& GetFrameTables() const { return frameTables; }
		/** @brief Render thread, the frame's start: the frame's values sized and keyed to the frame's tables, before any build. */
		void SyncFrameTables()
		{
			const Tables& view = View();
			frameTables.SyncPipelines(view.pipelines, view.pipelineBindingVersion, frameTablesGeneration);
		}
		/**
		 * @brief Render thread, EarlyPrepass: the joins' frame inputs posted (LightLimitFix's room map, latest wins: PostRoomMap), the
		 * capture drained (DrainCapture) and the observers of the frame's registrations told (Diagnostics::RegistrationsDone). No scene
		 * work: the next scene pass takes the inputs (TakeAccumulateInputs).
		 */
		void PostAccumulateInputs();
		/**
		 * @brief Render thread, Prepass (RefreshFrameConstants): the frame's pipeline sample with a sun, posted latest-wins for the
		 * coordinator's new pipelines' PerGeometry blocks (MakeNewPipelineConstants).
		 */
		void PostPipelineFrame(const GeometryPort::PipelineFrame& a_frame);
		/** @brief Render thread, after HandOverAtFrameStart and the coverage decision: the installed publication's claims made the frame's. */
		void InstallClaims();
		/**
		 * @brief Any thread: the frame's claims - the installed publication's set (InstallClaims), an empty set after a withdrawal
		 * (WithdrawSet), null during a load or before the first. DCLF's own readers of the frame's set take it here; the registration hooks'
		 * copy (PassCapture::PublishSet, the same object) is the engine-facing cut alone.
		 * Immutable and swapped whole.
		 */
		std::shared_ptr<const SetSnapshot> FrameClaims() const { return std::atomic_load(&frameClaims); }
		/** @brief Whether a publication is installed (none: the frame has no claims, WithdrawSet). */
		bool HasInstalled() const { return installed != nullptr; }
		/** @brief The toggles generation the installed publication's commit was made under (the toggle reinstall), 0 without one. */
		std::uint32_t InstalledToggles() const { return installed ? installed->togglesGeneration : 0u; }
		/** @brief The LOD gates' forced-release generation the installed publication was built for, and the frame's (BeginFrame's read). */
		std::uint64_t InstalledLodForced() const { return installed ? installed->lodForcedGeneration : 0u; }
		std::uint64_t FrameLodForced() const { return frameLodForced; }
		/**
		 * @brief Render thread, the frame's start after InstallClaims or WithdrawSet (before the culls): the LOD gates the installed
		 * publications flipped, released (LodGates::Flip: the incoming blocks shown and the outgoing hidden in the frame whose claims are
		 * post-swap; a withdrawn frame's too, every later publication being post-swap), then the coordinator woken for their outcomes. Under
		 * the set or persistent parity, the installed claims checked against each flip (<- GATE HOLE, <- GATE COVERAGE).
		 */
		void ReleaseGates();
		PublicationStats TakePublicationStats() { return std::exchange(publicationStats, PublicationStats{}); }
		/**
		 * @brief Render thread, the frame's start (no coordinator state read; the join is not needed): a_adopted, the publication of
		 * the snapshot the frame adopted (IndirectDraws::AdoptSnapshot; null: none yet), installed when newer than the installed one - the
		 * publication log walked up to it (the frame inputs: the placement plans, shading names, seed requests, switches applied, retired
		 * imports, the actors' wetness membership; then the claims' changes), its tables, lookups and catalog the frame's (DrawPipelines::
		 * HoldCatalog), the replaced one back to the coordinator - and the sun and light candidates the installed one's, and the held
		 * PrimaryCull notes delivered. What goes to the coordinator (the fade roots the depth commit holds, PrimaryCull's fade ownership and
		 * reseed, the pipeline blocks) is posted where it is made and taken at the coordinator's next pass (TakeCoordinatorInputs).
		 */
		void HandOverAtFrameStart(std::shared_ptr<const void> a_adopted);
		const std::shared_ptr<const Tables>& AcceptedTables() const { return acceptedTables; }
		/**
		 * @brief Render thread, the depth commit: its fade root rows hold a_held (Tables::fadeRootsJournal's version), posted latest-wins:
		 * at its next pass the coordinator's journal forgets what it has, and the next write opens a new version.
		 */
		void FadeRootsSent(std::uint64_t a_held)
		{
			// No wake: the frame's next pass takes it.
			channels.fadeRootsSentPosted.store(a_held, std::memory_order_release);
		}
		/** @brief Render thread: the frame's capture (latched by every commit). */
		const FrameCapture& GetFrameCapture() const { return frameCapture; }
		/**
		 * @brief The frame's tables' generation (the publication's they are, 0 before the first): the coordinator's moves whenever
		 * the slot tables are reset or every cached verdict is dropped (InvalidateVerdicts), and a publication carries the one it was made
		 * at. Render thread; the coordinator reads its own (tablesGeneration).
		 */
		std::uint32_t GetTablesGeneration() const { return frameTablesGeneration; }
		/** @brief The sun entries DCLF can take out of the cascade culls (UpdateSunCandidates), and their generation now. */
		std::shared_ptr<const SunCandidates> GetSunCandidates() const { return frameSunCandidates; }
		/**
		 * @brief The entries DCLF can take out of the point lights' culls (UpdateLightCandidates), and their generation now:
		 * the light entries (LightEntryOf: an actor's root, a reference's, a terrain block's multibound node) whose every
		 * tracked geometry is a table object (the paraboloid exclusion then asks that it be drawn in the mode:
		 * BuildSunExclusion), or one the light's registration takes nothing from (LightEntryAllows). The lights' bits of their
		 * geometries are LocalLightCull's.
		 */
		std::shared_ptr<const SunCandidates> GetLightCandidates() const { return frameLightCandidates; }
		/**
		 * @brief Render thread: a count of the light entries that gained their first tracked geometry. An entry the point
		 * lights' filter cut for holding none (LocalLightCull) is judged again when it moves.
		 */
		std::uint64_t GetLightEntriesAppeared() const { return frameLightEntriesAppeared; }
		/**
		 * @brief The frame (the point lights' culls): whether a node is a light entry (LightDependentsOf's rule: a key with tracked
		 * geometries, not a category node). The render thread's set, kept from the publication log's changes up to the newest
		 * publication taken (written only at the frame's start); never the coordinator's.
		 */
		bool IsLightEntry(const RE::NiAVObject* a_node) const
		{
			return frameLightEntrySet.contains(a_node) && !IsCategoryNode(a_node);
		}
		/** @brief The frame: whether a node is one of the cells' category nodes DCLF tracks (RefreshCategoryNodes; the newest publication's set). */
		bool IsCategoryNode(const RE::NiAVObject* a_node) const
		{
			return frameCategoryNodes && frameCategoryNodes->contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_node)));
		}
		/**
		 * @brief The frame's lookups (Lookups.h), read by its epochs and diagnostics: the installed publication's, the newest published
		 * while none is installed (as the frame's tables), empty before the first. Made by the scene lane, immutable.
		 */
		const Lookups& GetLookups() const { return frameLookups ? *frameLookups : EmptyLookups(); }
		static const Lookups& EmptyLookups()
		{
			static const Lookups empty;
			return empty;
		}
		/** @brief Whether the slot is a resident record in the frame's tables (their residentSlot). */
		bool IsMember(std::int32_t a_object) const
		{
			const auto& resident = View().residentSlot;
			return a_object >= 0 && static_cast<std::size_t>(a_object) < resident.size() && resident[a_object] != 0;
		}
		/**
		 * @brief Render thread, BeginSceneFrame, in place of ApplySet: a frame no scene revision covers (none selected yet, or its
		 * recordings are of the graph before a build: IndirectDraws::DecideCoverage) has no claims - every one taken back, as if
		 * every member left (the records, the engine's claims, PrimaryCull's admission, the sun exclusion), so the engine draws
		 * everything and DCLF's epochs draw nothing. The commits go on deciding; the next ApplySet applies the whole set again.
		 */
		void WithdrawSet();
		/** @brief Whether the frame's claims were withdrawn (WithdrawSet) and not applied since. */
		bool SetWithdrawn() const { return setWithdrawn; }
		/**
		 * @brief The frame's start (render thread): the frame number, the frame's globals captured, the frame inputs posted for the
		 * coordinator (a message that wakes its pump; nothing is kicked or joined).
		 *
		 * Render thread, in order: BeginFrame, the newest snapshot adopted and its publication installed, its claims installed, the
		 * coordinator's requests served and the captures posted (ServeFrameRequests), what the frame's claims need (the filters). The
		 * frame reads only its own state, the immutable publications and the log; what the passes have for other modules is a message:
		 * PrimaryCull's hidden keys and lost members (primaryNotes, delivered at the frame's start), the claims' changes and the frame
		 * inputs (the publication log), the engine references they let go of (released at Present: EngineReleases).
		 */
		void BeginFrame();
		/**
		 * @brief EarlyPrepass, render thread: the material tail: the material captures the last joins asked for
		 * (ServeMaterialRequests, answered for the next scene pass), the material writes and the validation slice. The registrations and
		 * the frame inputs are PostAccumulateInputs'. Reads nothing of the coordinator's (the scene pass may run).
		 */
		void PrepareAccumulatePhase();
		/** @brief The set's phases of an object slot (SetPhase bits), 0 when it is not a member. Render thread, or any thread between commits. */
		std::uint8_t SetPhasesOf(std::int32_t a_object) const
		{
			const auto& phases = View().setPhases;
			return a_object >= 0 && static_cast<std::size_t>(a_object) < phases.size() ? phases[a_object] : std::uint8_t{ 0 };
		}
		/**
		 * @brief How many objects take part in an occlusion map's phase this frame without being its members (not ready): the engine
		 * must then register the map's scene, and its registration withholds the members (PassCapture). 0: every occluder DCLF knows
		 * is drawn by DCLF, and the engine's cull of the map can be skipped.
		 */
		std::uint32_t SetLacking(std::uint8_t a_phase) const
		{
			return a_phase == kSetOccluderSky ? frameLackingCount[0] : a_phase == kSetOccluderPrecipitation ? frameLackingCount[1] : a_phase == kSetReflection ? frameLackingCount[2] : 0u;
		}
		/** @brief Render thread (PrimaryCull): posted latest-wins, applied at the coordinator's next scene pass (TakeCoordinatorInputs). */
		void SetFadeRootsOwned(const std::vector<OwnedFadeRoot>& a_owned)
		{
			// No wake: the frame's next pass takes it.
			channels.fadeOwnedPosted.Post(std::make_unique<std::vector<OwnedFadeRoot>>(a_owned));
		}
		/** @brief Tree LOD's mirror of the engine's groups (TreeLod.h): the depth commit uploads what it changed. Render thread. */
		TreeLod::Mirror& TreeLodMirror() { return treeLod; }
		/**
		 * @brief Render thread: whether the installed publication claims the geometry, so it is alive while the frame holds that
		 * publication (its tables name it: the retirement chain keeps what a held publication names). PrimaryCull's liveness check for the
		 * claims' changes (NoteSetChanges), in place of the coordinator's tracked set.
		 */
		bool HeldByInstalled(const RE::BSGeometry* a_geometry) const
		{
			return installed && installed->claims && installed->claims->phases.contains(a_geometry);
		}
		/**
		 * @brief Render thread, at an execution's commit: the import leases of the geometry slots cleared since the last call, for
		 * the execution to hold until the GPU retires it. The frames already submitted read those buffers by their device
		 * addresses, which DXVK does not see: a lease released at once let it free the buffer and give its memory to the next one
		 * the game created (a LOD chunk streamed in after a teleport), so an in-flight frame's colour pass drew other vertices
		 * than its Z-prepass had. Any execution submitted after the clear completes after every one before it (one queue).
		 */
		std::vector<std::shared_ptr<const void>> TakeRetiredImports() { return std::exchange(frameRetiredImports, {}); }
		/** @brief Render thread: a GPU object the frame's executions may still read, released with the next main commit's (TakeRetiredImports). */
		void RetireImport(std::shared_ptr<const void> a_owner) { frameRetiredImports.push_back(std::move(a_owner)); }
		/** @brief Render thread, the frame's start: the seeds requested since the last take (the publication log's, HandOverAtFrameStart). */
		std::vector<FadeSeedItem> TakeFadeSeeds() { return std::exchange(frameFadeSeeds, {}); }
		std::vector<TreeSeedItem> TakeTreeSeeds() { return std::exchange(frameTreeSeeds, {}); }
		/** @brief Render thread: the frame's engine globals (FrameValues binds them for the seeds' LOD scale). */
		const std::shared_ptr<const FrameGlobals>& FrameGlobalsOfFrame() const { return frameGlobals; }
		/** @brief Render thread, the frame's start: the slots named since the last take, once (the publication log's). */
		std::vector<ShadingItem> TakeShadingItems() { return std::exchange(frameShading, {}); }
		/**
		 * @brief Render thread, at the frame's start: every actor's wetness (Skin::GetWetness, whose first call a frame advances the
		 * actor's fade and is not thread-safe), fanned out to the meshes whose value changed or that joined. The actors' membership
		 * is the render thread's own index (frameActorWetness), kept from the log's changes; the geometries are the newest publication's.
		 */
		std::vector<WetnessValue> CaptureWetness();
		/**
		 * @brief Render thread, the frame's start: the plans the walks made since the last take, oldest first (the publication log's;
		 * FrameValues samples every one's written slots and draws with the newest).
		 */
		std::vector<std::shared_ptr<const PlacementPlan>> TakePlacementPlans() { return std::exchange(framePlans, {}); }
		/**
		 * @brief Render thread (a message): every cached classification verdict and derivation is to be dropped - a live toggle
		 * that enters the classification (Toggles.h) changed, or the feature came back on - by the coordinator at its next scene pass
		 * (the frame inputs' verdicts generation: InvalidateVerdicts).
		 */
		void RequestVerdictsInvalidated() { ++verdictsRequested; }
		/** @brief Render thread (diagnostics): the slots the publication log holds past the frame's cursor, not taken. */
		std::vector<ShadingItem> PeekShadingItems() const;
		/** @brief Render thread (the frame's start, an inline pass's end): the coordinator's notes to PrimaryCull, delivered. */
		void DeliverPrimaryNotes();

	private:
		Channels& channels = Channels::Get();
		void CaptureCategories(bool a_force);
		/** @brief A cheap hash of everything the category capture reads to find category nodes (its signature). */
		std::uint64_t CategorySignature() const;
		/**
		 * @brief Render thread, the frame's start: what the coordinator found the mirror lacking (Channels::mirrorCaptureRequests, each held by its
		 * request) captured with its chain up to the world's root (SceneCapture::CaptureAttached) and pushed onto the tracker's stack,
		 * mirror-only. The mirror is not read here (the coordinator asked because it lacked the record).
		 */
		void CaptureMirrorRequests();
		/** @brief Render thread, the frame's start (ServeFrameRequests): the fade node requests answered. */
		void ServeFadeNodeRequests();
		/**
		 * @brief Render thread, EarlyPrepass: drains the capture every frame (its fixed-capacity buffer and the withholding counters);
		 * what was registered is the observers' alone (Diagnostics::CaptureDrained: the registration parity, the residue, the decal order
		 * probe). The normal path reads none of it.
		 */
		void DrainCapture();
		/** @brief Render thread, the frame's start (BeginFrame, the coordinator idle): the engine's BSLightingShader instance, once. */
		void CaptureLightingShader();
		/** @brief Render thread (PostAccumulateInputs): LightLimitFix's room map copied and posted when its generation moves. */
		void PostRoomMap();
		/** @brief Render thread, after BeginFrame's capture: the frame's inputs posted for the coordinator. */
		void PostFrameInputs();
		/**
		 * @brief Render thread, Prepass: the character light's view this frame (FrameCapture::characterLightView) from the frame's sample
		 * (MaterialSources::FrameCharacterLightView). The records' frame components are the scene work's (RefreshMaterialSignatures).
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
		/** @brief Render thread: the replaced or skipped publication back to the coordinator, which drops it (Channels::retiredPublications). */
		void ReturnPublication(std::shared_ptr<const ScenePublication>&& a_publication);
		/**
		 * @brief Render thread, EarlyPrepass (a frame input): the captures the joins asked for (Channels::materialRequests),
		 * each read off the property the join held, onto MaterialPort::captures; every request answered (Channels::materialRequestsAnswered).
		 */
		void ServeMaterialRequests();
		/** @brief Render thread: the installed publication, null before the first. */
		std::shared_ptr<const ScenePublication> Newest() const { return installed; }
		FrameCapture frameCapture;  // GetFrameCapture
		FrameTables frameTables;    // GetFrameTables: the frame's values, out of the tables (render thread)
		std::shared_ptr<const CategoryCapture> categoryCapture;  // render thread: the newest made
		// Render thread: the captures made and not yet superseded by one the coordinator applied, oldest first. Their pins and
		// nodes are dropped here (the render thread's), once the coordinator has let go of its copy and moved categoryAppliedGeneration on.
		std::deque<std::shared_ptr<const CategoryCapture>> categoryCapturesHeld;
		std::uint64_t categoryCaptures = 0;  // render thread: the last capture's generation
		// Render thread, since the last report: the frame captures' time (FrameGlobals, CaptureCategories), the category captures
		// made, and the frames.
		std::uint64_t captureNs = 0, categoryCaptureNs = 0, categoryCapturesMade = 0, captureFrames = 0;
		std::uint64_t categoryMirrorCaptures = 0, categoryMirrorRecords = 0;  // CaptureCategories' captures for the mirror
		// The frame's engine globals (FrameGlobals::Capture at BeginFrame): the render thread's (FrameValues binds them for its seeds).
		std::shared_ptr<const FrameGlobals> frameGlobals;
		std::uint64_t frameLodForced = 0;     // the frame's LodGates::ForcedGeneration (BeginFrame)
		std::uint64_t roomMapPostedGeneration = ~0ull;  // render thread: the generation last posted
		// Render thread: the first frame's start after a load catches up every switch in the world (CatchUpSwitches), and its category
		// capture is forced (as one after a pass saw a detach: categoryDetachSeen).
		bool worldCatchUpPending = false;
		bool categoryCapturePending = false;
		std::vector<RE::NiAVObject*> attachedRoots;  // the ingestion's attached subtrees, for CatchUpSwitches (scratch)
		// The frame's copies (the render thread's), taken at its start from the newest publication taken and the publication log
		// (HandOverAtFrameStart).
		std::shared_ptr<const SunCandidates> frameSunCandidates, frameLightCandidates;
		std::uint64_t frameLightEntriesAppeared = 0;
		std::uint32_t frameTablesGeneration = 0;  // the frame's tables' (GetTablesGeneration)
		std::vector<const RE::NiAVObject*> frameSwitchChanges;
		bool frameSwitchResync = true;
		bool switchResyncNext = false;  // NoteLoadingScreen: PrimaryCull reads every switch again from the next frame's start
		std::vector<std::shared_ptr<const void>> frameRetiredImports;
		std::vector<std::shared_ptr<const PlacementPlan>> framePlans;
		std::vector<ShadingItem> frameShading;
		std::vector<FadeSeedItem> frameFadeSeeds, frameTreeSeeds;
		ankerl::unordered_dense::set<const RE::NiAVObject*> frameLightEntrySet;
		std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> frameCategoryNodes;  // the installed publication's (IsCategoryNode)
		// CaptureWetness's actor index: the membership the coordinator's walks set (Tables::actorWetnessChanges, through the log),
		// the values and the fan-out the render thread's.
		ActorValueIndex frameActorWetness;
		// The main camera's accumulator, latched non-null.
		RE::BSGraphics::BSShaderAccumulator* latchedAccumulator = nullptr;
		// The frame's (render thread, HandOverAtFrameStart): the installed publication's lookups and catalog, the newest published while none
		// is installed.
		std::shared_ptr<const Lookups> frameLookups;
		std::shared_ptr<const PipelineCatalog> frameCatalog;
		std::shared_ptr<const Tables> acceptedTables;
		ProjectedTextures projectedTextures;
		bool loadingPosted = false;  // render thread: the load's marker is posted
		bool rescanCaptureAll = false;  // render thread: a load's marker posted, so the next category capture pins every category node
		// Render thread: the flips of the publications installed since the last ReleaseGates (HandOverAtFrameStart), and the releases'
		// counts and observers.
		std::vector<GateFlip> frameGateReleases;
		// The render thread's: the installed publication (the adopted snapshot's, HandOverAtFrameStart).
		std::shared_ptr<const ScenePublication> installed;
		// The render thread's cursors into the log: the frame inputs taken and the claims' changes taken (both up to the installed one,
		// the claims' never past the frame inputs'). Null: the log's first node.
		PublicationLog<PublicationDeltas>::Node* deltasCursor = nullptr;
		PublicationLog<PublicationDeltas>::Node* notesCursor = nullptr;
		// The frame's: the installed publications' claim changes not told to PrimaryCull yet, whether one is waiting, the claims it was
		// told of, and the lacking counts.
		std::vector<std::pair<std::vector<const RE::BSGeometry*>, std::vector<const RE::BSGeometry*>>> installNotes;
		bool installPending = false;
		std::shared_ptr<const SetSnapshot> notedClaims;
		// FrameClaims (render thread writes through PublishFrameClaims, any thread reads), and the registration hooks told of it too.
		std::shared_ptr<const SetSnapshot> frameClaims;
		std::array<std::uint32_t, 3> frameLackingCount{};
		PublicationStats publicationStats;
		bool setWithdrawn = false;  // the frame's claims (WithdrawSet), until the next installation
		// Tree LOD (dclf-lod.md, L4): the engine's tree LOD groups and their instance records, by events (TreeLod.h).
		TreeLod::Mirror treeLod;
		// The frame sample the held pipeline blocks were made from (RefreshFrameConstants' last with a sun; posted to the coordinator too,
		// PostPipelineFrame).
		std::optional<GeometryPort::PipelineFrame> geometryPortFrame;
		std::uint32_t geometryPortFrameNumber = ~0u;
		// The first RefreshFrameConstants evaluates every pipeline in full and resamples every slot's shading. The full
		// evaluations are what seed the frame lighting (FrameCapture::lighting) with the variables the per-frame sample's
		// pipeline does not write, such as AmbientSpecularTintAndFresnelPower: pipelines made later are evaluated where
		// they are written (WriteObject), which does not publish lighting.
		bool constantsRefreshed = false;
		std::uint32_t verdictsRequested = 0;  // render thread: bumped by each request to drop the cached verdicts
		std::uint32_t frame = 0;
		// The pipelines' PerGeometry blocks (RefreshFrameConstants): full evaluations, frame samples, the pipelines changed and the frame
		// lighting's versions.
		struct GeometryStats
		{
			std::uint64_t frames = 0, full = 0, samples = 0, changed = 0, lightingVersions = 0;
		} geometryStats;
		GateReleaseStats gateReleaseStats;
		// Since the last report: the pipeline blocks posted to the coordinator (PostPipelineConstants), the Prepass frames.
		std::atomic<std::uint64_t> pipelinesPosted{ 0 };  // taken by the coordinator's report
		std::uint64_t shadingFrames = 0;
	};
}
