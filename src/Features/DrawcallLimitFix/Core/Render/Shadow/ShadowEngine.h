#pragma once

// The shadow engine (ShadowEngine, IndirectDraws::Impl's shadowEngine): the shadow and occlusion views' state - their resources, the
// capability and its catalog, the placements, the frame's captured views and the occlusion maps', the payload the epoch committed, the
// builds' kept state - and the functions over it. It is the shadow shape kind's revision participant (Revision/
// Participant.h: its requirements, shapes, revision inputs and payload, Shadow/ShadowRevision.cpp). What it needs of the rest of the
// implementation it reaches through ShadowHost (the revision inputs' post, the scene buffers) or takes as parameters (the revision
// producer's state, RevisionMake::producer); it holds no reference to the whole Impl. The shadow epoch's frame half, the occlusion maps'
// with it (IndirectDraws' shadow functions, Shadow/*.cpp), reads it through Impl. Each member says whose it is.
// Part of IndirectDraws' implementation: included by Impl.h, before the struct.

namespace DCLF
{
	/** @brief What the shadow engine reaches of the rest of the implementation (Impl): at setup and the captures, never per object. */
	struct ShadowHost
	{
		/** @brief Render thread: posts the revision inputs when anything moved (Impl::PostRevisionInputs). */
		virtual void PostRevisionInputs() = 0;
		/** @brief Render thread: the scene buffers, created once (Impl::EnsureSceneBuffers). Null when they have no device address. */
		virtual std::shared_ptr<SceneBuffers> ShadowSceneBuffers(rhi::Device a_device) = 0;

	protected:
		~ShadowHost() = default;
	};

	struct ShadowEngine final : RevisionParticipant
	{
		explicit ShadowEngine(ShadowHost& a_host) :
			shadowHost(a_host)
		{}
		ShadowEngine(const ShadowEngine&) = delete;
		ShadowEngine& operator=(const ShadowEngine&) = delete;

		ShadowHost& shadowHost;

		// The revision participant's steps (Shadow/ShadowRevision.cpp), in the order Revision/Participant.h calls them.
		void RequireSequences(RevisionMake& a_make, GrowthBuild& a_build) override;
		void RequireLatched(RevisionMake& a_make, GrowthBuild& a_build) override;
		void NoteSealed(RevisionMake& a_make) override;
		void MixShapesKey(const RevisionMake& a_make, Draws::ShapesKeyMixer& a_mixer) const override;
		void MakeShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout) override;
		void PostInputs(RevisionInputs& a_next) override;
		void PrepareAhead(ImplSnapshotBuilder::AheadContext& a_context) override;
		/**
		 * @brief The shadow payload's shared half (BuildShadowPayloadShared: the shared blocks, the material rows and records), from the
		 * build's tables, stream views and lookups alone.
		 */
		void BuildPayloadShared(const AheadBuild& a_build) override;
		/**
		 * @brief The shadow payload, from the build's tables, stream views and lookups, with the publication's candidates, and its exclusions:
		 * the shared half first unless BuildPayloadShared ran it for this build, then the region half and the exclusions.
		 */
		void BuildPayload(const AheadBuild& a_build) override;
		/** @brief The snapshot builder's: the build ahead's shadow payload between its halves (BuildPayloadShared, BuildPayload). */
		struct AheadShadow
		{
			std::shared_ptr<ShadowPayload> payload;
			ShadowInputs in;
			ShadowBuildSplit split;
			bool sharedRegion = false;            // the region half writes into the main build's region (Policy::ReuseResidentRegion)
			const void* publication = nullptr;    // the build ahead's (AheadBuild::out) whose shared half ran
			std::uint32_t frame = 0;
		} aheadShadow;
		/** @brief The snapshot builder's, per make (RequireSequences, RequireLatched): the make's shadow pipelines and the shadow commit's latched copies. */
		struct RevisionScratch
		{
			ShadowIndirectState indirect;
			std::vector<LatchedCopy> latched;
		} revisionScratch;
		std::shared_ptr<const ShadowSlotBuffers> postedShadowSlots;  // the render thread's: the slots' buffers as last posted (PostInputs)

		// The shadow views (CS_DCLF_SHADOWS): their own resources, apart from the main epochs'.
		std::shared_ptr<ShadowResources> shadow;
		bool shadowSetupFailed = false;
		// The shadow epochs counted for the culling counters' samples (ReadShadowCullCounters: one in 127).
		std::uint32_t shadowCullEpochs = 0;
		std::uint32_t shadowLoggedTargets = 0;
		void ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats);
		std::vector<PendingView> pendingViews;
		// The placements the last shadow epoch's views took (bit: slot - kFirstShadowViewSlot), posted as a revision input: the revisions
		// make a recording for each recent set (ShadowFrame::activeViews), and an epoch takes the narrowest one covering its views.
		std::uint64_t lastActiveViews = 0;
		// Per occlusion view: its view, the frame it was set up for (OcclusionView), its pipelines' state and format, and its occluders
		// the last shadow commit uploaded.
		struct OcclusionState
		{
			PendingView view;
			std::uint32_t capturedFrame = ~0u;
			std::uint32_t rasterState = 0;
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
			std::uint32_t inputs = 0, skipped = 0;
		};
		std::array<OcclusionState, kOcclusionViews> occlusion;
		/** @brief Of the maps in a_maps (a bit each), those whose view was set up for frame a_frame (OcclusionView). */
		std::uint32_t OcclusionSetUp(std::uint32_t a_maps, std::uint32_t a_frame) const
		{
			std::uint32_t out = 0;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				out |= ((a_maps >> v) & 1) && occlusion[v].capturedFrame == a_frame ? 1u << v : 0u;
			return out;
		}
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
		// The capability's report counters (ShadowCapabilityReport): since startup.
		struct CapabilityStats
		{
			std::uint32_t changes = 0;  // every change of the modes or the catalog, setup's included
			std::uint32_t defects = 0;  // of them, a view the capability did not foresee
			std::string lastCause;
		} capabilityStats;
		/** @brief The shadow payload the frame's epoch committed (its occlusion maps' views too), held until the next commit. */
		std::shared_ptr<ShadowPayload> committedShadow;
		std::vector<std::shared_ptr<ShadowPayload>> shadowPayloadPool;  // the snapshot builder's
		std::shared_ptr<ShadowPayload> AcquireShadowPayload();
		std::shared_ptr<const void> shadowExecutionOwner;  // the last shadow execution's owners, held until the next
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
		/**
		 * @brief The occlusion maps' layouts as last set up (OcclusionView), each map's while on in a_toggles: what OcclusionReady checks
		 * (the frame's toggles); null, every set-up map's: the revision input, which each revision narrows to its publication's toggles
		 * (RequireSequences) and appends to the placements as the shadow shape's occlusion views.
		 */
		std::vector<ShadowViewLayout> PredictedOcclusion(const ToggleSet* a_toggles) const;
		// The recent sets of views the shadow epochs drew (placements and occlusion maps), a variant each.
		static constexpr std::size_t kRecentActiveViews = 4;
		/** @brief A view rasterizer state's map row's buckets, from a_cache (the revision producer's). */
		static const RowBuckets& ShadowRowBuckets(RowBucketsCache& a_cache, std::uint32_t a_mode, std::uint32_t a_state, const Lookups& a_lookups,
			const ShadowIndirectState& a_indirect);
		// The revision producer's shadow steps (the snapshot builder's), over its state alone (a_producer).
		/** @brief Revision code: every shadow view slot's sequences required to hold the scene's draw bound (brought up to date first: UpdateDrawBound). */
		static void RequireShadowSequences(ImplProducer& a_producer, GrowthBuild& a_build);
		/**
		 * @brief Revision code: view slots and map rows grown (more slots rebuild the graph; either growth is a new latch layout), the
		 * view blocks and every slot's bucket counts required to hold them.
		 */
		static void ReserveShadowLatch(ImplProducer& a_producer, GrowthBuild& a_build, ShadowResources& a_shadow, std::uint32_t a_views, std::uint32_t a_keys,
			std::uint32_t a_rasterStates, std::uint32_t a_sunProcesses);
		/**
		 * @brief Revision code: the shadow epoch's shape (the placements, then the occlusion maps' views) and its variants, from a_sealed,
		 * the make's shadow pipelines and lookups and the shadow commit's latched copies (a_latched).
		 */
		static void MakeShadowShapes(ImplProducer& a_producer, const NamedVersions& a_sealed, const ShadowIndirectState& a_indirect, const Lookups& a_lookups,
			const std::vector<LatchedCopy>& a_latched);
		/**
		 * @brief Whether the last shadow shapes hold a_bounds (capacities and buckets enough, no row at the bucket words
		 * they were made for). Capacities only grow, so a shape remade from a bound they hold is the same shape.
		 */
		static bool ShadowShapesHold(const ImplProducer& a_producer, const ShadowBounds& a_bounds);
		ShadowInputs PrepareShadowInputs(const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
			const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const;
		// ExecuteShadowFrame's stages (Shadow/ShadowEpochs.cpp): the pending views' modes and states noted against the catalog, and
		// a submitted epoch's results.
		void NoteShadowViewModes(std::array<bool, kShadowModeCount>& a_modeUsed, std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates);
		void NoteShadowEpochDrawn(const ShadowPayload& a_payload, const std::array<bool, kShadowModeCount>& a_modeUsed, std::uint32_t a_frame,
			std::uint32_t a_mapsDrawn, double a_occlusionMs, IndirectDraws::ShadowStats& a_stats);
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

		ShadowKept shadowKept;  // the shadow epoch's inputs and records
		ShadowKept* ShadowKeptState() { return &shadowKept; }
		// The builds' exclusions and their caches are the shadow exclusion contribution's (Api::ShadowExclusionContribution: Features/
		// Shadows/Local), which BuildPayload calls.
	};
}
