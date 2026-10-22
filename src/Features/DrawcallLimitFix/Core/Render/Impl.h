#pragma once

// IndirectDraws::Impl: the state of IndirectDraws' threads and the functions over it: the render thread's frame (captures, the
// adopted snapshot and its payload, the epochs' commits), the coordinator's copy of the ahead context, the snapshot builder
// (builds ahead and the revision producer, on the preparation pool) and the report counters. Each member says whose it is.
// The umbrella: Impl's cohesive groups of types and members are its bases, one per header of Impl/ (SnapshotBuilder, Views,
// Producer, Stats); the functions over them, and the rest of the state, are here. The shadow and occlusion views' state is the
// shadow engine's (shadowEngine: Shadow/ShadowEngine.h), which reaches Impl through ShadowHost alone. Included by Internal.h, last.

namespace DCLF
{
	// The implementation's names resolve here as they do in DCLF::Draws.
	using namespace Draws;
}

#include "Features/DrawcallLimitFix/Core/Render/Epochs/ExecutionOwners.h"
#include "Features/DrawcallLimitFix/Core/Render/Impl/SnapshotBuilder.h"
#include "Features/DrawcallLimitFix/Core/Render/Impl/Views.h"
#include "Features/DrawcallLimitFix/Core/Render/Impl/Producer.h"
#include "Features/DrawcallLimitFix/Core/Render/Impl/Stats.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Participant.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEngine.h"

namespace DCLF
{
	namespace Draws
	{
		struct MainCommit;  // CommitMainPayload's shared locals (Payload/Uploads.cpp)
	}

	struct IndirectDraws::Impl final : ImplSnapshotBuilder, ImplViews, ImplProducer, ImplStats, ShadowHost
	{
		std::shared_ptr<Resources> resources;
		// What the resources were created for; a change rebuilds them (and the graph).
		TargetFormats formats{};
		std::uint32_t width = 0, height = 0;
		std::optional<Capture> pending;  // this frame's main-pass bindings, until the epoch runs

		// The shadow and occlusion views' state and the functions over it alone: the shadow engine's (Shadow/ShadowEngine.h).
		std::unique_ptr<ShadowEngine> shadowEngine = std::make_unique<ShadowEngine>(*this);
		// The report counters: Impl/Stats.h (ImplStats).
		/**
		 * @brief Revision code: the main layout (RevisionProducer::mainLayout) brought up to the draw bound, and a_resources' sequences,
		 * bucket counts and rows required to hold it, with the colour segment's state class sub-ranges of a_make's set version
		 * (a_make.classPlan) and their counts. A bound over the device's max sequence count or the sort's rank field is a hard failure.
		 */
		void RequireMain(GrowthBuild& a_build, Resources& a_resources, RevisionMake& a_make);
		/**
		 * @brief RequireMain's stage: the colour segment's state class sub-ranges (PlanSetClasses) of the layout as it stands, the decal
		 * sub-ranges it holds grown to them, their counts and the latch's class table and map reserved.
		 */
		void RequireMainClasses(GrowthBuild& a_build, Resources& a_resources, RevisionMake& a_make);
		/** @brief Revision code: the index pool's room for every geometry the tables have, and a relayout of a fragmented one. */
		void RequireIndexPool(GrowthBuild& a_build, SceneBuffers& a_scene, const SceneTables& a_tables, std::uint32_t a_generation);
		// Revision code: the main latch's layout grown when it holds fewer cascades, volumes, buckets, state classes or class map words
		// than asked (RequireLatches asks the set for a block of it).
		void ReserveMainLatch(std::uint32_t a_cascades, std::uint32_t a_shadowVolumes, std::uint32_t a_buckets = 0, std::uint32_t a_classes = 0,
			std::uint32_t a_classMapWords = 0);
		/**
		 * @brief Revision code: every latch block laid out, required of the set as its layout stands (another layout: a new block),
		 * with the zeros the shadow and view latched copies take. Once per make, after every reserve.
		 */
		void RequireLatches(GrowthBuild& a_build);

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
		 * @brief Render thread: a_out made from the frame as it is now (the main resources, the epochs' masks, the participants' parts),
		 * under the newest toggles. False without main resources (a_failed, none yet): a_out holds the toggles alone and no fit is posted.
		 */
		bool MakeAheadContext(AheadContext& a_out, bool a_failed);
		/**
		 * @brief Render thread, RunEpoch: main resources made at this frame's Setup. Their context posted for the builder alone
		 * (resourcesAheadSlot; the coordinator's is the frame start's), and the builder woken, so the next frame's wait for a snapshot
		 * for them (AdoptCurrentSnapshot) needs no publication of the coordinator's.
		 */
		void PostResourcesAhead(bool a_failed);
		/**
		 * @brief The builder, before its draws are built for a sealed revision: a work item whose context was made for other main resources
		 * than the revision inputs' (a publication of the coordinator's made before they changed) takes the render thread's for them
		 * (PostResourcesAhead) instead. True when replaced: its draws are built again.
		 */
		bool RefreshWorkContext();
		/**
		 * @brief The stale kind of a_snapshot for the graph as built now, the targets, the shadow format and the main resources
		 * (SnapshotStale), kStaleKinds when current. One without a revision (the publication alone) is never stale.
		 */
		std::size_t StaleKindOf(const SceneSnapshot& a_snapshot) const;
		/**
		 * @brief Render thread: the adopted snapshot is one a frame draws from - current (StaleKindOf), with a revision, complete
		 * draws and their payload ready.
		 */
		bool AdoptedCurrent() const;

		/**
		 * @brief The addresses, SRV indices, capacities and fit a main (shadow) build embeds of its resources' versioned buffers, as
		 * a_sealed names them (the features' frame bindings read at a_frame), and the main layout's decal range.
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
		 * while the last snapshot posted is still in the slot, nor before the scene pass of the frame that took it has ended (a build
		 * starts only while that gate is open, ImplSnapshotBuilder::WorkToBuild, so only draws made again for a posted snapshot wait
		 * here); the frame's take and the pass's end wake the builder.
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
			std::uint32_t a_generation, std::uint32_t a_frame, std::shared_ptr<const EntrySpace> a_sunCandidates, std::shared_ptr<const EntrySpace> a_lightCandidates);
		/** @brief Waits until no build ahead runs (DrainAsync; never per frame). */
		void WaitAhead();
		/**
		 * @brief The depth commit, after the commit hooks (the root states' producer arms its write-back there: Features/Fades' hook):
		 * harvests the frame slot's last visibility events for the decode task, resets the list, arms the slot (the reported words zeroed
		 * once per backing).
		 */
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
		// The frame's draw family decisions (DecideDrawFamilies, by family), which the depth commit and a trees view's faces draw on.
		std::array<bool, Extensions::kDrawFamilies> familyOwned{};
		// The views' frame state: Impl/Views.h (ImplViews).
		/** @brief A view's graph resources, created once its targets are known (render thread). */
		bool SetupView(ViewKind a_kind);
		/**
		 * @brief Revision code: a view's latch layout for a_slots slots, a_buckets buckets and a_classMapWords set indices in its class map,
		 * and its buffers required to hold them and a_draws a face.
		 */
		void RequireView(GrowthBuild& a_build, ViewResources& a_view, std::uint32_t a_slots, std::uint32_t a_buckets, std::uint32_t a_draws, std::uint32_t a_classMapWords);
		/** @brief A DepthTarget view's target, the engine's kPOST_ZPREPASS_COPY, imported (again when it changes), and its extension added. */
		bool ImportViewDepthTarget(ViewKind a_kind);
		/**
		 * @brief A LodTechniques view's phase readiness of an object (a_phase the view's set phase): its pipeline slot's forward pipeline
		 * for the view is in its state's slot pipelines (IndirectDraws::ResolveViewPipelines).
		 */
		bool LodPhaseReady(std::uint8_t a_phase, const SceneTables& a_tables, std::uint32_t a_slot) const;
		/** @brief A ColourWithOwnDepth view's target, the engine's a_texture, imported (again when it changes) with DCLF's own depth. */
		bool ImportViewColourTarget(ViewKind a_kind, ID3D11Texture2D* a_texture);
		/**
		 * @brief A trees view's draw families' rows for an epoch's commit (IndirectDraws::SubmitViewEpoch), in its state: each naming
		 * nothing unless the faces' family is DCLF's.
		 */
		ViewFamilyRows ViewFaceRows(ViewKind a_kind, const ViewFrame& a_shape, std::vector<std::shared_ptr<const void>>& a_owners);
		/** @brief A view's commit (render thread): its map, faces and draw families' face rows into the shape's latch; returns faces drawn. */
		std::uint32_t CommitView(const ViewFrame& a_shape, std::span<const ViewFace> a_faces, std::uint32_t a_inputs, const ViewFamilyRows* a_rows);
		/**
		 * @brief A hosted view epoch a_epoch's commit (IndirectDraws::SubmitViewEpoch of the reflection's faces, first person's depth), into its host's commit
		 * (hostedCommit: only from inside it, the depth commit running the view's work), against the selected revision's shape of the epoch,
		 * which the host's recordings carry: a_listed's payloads, its frame textures and its draw families' rows held with the host's
		 * execution, its latch written in the host's frame slot, and the epoch marked committed (the host submits its recording with its
		 * passes' work). NoRevision outside its host's commit or uncovered.
		 */
		IndirectDraws::ViewSubmit CommitHostedView(ViewKind a_view, std::uint32_t a_epoch, const IndirectDraws::ViewListed& a_listed, std::span<const ViewFace> a_faces,
			std::uint32_t* a_drawn, std::array<std::uint64_t, 2>* a_nullTextures);
		/**
		 * @brief What an epoch drawing a_payloads with the main frame record holds (their bindings, the colour segment's frame textures, the
		 * adopted snapshot); sealed by its record step (ExecutionOwners::Seal). False without a cleanup lane.
		 */
		ExecutionOwners EpochOwners(std::span<const MainPayload* const> a_payloads);
		/** @brief The scene buffers, created once (render thread). False when they have no device address. */
		bool EnsureSceneBuffers(rhi::Device a_device);
		/** @brief ShadowHost: the scene buffers for the shadow resources (SetupShadow). */
		std::shared_ptr<SceneBuffers> ShadowSceneBuffers(rhi::Device a_device) override { return EnsureSceneBuffers(a_device) ? scene : nullptr; }
		/**
		 * @brief Revision code: the scene's tables required to hold a_tables (and the frame's posted counts), with the per-object
		 * buffers (the objects and the input demand) and the faces' draw family lists.
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
		std::array<std::uint32_t, 5> decalWords{};  // the count buffer's decal words (19-22, then 27), uploaded per colour epoch

		/** @brief An epoch's per-frame constant blocks from the capture's mirrors (render thread; keeps the Z-prepass's bytes). */
		void PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, FrameBlocks& a_out);
		// PackFrameBlocks' stages: the captured buffers' mirrored blocks (the Z-prepass's vertex bytes kept, replayed for the colour
		// epoch), and the slots the capture lacks supplied from the last one.
		void MirrorFrameBlocks(const Capture& a_capture, bool a_depthOnly, bool a_replayVertexInputs, FrameBlocks& a_out);
		void CarryMissingBlocks(bool a_depthOnly, bool a_replayVertexInputs, FrameBlocks& a_out);
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
		// CommitMainPayload's stages (Payload/Uploads.cpp): Draws::MainCommit is its shared locals.
		void CommitShadowInputs(bool a_depthOnly, const PassFrame& a_shape, std::uint32_t a_revisionEpoch);
		void ResolveFrameTextures(const MainCommit& a_commit, std::array<std::uint32_t, kTextureRegisters>& a_frameTextures);
		void UploadSceneStreams(const MainCommit& a_commit);
		void ZeroCommitCounts(const MainCommit& a_commit);
		std::uint32_t WriteDepthBuckets(const MainCommit& a_commit, std::uint32_t a_latchSlot, BuildDrawsLatch& a_latch);  // the index pool's copies
		void WriteShadowLatch(const MainCommit& a_commit, std::uint32_t a_latchSlot, BuildDrawsLatch& a_latch);
		void WriteClassRegion(const MainCommit& a_commit, std::uint32_t a_latchSlot, BuildDrawsLatch& a_latch);

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
		std::vector<GpuCascade> sunCascades;
		// The light volumes its BuildDraws selected against (Api::LightVolumesContribution's sample), unclamped (the build parity's
		// reference), and how many of them the latch takes (clamped to the revision's layout: CommitShadowInputs).
		std::vector<GpuShadowVolume> shadowVolumes;
		std::uint32_t shadowVolumesLatched = 0;
		// The depth commit's scratch: its buckets (PlanZBuckets) and the zeros their count words take.
		ZBucketPlan zBucketPlan;
		std::vector<std::uint32_t> zBucketZeros;
		// The colour commit's: the zeros its state class sub-ranges' count words take.
		std::vector<std::uint32_t> classCountZeros;

		// The revision producer's and the scene revisions' types and state: Impl/Producer.h (ImplProducer).
		void ServeRevisionRequests();
		/**
		 * @brief Render thread (the build point, and a frame's wait for a current snapshot): the revision code's requests served, the
		 * graph built when an extension was added or removed (explicit builds), and the revision inputs posted when they moved.
		 */
		void BuildIfAsked(org::PersistentGraphHost& a_host);
		/** @brief Render thread: while recordings are outstanding, every epoch's ready ticket outside a_submitted (segment bits) given back. */
		void ReleaseTicketsForRecordings(org::PersistentGraphHost& a_host, std::uint32_t a_submitted);
		/** @brief Render thread: posts the revision inputs when anything moved. Called where each value is captured (and ShadowHost's). */
		void PostRevisionInputs() override;
		/**
		 * @brief The snapshot builder: a scene revision's shapes, reserved and made from a_publication and the producer's inputs
		 * alone, then assembled. a_frame: the publication's commitFrame.
		 */
		void MakeRevision(const ScenePublication& a_publication, std::uint32_t a_frame);
		/** @brief MakeRevision's stages (Producer.cpp): the main participant's and the views' steps (MainParticipant, ViewParticipant below). */
		void RequireRevisionSet(RevisionMake& a_make, GrowthBuild& a_build);
		void RequireRevisionViews(RevisionMake& a_make, GrowthBuild& a_build);
		void RequireRevisionLatched(RevisionMake& a_make, GrowthBuild& a_build);
		void MakeMainShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout);
		void MakeViewShapes(const RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout);
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
		/**
		 * @brief DecideCoverage's stages (Coverage.cpp): a failed graph's coverage, the epochs' recordings, a registered view's (its
		 * feature's covered callback over Api::ViewCoverage).
		 */
		void CoverFailedGraph();
		void DecideEpochCoverage(org::PersistentGraphHost* a_host);
		void DecideViewCoverage(ViewKind a_kind, org::PersistentGraphHost* a_host, bool a_main, std::uint32_t a_frame);
		void SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index);
		std::string RevisionReport();
		// By draw family, its pipelines as the request's catalog has them (null: none).
		using FamilyPipelinesSet = std::array<const FamilyPipelines*, Extensions::kDrawFamilies>;
		/** @brief What the make's shapes are made from: what every shape names, then each participant's parts (MixShapesKey). */
		ShapesKey RevisionShapesKey(const NamedVersions& a_sealed, const RevisionMake& a_make, std::size_t a_pipelines) const;
		/** @brief RevisionShapesKey's main part: the main pipeline set into the pipelines group, the main segments' latch and captures into theirs. */
		void MixMainShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const;
		/** @brief RevisionShapesKey's views' part: each view's inputs into its slot's key group, the draw families' faces', the DropWhenOff views' toggles. */
		void MixViewShapesKey(ShapesKeyMixer& a_mixer, const FamilyPipelinesSet& a_families) const;
		// The scene revisions' participants (Revision/Participant.h), by ShapeKind: the main segments' (Impl's main steps above), the
		// shadow engine (shadowEngine), the views' (Impl's view steps). Registered here, at construction, in ShapeKind's order, which
		// every loop over them keeps.
		struct MainParticipant final : RevisionParticipant
		{
			explicit MainParticipant(Impl& a_owner) :
				owner(a_owner)
			{}
			Impl& owner;
			void RequireSequences(RevisionMake& a_make, GrowthBuild& a_build) override { owner.RequireRevisionSet(a_make, a_build); }
			void RequireLatched(RevisionMake& a_make, GrowthBuild& a_build) override { owner.RequireRevisionLatched(a_make, a_build); }
			void MixShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const override { owner.MixMainShapesKey(a_make, a_mixer); }
			void MakeShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout) override { owner.MakeMainShapes(a_make, a_sealed, a_layout); }
		};
		struct ViewParticipant final : RevisionParticipant
		{
			explicit ViewParticipant(Impl& a_owner) :
				owner(a_owner)
			{}
			Impl& owner;
			void RequireSequences(RevisionMake& a_make, GrowthBuild& a_build) override { owner.RequireRevisionViews(a_make, a_build); }
			void MixShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const override;
			void MakeShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout) override { owner.MakeViewShapes(a_make, a_sealed, a_layout); }
		};
		MainParticipant mainParticipant{ *this };
		ViewParticipant viewParticipant{ *this };
		std::array<RevisionParticipant*, 3> participants{ &mainParticipant, shadowEngine.get(), &viewParticipant };

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
		// Setup's stages: the main segment's buffers and programs, and the main pass's imported targets and depth.
		bool CreateMainBuffers(Resources& a_state, org::PersistentGraphHost& a_host, rhi::Device a_device);
		bool ImportMainTargets(Resources& a_state, const Capture& a_capture, rhi::Device a_device, const D3D11_TEXTURE2D_DESC& a_target,
			const TargetFormats& a_targets);
	};
}
