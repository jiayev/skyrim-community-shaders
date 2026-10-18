#pragma once

// The kept stores (object records, draw bound, extras, geometry draws), journalled across frames so a buffer is sent only what
// changed since the content epoch it holds, and the per-frame stream views made from them. Included by Internal.h, in its order.

namespace DCLF::Draws
{
	// SceneFit (what the named versions of the scene's buffers hold) is Core/Extensions/SceneResources.h's.

	// What a build addresses in an epoch's buffers (a versioned buffer's address, SRV index and capacity: the sealed set's,
	// IndirectDraws::Impl::AddressesOf); a change means the resources were recreated under it.
	struct ResourceAddresses
	{
		// The payload's tables (records, rows, extras) are none of these: the epochs take them from the installed payload.
		std::uint64_t constants = 0, frameConstants = 0;
		std::uint64_t streamBuffer = 0;  // the vertex stream's buffer (SceneBuffers::vertexStream) while its entry class is taken
		std::uint32_t placementsIndex = 0;  // the frame's placement rows (FrameValues::PlacementsIndex)
		std::uint32_t palettesIndex = 0;    // the frame's palettes (FrameValues::PalettesIndex)
		std::uint32_t shadingIndex = 0;     // the frame's shading rows (FrameValues::ShadingIndex)
		// The features' frame bindings' descriptors for the frame the context was posted at (Extensions::FrameBindingValuesOf).
		Extensions::FrameBindingValues frameBindings{};
		std::uint32_t sequenceDecals = 0;    // the sequences' decal range per group (MainLayout): a decal past it waits for their growth
		SceneFit fit;                        // what the scene's buffers hold (an object past them waits)
		const void* identity = nullptr;

		bool operator==(const ResourceAddresses&) const = default;
	};

	/**
	 * @brief The per-object records (BindlessObject) one objects buffer holds, kept across frames (dclf-architecture.md,
	 * "Persistent draw state"): a record is written again only when the change log names a column it is built from, and the
	 * buffer is sent what changed since the content epoch it holds (KeptArray). One, for every epoch's builds (SceneBuffers),
	 * updated by MakeStreamViews, which never runs twice at once; `collisions` counts it if anything ever does.
	 */
	struct ObjectRecordStore
	{
		LogCursor cursor;
		MarkedList changedObjects;
		KeptArray<BindlessObject> records;
		std::atomic<std::uint32_t> busy{ 0 };
		// Since the last report.
		std::uint64_t updates = 0, rewritten = 0, resyncs = 0, collisions = 0;
		// Updates whose changes were only placements and palettes (the per-frame streams), and the updates with any other
		// cause, by cause.
		std::uint64_t streamOnly = 0, structural = 0;
		std::array<std::uint64_t, kChangeCauseCount> byCause{};
	};

	/**
	 * @brief A view rasterizer state's buckets (ShadowBucket): the distinct pipelines its map row names, the depth-only class's
	 * first (DrawPipelines::ShadowDiscards) - a view draws them before the alpha-tested casters, whose fragments then fail
	 * the depth test sooner - each class in index order; and each key slot's bucket. A pipeline the epoch's state does not
	 * hold yet (published after it was taken) has no bucket: its key slots draw nothing this frame.
	 */
	struct RowBuckets
	{
		std::vector<std::uint32_t> pipelines;     // by bucket
		std::vector<std::uint32_t> bucketOfSlot;  // by key slot: its bucket, or Lookups::kNone
		bool operator==(const RowBuckets&) const = default;
	};

	/**
	 * @brief The draws the scene's objects can produce (SceneDrawBound), in all and per pipeline slot, kept from the change log:
	 * a slot the log names has its share taken out and put back, so a reserve reads what changed rather than every slot. Owned
	 * by the revision producer (Impl::RevisionProducer::drawBound): each make brings it up to date once, before the reuse key
	 * (Impl::MakeRevision); RequireMain and the shadow engine's RequireSequences only read it.
	 */
	struct DrawBoundStore
	{
		static constexpr std::uint32_t kNoPipeline = ~0u;
		LogCursor cursor;
		bool partitioned = false;                 // the tables had a skin partition column (SceneDrawBound reads it only then)
		std::vector<std::uint32_t> produced;      // by object slot: its draws (a free slot's none)
		std::vector<std::uint32_t> pipeline;      // by object slot: the pipeline slot they count toward (kNoPipeline: a decal's)
		std::vector<std::uint64_t> perPipeline;   // by pipeline slot
		std::uint64_t draws = 0;
		/**
		 * @brief What an object casts with: its caster technique (the shadow modes add their bits) and the occlusion views', its
		 * raster flags and vertex layout - a ShadowPipelineKey per mode - and its draws.
		 */
		struct ShadowShare
		{
			std::uint32_t produced = 0;  // 0: none (a free slot)
			bool caster = false;         // not kObjectNoShadow
			std::uint32_t technique = 0;
			std::array<std::uint32_t, kOcclusionViews> occlusion{};
			std::uint32_t rasterFlags = 0;
			std::uint64_t vertexLayout = 0;
			bool operator==(const ShadowShare&) const = default;
			/** @brief Its key slot's key under a_mode (Lookups::shadowSlots: the base technique), false when it does not draw in the mode. */
			bool KeyOf(std::uint32_t a_mode, ShadowPipelineKey& a_key) const;
		};
		// The same per shadow and occlusion mode and caster key: every draw an object of the scene can cast in the mode, the set's
		// or not. The shadow views' capacities are sized from it (ShadowBounds), so they change with the scene, not the frame's casters.
		std::vector<ShadowShare> shadow;  // by object slot
		std::array<ankerl::unordered_dense::map<ShadowPipelineKey, std::uint64_t, ShadowPipelineKeyHash>, kShadowModeCount> modeKeyDraws;
		// Since the last report.
		std::uint64_t updates = 0, changes = 0, resyncs = 0;
		// Moves whenever an object's casting share does (modeKeyDraws changed): what the revisions' shadow shapes are sized from.
		std::uint64_t shadowVersion = 0;

		std::uint32_t Draws() const { return static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX)); }
		std::uint32_t PipelineDraws(std::size_t a_pipeline) const
		{
			return a_pipeline < perPipeline.size() ? static_cast<std::uint32_t>(std::min<std::uint64_t>(perPipeline[a_pipeline], UINT32_MAX)) : 0u;
		}
	};

	/** @brief Per shadow and occlusion mode, the draws of each caster key (DrawBoundStore::modeKeyDraws). */
	using ModeKeyDraws = std::array<ankerl::unordered_dense::map<ShadowPipelineKey, std::uint64_t, ShadowPipelineKeyHash>, kShadowModeCount>;
	/** @brief The draw bound DrawBoundStore keeps, by a scan of every slot of a_tables (pure: the store's reference). */
	struct ScannedDrawBound
	{
		std::uint32_t draws = 0;
		std::vector<std::uint32_t> perPipeline;  // by pipeline slot
		ModeKeyDraws modeKeys;
	};
	ScannedDrawBound ScanDrawBound(const SceneTables& a_tables);

	/** @brief A build's view of the object records: the store's, or a full set of its own without one (content epoch 0). */
	using ObjectRecordsOut = KeptView<BindlessObject>;

	/**
	 * @brief The rows the extras buffer holds (Records.h, kExtraRows a block): read straight from the tables' array, and journalled
	 * as rows, so the buffer is sent the rows changed since the content epoch it holds. The change log names them: an extras block's
	 * rows or place (kChangeExtras). One, for every epoch's builds (SceneBuffers), updated like ObjectRecordStore.
	 */
	struct ExtrasStore
	{
		LogCursor cursor;
		ChangeJournal rows;
		std::uint64_t updates = 0, rowsSent = 0, resyncs = 0;
	};

	/** @brief A build's view of the rows: the tables' array, with the store's changes (content epoch 0: sent whole). */
	struct ExtrasOut
	{
		const float* extras = nullptr;
		std::uint32_t extraRows = 0;
		ChangeJournal::Snapshot changes;
		std::uint64_t ContentEpoch() const { return changes.contentEpoch; }
		std::uint32_t Rows() const { return extraRows; }
		const float* Row(std::uint64_t a_row) const { return extras + std::size_t(a_row) * 4; }
		void Reset() { *this = {}; }
	};

	void UpdateExtras(ExtrasStore* a_store, std::uint64_t a_oldestContentEpoch, const SceneTables& a_tables, std::uint32_t a_generation, ExtrasOut& a_out);

	/**
	 * @brief The uploads of a build's rows a buffer at a_contentEpoch lacks (the changed runs, else all of them), clipped to the buffer
	 * (a_bufferRows: what is past it waits for its growth). a_mirror (an observer's, Diagnostics::ExtrasSending): that buffer's
	 * rows as uploaded, written with them.
	 */
	template <class Emit>
	std::size_t EmitExtras(const ExtrasOut& a_out, std::uint64_t a_contentEpoch, std::vector<float>* a_mirror, Emit&& a_emit, std::uint32_t a_bufferRows = ~0u)
	{
		const std::uint32_t rows = std::min(a_out.Rows(), a_bufferRows);
		if (!rows || !a_out.extras)
			return 0;
		std::size_t sent = 0;
		a_out.changes.ForEachRun(a_contentEpoch, rows, [&](std::uint64_t a_first, std::uint64_t a_count) {
			a_emit(a_out.Row(a_first), std::size_t(a_count) * 16, std::size_t(a_first) * 16);
			if (a_mirror) {
				if (a_mirror->size() < std::size_t(rows) * 4)
					a_mirror->resize(std::size_t(rows) * 4, 0.0f);
				std::memcpy(&(*a_mirror)[std::size_t(a_first) * 4], a_out.Row(a_first), std::size_t(a_count) * 16);
			}
			sent += static_cast<std::size_t>(a_count);
		});
		return sent;
	}

	/**
	 * @brief A build's view of the geometry table: the geometry slots' draws (the store's, or a set of its own without one)
	 * and the vertex stream rows' after them, which are the frame's and sent whole every build.
	 */
	struct GeometryDrawsOut
	{
		KeptView<GeometryDraw> slots;
		std::vector<GeometryDraw> streamRows;
		std::size_t SlotCount() const { return slots.Count(); }
		std::size_t Count() const { return SlotCount() + streamRows.size(); }
		std::uint64_t ContentEpoch() const { return slots.ContentEpoch(); }
		std::vector<GeometryDraw> Flat() const
		{
			std::vector<GeometryDraw> flat;
			if (slots.elements)
				flat = *slots.elements;
			flat.insert(flat.end(), streamRows.begin(), streamRows.end());
			return flat;
		}
		void Reset() { *this = {}; }
	};

	/**
	 * @brief The scene streams of one set of tables: the object records, the extras rows and the geometry slots' draws, brought
	 * up to date once (MakeStreamViews) and read by every build and commit of those tables as immutable views. The tables are
	 * held: the extras' view reads their array.
	 */
	struct StreamViews
	{
		std::shared_ptr<const SceneTables> tables;
		std::uint32_t generation = 0;
		ObjectRecordsOut objects;
		ExtrasOut extras;
		KeptView<GeometryDraw> geometries;
	};
	/**
	 * @brief A build's geometry slots: the frame's stream views', or packed whole from the tables without them (a diagnostics
	 * reference build, or no scene buffers yet).
	 */
	void TakeGeometryDraws(const StreamViews* a_streams, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame,
		GeometryDrawsOut& a_out);

	/**
	 * @brief The geometry slots' draws one geometry buffer holds, kept across builds: repacked from the tables' geometry log
	 * (SceneTables::geometryLog), and sent as the slots changed since the content epoch the buffer holds. One, for every epoch's
	 * builds (SceneBuffers), updated like ObjectRecordStore.
	 */
	struct GeometryStore
	{
		LogCursor cursor;
		KeptArray<GeometryDraw> packed;
		// Since the last report.
		std::uint64_t updates = 0, rewritten = 0, resyncs = 0;
	};

	/**
	 * @brief The stores brought up to a_tables (their journals trimmed to a_oldest), as views (StreamViews). One caller at a time: the
	 * snapshot builder.
	 */
	std::shared_ptr<const StreamViews> MakeStreamViews(ObjectRecordStore& a_objects, ExtrasStore& a_extras, GeometryStore& a_geometries, const TablesContentEpochs& a_oldest,
		std::shared_ptr<const SceneTables> a_hold, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame);

	// What a record is built from (BuildObjectRecord): the alpha test and the LOD fades (bindings), the ancestor value, the
	// palette's place and the extras'.
	constexpr std::uint32_t kObjectRecordCauses = kChangeBindings | kChangeAncestorValue | kChangeSkin | kChangeExtras;

	/**
	 * @brief The build's object records: brought up to date from the change log in the store (its journal trimmed to
	 * a_oldestContentEpoch, the oldest a payload backing holds), or built whole without one.
	 */
	void UpdateObjectRecords(ObjectRecordStore* a_store, std::uint64_t a_oldestContentEpoch, const SceneTables& a_tables, std::uint32_t a_generation,
		std::uint32_t a_frame, ObjectRecordsOut& a_out);
}
