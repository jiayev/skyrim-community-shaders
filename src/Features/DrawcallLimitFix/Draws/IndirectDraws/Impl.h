#pragma once

// IndirectDraws::Impl: the state of IndirectDraws' threads and the functions over it: the render thread's frame (captures, the
// adopted snapshot and its payload, the epochs' commits), the coordinator's copy of the ahead context, the snapshot builder
// (builds ahead and the revision producer, on the preparation pool) and the report counters. Each member says whose it is.
// The umbrella: Impl's cohesive groups of types and members are its bases, one per header of Impl/ (SnapshotBuilder, Views,
// Producer, Stats); the functions over them, and the rest of the state, are here. Included by Internal.h, last.

namespace DCLF
{
	// The implementation's names resolve here as they do in DCLF::Draws.
	using namespace Draws;
}

#include "Impl/SnapshotBuilder.h"
#include "Impl/Views.h"
#include "Impl/Producer.h"
#include "Impl/Stats.h"

namespace DCLF
{
	struct IndirectDraws::Impl : ImplSnapshotBuilder, ImplViews, ImplProducer, ImplStats
	{
		std::shared_ptr<Resources> resources;
		// What the resources were created for; a change rebuilds them (and the graph).
		TargetFormats formats{};
		std::uint32_t width = 0, height = 0;
		std::optional<Capture> pending;  // this frame's main-pass bindings, until the epoch runs

		// The shadow views (CS_DCLF_SHADOWS): their own resources, apart from the main epochs'.
		std::shared_ptr<ShadowResources> shadow;
		bool shadowSetupFailed = false;
		// The shadow epochs counted for the culling counters' samples (ReadShadowCullCounters: one in 127).
		std::uint32_t shadowCullEpochs = 0;
		std::uint32_t shadowLoggedTargets = 0;
		void ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats);
		using PendingView = Draws::PendingView;
		std::vector<PendingView> pendingViews;
		// Per occlusion view: its view, its pipelines' state and format, and the frame whose commit uploaded its occluders.
		struct OcclusionState
		{
			PendingView view;
			std::uint32_t capturedFrame = ~0u;
			std::uint32_t rasterState = 0;
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
			std::uint32_t committedFrame = ~0u;
			std::uint32_t inputs = 0, skipped = 0;
		};
		std::array<OcclusionState, kOcclusionViews> occlusion;
		/** @brief The occlusion views' formats, for the scene lane's shadow pipelines (PostLookupInputs). */
		std::array<DXGI_FORMAT, kOcclusionViews> OcclusionFormats() const
		{
			std::array<DXGI_FORMAT, kOcclusionViews> viewFormats{};
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				viewFormats[v] = occlusion[v].dsvFormat;
			return viewFormats;
		}
		/** @brief A shadow view this frame's epoch cannot draw: its withheld casters are a hole (flagged); the capability stays. */
		void ShadowsNotDrawn(IndirectDraws::ShadowStats& a_stats);
		/**
		 * The shadow capability: the modes DCLF draws and the state catalog their pipelines are built for; changed only by a toggle,
		 * a failure, an occlusion map's first capture, or a defect ("<- PHASES"). Render thread; the set reads the published phases.
		 */
		std::array<bool, kShadowModeCount> readyModes{};
		std::array<ModeRasterStates, kShadowModeCount> readyStates{};
		std::atomic<std::uint8_t> shadowCapability{ 0 };
		bool shadowCatalogBuilt = false;
		DXGI_FORMAT shadowTargetFormat = DXGI_FORMAT_UNKNOWN;
		/** @brief The catalog: every solid-fill state of the engine's table its shadow views can draw with, and the cascades' clones. */
		void BuildShadowCatalog();
		/** @brief The capability changed: the readiness serial moves and every caster is decided again. a_defect: an unforeseen view. */
		void NoteCapability(bool a_defect, std::string a_cause);
		// The report counters: Impl/Stats.h (ImplStats).
		/** @brief The shadow payload the frame's epoch committed, read by the occlusion epoch until the next commit. */
		std::shared_ptr<ShadowPayload> committedShadow;
		std::vector<std::shared_ptr<ShadowPayload>> shadowPayloadPool;  // the snapshot builder's
		std::shared_ptr<ShadowPayload> AcquireShadowPayload();
		std::shared_ptr<const void> shadowExecutionOwner;  // reused by the sky epoch's copy of the shadow records
		/** @brief The capability's modes and states and the shadow targets' format that the builds ahead and shadow lookups are for. */
		struct LastShadow
		{
			std::array<bool, kShadowModeCount> modes{};
			std::array<ModeRasterStates, kShadowModeCount> rasterStates{};
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
			bool known = false;
		} lastShadow;
		// What the scene lane's shadow pipelines are resolved for, latest wins (postedShadowLookups: the render thread's copy).
		std::atomic<std::shared_ptr<const ShadowLookupInputs>> shadowLookupInputs;
		ShadowLookupInputs postedShadowLookups;
		/**
		 * @brief The shadow views' placements (built at setup): every place the engine's shadow maps can take a view (cascade slices
		 * and their volumetric copies, a light map slice whole or as a paraboloid pair's halves), each with its mode, state and slot.
		 * The shadow epoch's shape is all of them; an empty slot does no work. A view at no placement, under another state or twice in
		 * a frame is a defect (ShadowNotReady::Placement), flagged.
		 */
		std::vector<ShadowViewLayout> shadowPlacements;
		bool BuildShadowPlacements();
		/** @brief The placement a captured view drew at (its target, slice and viewport), or SIZE_MAX. */
		std::size_t PlacementOf(const PendingView& a_view) const;
		std::uint32_t placementDefectsLogged = 0;
		/** @brief The occlusion maps' layouts as last captured: what revisions make occlusion shapes for and OcclusionReady checks. */
		std::vector<ShadowViewLayout> PredictedOcclusion() const;
		static constexpr std::size_t kRecentOcclusionLayouts = 4;
		/** @brief A view rasterizer state's map row's buckets, from a_cache (the revision producer's). */
		static const RowBuckets& ShadowRowBuckets(RowBucketsCache& a_cache, std::uint32_t a_mode, std::uint32_t a_state, const Lookups& a_lookups,
			const ShadowIndirectState& a_indirect);
		/** @brief Revision code: the producer's draw bound brought up to date with a_tables' change log. */
		void UpdateDrawBound(const SceneTables& a_tables, std::uint32_t a_generation);
		/**
		 * @brief Revision code: the main layout (RevisionProducer::mainLayout) brought up to the draw bound, and a_resources' sequences,
		 * bucket counts and rows required to hold it. A bound over the device's max sequence count or the sort's rank field is a hard
		 * failure.
		 */
		void RequireMain(GrowthBuild& a_build, Resources& a_resources, const SceneTables& a_tables, std::uint32_t a_generation);
		/** @brief Revision code: the index pool's room for every geometry the tables have, and a relayout of a fragmented one. */
		void RequireIndexPool(GrowthBuild& a_build, SceneBuffers& a_scene, const SceneTables& a_tables, std::uint32_t a_generation);
		/** @brief Revision code: every shadow view slot's sequences required to hold the scene's draw bound. */
		void RequireShadowSequences(GrowthBuild& a_build, const SceneTables& a_tables, std::uint32_t a_generation);
		/**
		 * @brief Revision code: view slots and map rows grown (more slots rebuild the graph; either growth is a new latch layout), the
		 * view blocks and every slot's bucket counts required to hold them.
		 */
		void ReserveShadowLatch(GrowthBuild& a_build, ShadowResources& a_shadow, std::uint32_t a_views, std::uint32_t a_keys, std::uint32_t a_rasterStates,
			std::uint32_t a_sunProcesses);
		// Revision code: the main latch's layout grown when it holds fewer cascades, volumes or buckets than asked (RequireLatches
		// asks the set for a block of it).
		void ReserveMainLatch(std::uint32_t a_cascades, std::uint32_t a_shadowVolumes, std::uint32_t a_buckets = 0);
		/**
		 * @brief Revision code: every latch block laid out, required of the set as its layout stands (another layout: a new block),
		 * with the zeros the shadow and view latched copies take. Once per make, after every reserve.
		 */
		void RequireLatches(GrowthBuild& a_build);
		ShadowInputs PrepareShadowInputs(const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
			const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const;
		std::uint32_t shadowLoggedReasons = 0;
		bool ShadowNotReady(std::uint32_t a_reason, const char* a_what)
		{
			if (!((shadowLoggedReasons >> a_reason) & 1)) {
				shadowLoggedReasons |= 1u << a_reason;
				logger::info("[DCLF] shadow views not ready: {}", a_what);
			}
			return false;
		}
		bool SetupShadow();
		bool ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target);
		/** @brief Imports a_texture through a_view as depth target a_index (a_label: for the log). */
		bool ImportDepthTexture(std::uint32_t a_index, ID3D11Texture2D* a_texture, ID3D11DepthStencilView* a_view, std::uint32_t a_label);

		// The volatile t16+ inputs keep a live import only while their register keeps naming that SRV: reuse is bounded without an
		// age-based import cache.
		struct FrameTextureBinding
		{
			ID3D11ShaderResourceView* view = nullptr;
			GpuTextures::Binding binding;
		};
		std::array<FrameTextureBinding, kTextureRegisters> frameTextureBindings{};
		std::uint32_t frameTextureGeneration = ~0u;
		// The commits' own uploads on the render thread (CommitUploads).
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>> commitStagedPool;
		BuildCache mainCache;  // the main list's region, both segments' (BuildMainPayloads)
		// The builds ahead and the snapshot builder's types and state: Impl/SnapshotBuilder.h (ImplSnapshotBuilder).
		void PostFit();
		/** @brief Render thread: posts the context and the fits as they are now (a_fit: PostFit first). */
		void PostAhead(bool a_fit = true);

		/**
		 * @brief The addresses, SRV indices, capacities and fit a main (shadow) build embeds of its resources' versioned buffers, as
		 * a_sealed names them (the tree wind read at a_frame: the buffer the frame before wrote), and the main layout's decal range.
		 * Every other field of a_out stands.
		 */
		static void AddressesOf(const NamedVersions& a_sealed, const MainLayout& a_layout, const Resources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out);
		static void AddressesOf(const NamedVersions& a_sealed, const ShadowResources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out);
		/** @brief The snapshot builder's pump (configured once, never destroyed: a function-local static). */
		org::async::SerializedTaskPump& SnapshotPump();
		/** @brief The builder's pass (SnapshotPump's drain): see SnapshotBuilder. */
		void SnapshotPass();
		/**
		 * @brief The builder: the work item's draws (RunAhead), every versioned buffer's address and SRV index they embed of the version
		 * a_sealed names (AddressesOf, a_layout's decal range; none without a set: such draws draw nothing).
		 */
		void BuildSnapshotDraws(const NamedVersions* a_sealed, const MainLayout* a_layout);
		/**
		 * @brief The builder: the payload of its draws (PayloadBuilder::Publish), kept when the draws are (a snapshot made again for
		 * them uploads nothing); its uploads' completion wakes the builder. Published only for a snapshot the frame will adopt: none
		 * while the last snapshot posted is still in the slot, nor before the scene pass of the frame that took it has ended (a
		 * publication of that pass replaces these draws first); the frame's take and the pass's end wake the builder.
		 */
		void PublishPayload();
		/** @brief The builder: its draws have something to draw, so their snapshot needs a payload (PublishPayload). */
		bool PayloadWanted() const;
		/** @brief The builder: posts the snapshot of its work item, draws and the completed revision a_revision. */
		void PostSnapshot(org::async::RevisionAssembler::Lease a_revision);
		/** @brief The builder's report lines (counters reset): revisions, growths, shapes made, the draw bound. */
		std::string ProducerReport();
		/** @brief The builder's report lines of the stores its builds write (main rows, object records, shadow state, extras, geometry, exclusions), reset. */
		std::string StoresReport();
		/** @brief The frame's report lines of the snapshots, and the builder's last. */
		std::string SnapshotReport();
		/** @brief The snapshot builder: a publication's stream views and payloads (on the pool, in publication order). */
		std::shared_ptr<const DrawPublication> RunAhead(std::shared_ptr<const SceneTables> a_tables, const AheadContext& a_context, const Lookups& a_lookups,
			std::uint32_t a_generation, std::uint32_t a_frame, std::shared_ptr<const SunCandidates> a_sunCandidates, std::shared_ptr<const SunCandidates> a_lightCandidates);
		/** @brief Waits until no build ahead runs (DrainAsync; never per frame). */
		void WaitAhead();
		/** @brief Waits for the fade write-back task (DrainAsync; never per frame). */
		void WaitFadeWriteBack();
		/** @brief The depth commit: harvests the frame slot's last fade events for the write-back task, resets the list, arms the slot. */
		void ArmFadeReadback(SceneBuffers& a_scene, CommitUploads& a_uploads);
		/** @brief As ArmFadeReadback for the visibility events (the reported words zeroed once per backing). */
		void ArmVisibilityReadback(SceneBuffers& a_scene, CommitUploads& a_uploads);
		/** @brief Waits for the visibility decode task (DrainAsync; never per frame). */
		void WaitVisibilityReadback();
		std::shared_ptr<const DrawPublication> installedDraws;  // the frame's
		/** @brief The installed tables' pipeline slots; a covering revision was made at or after its commit, so its latch holds them. */
		std::uint32_t InstalledPipelineSlots() const
		{
			return installedDraws && installedDraws->tables ? static_cast<std::uint32_t>(installedDraws->tables->pipelines.size()) : 0u;
		}
		// The scene buffers and their stores, every epoch's: the builds ahead run one at a time, in publication order.
		std::shared_ptr<SceneBuffers> scene;
		// The frame's tree LOD decision (DecideTreeLod), which the depth commit draws on.
		bool treeLodOwned = false;
		// The views' frame state: Impl/Views.h (ImplViews).
		/** @brief A view's graph resources, created once its targets are known (render thread). */
		bool SetupView(ViewKind a_kind);
		/** @brief Revision code: a view's latch layout for a_slots slots and a_buckets buckets, and its buffers required to hold them and a_draws a face. */
		void RequireView(GrowthBuild& a_build, ViewResources& a_view, std::uint32_t a_slots, std::uint32_t a_buckets, std::uint32_t a_draws);
		/** @brief The engine's kPOST_ZPREPASS_COPY imported (again when it changes), and the extension added. */
		bool ImportFirstPersonDepth();
		/** @brief The reflection phase's readiness of an object: its pipeline slot's forward pipeline is built. */
		bool ReflectionPhaseReady(const SceneTables& a_tables, std::uint32_t a_slot) const;
		/** @brief The engine's cube target imported (again when it changes). */
		bool ImportReflectionCube(ID3D11Texture2D* a_texture);
		/** @brief A view's commit (render thread): its map, faces and tree LOD rows into the shape's latch; returns faces drawn. */
		std::uint32_t CommitView(const ViewFrame& a_shape, std::span<const ViewFace> a_faces, std::uint32_t a_inputs, const TreeLod::DrawRow* a_treeRow);
		/** @brief What an epoch's execution holds besides its shape (binding owners, the adopted snapshot). */
		std::shared_ptr<std::vector<std::shared_ptr<const void>>> EpochOwners(std::span<const MainPayload* const> a_payloads);
		/** @brief The scene buffers, created once (render thread). False when they have no device address. */
		bool EnsureSceneBuffers(rhi::Device a_device);
		/**
		 * @brief Revision code: the scene's tables required to hold a_tables (and the frame's posted counts), with the per-object
		 * buffers (the objects and the input demand) and the faces' tree lists.
		 */
		void RequireSceneTables(GrowthBuild& a_build, SceneBuffers& a_scene, const SceneTables& a_tables);
		ObjectRecordStore objectStore;
		ExtrasStore extrasStore;
		GeometryStore geometryStore;
		std::shared_ptr<MainPayload> AcquirePayload()
		{
			// An idle payload holds nothing: its stream views, tables and owners would keep the retirement chain from that
			// publication on.
			std::shared_ptr<MainPayload> free;
			for (auto& payload : payloadPool)
				if (payload.use_count() == 1) {
					payload->Reset();
					if (!free)
						free = payload;
				}
			return free ? free : payloadPool.emplace_back(std::make_shared<MainPayload>());
		}
		// The payloads the frame's main epochs committed, by job (the forward and first-person epochs draw the colour one's).
		std::array<const MainPayload*, 2> committedPayload{};
		std::array<std::uint32_t, 2> committedFrame{};
		MainRows mainRows;  // both main segments' (MainRows)
		ShadowKept shadowKept;  // the shadow epoch's inputs and records
		ShadowKept* ShadowKeptState() { return &shadowKept; }
		SunExclusionCache sunExclusionCache;  // the shadow builds', in frame order
		SunExclusionCache parabolicExclusionCache;  // likewise, the paraboloid mode's (LocalLightCull)
		std::array<std::uint32_t, 5> decalWords{};  // the count buffer's decal words (19-22, then 27), uploaded per colour epoch

		/** @brief An epoch's per-frame constant blocks from the capture's mirrors (render thread; keeps the Z-prepass's bytes). */
		void PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, FrameBlocks& a_out);
		// Per main epoch (Z-prepass, colour): each frame slot's last captured block, for a frame that lacks it.
		struct CarriedBlocks
		{
			std::array<std::vector<std::byte>, kConstantBufferRegisters> vs, ps;
		};
		std::array<CarriedBlocks, 2> carriedBlocks;
		std::uint32_t carriedLogged = 0;      // the slots logged once (bit: slot, +8 colour, +16 pixel)
		/** @brief The build's inputs, snapshotted on the render thread. Without a capture (a build ahead) the eye is the replayed one. */
		MainInputs PrepareMainInputs(bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask);

		// Per main epoch (kAsyncColour, kAsyncZPrepass): the frame slots it last supplied, which the builds ahead are made for.
		struct EpochMasks
		{
			std::uint32_t vsMask = 0, psMask = 0;
			bool known = false;
		};
		std::array<EpochMasks, 2> epochMasks;
		/** @brief Inside the epoch's preparation: the frame textures and blocks, the uploads, the PassFrame. */
		bool CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, const MainInputs& a_frame, MainPayload& a_payload,
			const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats,
			std::vector<std::shared_ptr<const void>>& a_bindingOwners, const EpochRevision& a_revision);

		// The vertex-stage inputs the Z-prepass wrote its depth with: the colour pass tests EQUAL against it, and the engine rewrites
		// the per-frame constants between the depth pass and the composite, so the colour pass reuses these bytes.
		std::array<std::vector<std::byte>, kConstantBufferRegisters> prepassVS;
		// The main pass's viewport depth range (from the last capture; stable frame to frame): the Z-prepass writes depth with it so
		// the main pass's EQUAL test matches.
		float mainMinDepth = 0.0f, mainMaxDepth = 0.0f;
		bool prepassInputs = false;

		winrt::com_ptr<ID3D11Texture2D> mainPassDepth;  // what the main pass binds, to check the depth pass against
		bool loggedDepthMismatch = false;
		// Colour commits whose sun test ran without the sun's cascades, over the report window (Reports::ColourSunTestLine).
		Reports::Window sunTestWindow;
		std::uint32_t sunTestEmpty = 0;
		bool loggedFrameConstants = false;

		// The sun cascades the colour commit's BuildDraws tests against (its latch's).
		std::vector<SunAccumulation::GpuCascade> sunCascades;
		// The local shadow lights its BuildDraws selected against (LocalShadowLights), and their volumes as uploaded.
		LocalShadowLights localShadows;
		std::vector<GpuShadowVolume> shadowVolumes;
		// The depth commit's scratch: its buckets (PlanZBuckets) and the zeros their count words take.
		ZBucketPlan zBucketPlan;
		std::vector<std::uint32_t> zBucketZeros;

		// The revision producer's and the scene revisions' types and state: Impl/Producer.h (ImplProducer).
		void ServeRevisionRequests();
		/** @brief Render thread: posts the revision inputs when anything moved. Called where each value is captured. */
		void PostRevisionInputs();
		/**
		 * @brief The snapshot builder: a scene revision's shapes, reserved and made from a_publication and the producer's inputs
		 * alone, then assembled. a_frame: the publication's commitFrame.
		 */
		void MakeRevision(const ScenePublication& a_publication, std::uint32_t a_frame);
		/** @brief Revision code: the latch blocks' handles registered (once), and each latch laid out at its first sizes once its resources are. */
		void EnsureRevisionLatches();

		/** @brief The builder: seals the revision of MakeRevision's shapes and sealed set; none while a needed build is pending. */
		void AssembleRevision(std::uint32_t a_frame);
		/**
		 * @brief The selected revision's shape and recordings for a_epoch, trusted. Empty (counted, <- UNREVISED) only when a coverage
		 * decision let a frame through without them; the epoch is then not submitted.
		 */
		EpochRevision RevisionOf(std::uint32_t a_epoch);
		/**
		 * @brief Whether the selected revision covers a_epoch and its graph still runs. An uncovered epoch is the engine's, decided
		 * before the engine draws, and not submitted. True without revisions.
		 */
		bool EpochCovered(std::uint32_t a_epoch) const;
		void SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index);
		std::string RevisionReport();
		ShapesKey RevisionShapesKey(const NamedVersions& a_sealed, const IndirectState& a_indirect, const ShadowIndirectState& a_shadowIndirect, const Lookups& a_lookups,
			std::size_t a_pipelines, const TreeLodPipelines* a_treeLod) const;
		/**
		 * @brief Whether the last shadow and occlusion shapes hold a_bounds (capacities and buckets enough, no row at the bucket words
		 * they were made for). Capacities only grow, so a shape remade from a bound they hold is the same shape.
		 */
		bool ShadowShapesHold(const ShadowBounds& a_bounds) const;

		std::uint32_t loggedReasons = 0;

		bool NotReady(std::uint32_t a_reason, const char* a_what)
		{
			if (!((loggedReasons >> a_reason) & 1)) {
				loggedReasons |= 1u << a_reason;
				logger::info("[DCLF] Main-pass draws not ready: {}", a_what);
			}
			return false;
		}

		bool Setup(const Capture& a_capture, bool a_depthOnly = false);
	};
}
