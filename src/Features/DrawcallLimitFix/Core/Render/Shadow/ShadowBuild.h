#pragma once

// The shadow build's types and helpers: caster classes, view bits and keys, the sun entry test, the kept shadow state, and the
// scene lane's pipeline lookups. The build runs on DCLF's pool (the snapshot builder). Included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief Whether an object is of the volumetric-only class in mode a_mode: drawn only by the views of the sun's
	 * volumetric lighting copy. A point light registers a volumetric-only caster like any other (dclf-architecture.md,
	 * "Point lights' shadow culls without DCLF's entries"), so in the paraboloid mode it is an ordinary caster.
	 */
	inline bool VolumetricClass(std::uint32_t a_mode, std::uint32_t a_flags)
	{
		return !IsOcclusionMode(a_mode) && a_mode != kParabolicShadowMode && (a_flags & kObjectVolumetricOnly) != 0;
	}

	/** @brief The technique bits a mode index adds to an object's base technique: none for an occlusion view's, which are complete. */
	inline std::uint32_t ModeBitsOf(std::uint32_t a_mode)
	{
		return IsOcclusionMode(a_mode) ? 0u : ShadowModeBits(PassCapture::kFirstShadowMode + a_mode);
	}

	/** @brief The technique object a_object draws with under mode a_mode: an occlusion view's own, or its caster technique with the mode's bits. 0: none. */
	inline std::uint32_t ModeTechnique(const SceneTables& a_tables, std::uint32_t a_mode, std::size_t a_object)
	{
		if (IsOcclusionMode(a_mode)) {
			const auto& column = a_tables.occlusionTechnique[OcclusionOfMode(a_mode)];
			return a_object < column.size() ? column[a_object] : 0u;
		}
		return (a_tables.objects[a_object].flags & kObjectNoShadow) ? 0u : (a_tables.shadowTechnique[a_object] | ModeBitsOf(a_mode));
	}

	/**
	 * @brief The view types a set phase mask takes part in (kView*): Core's phases' (main, casters) and the feature views' of both families
	 * (the occlusion maps' and the views' entries' phase and view bit), resolved once into a table by mask
	 * (Extensions::ViewBitTable::viewBitsOfPhases).
	 */
	inline std::uint32_t ViewBitsOf(std::uint8_t a_phases)
	{
		return Extensions::ResolvedViewBits().viewBitsOfPhases[a_phases];
	}
	/**
	 * @brief An input's view mask (DrawInput::ViewWords::mask): the view types of the object's applied phases (SceneTables::setPhases),
	 * and kViewMainCull for a main candidate (every input of the main payloads).
	 */
	inline std::uint32_t ViewMaskOf(const SceneTables& a_tables, std::size_t a_object, bool a_mainCandidate)
	{
		const std::uint8_t phases = a_object < a_tables.setPhases.size() ? a_tables.setPhases[a_object] : std::uint8_t{ 0 };
		return ViewBitsOf(phases) | (a_mainCandidate ? kViewMainCull : 0u);
	}
	/**
	 * @brief A main input's view mask (the one main list both segments read). A decal's is kViewDecal alone: the colour
	 * segment's fixed slots, culled by no other view. Every other candidate's is its phases' bits with kViewMainCull, kViewMain
	 * exactly while its record is a member (kObjectMember, what its drawable bit follows), so a drawable input is the colour
	 * segment's whenever it is the depth segment's.
	 */
	inline std::uint32_t MainMaskOf(const SceneTables& a_tables, std::size_t a_object)
	{
		const std::uint32_t flags = a_object < a_tables.objects.size() ? a_tables.objects[a_object].flags : 0u;
		// A CategoryRoot view's member's is its view bit alone (an exclusive phase): a first-person member's kViewFirstPerson, the
		// first-person epochs' cull and draws, never a main candidate (the world's depth segment would draw a drawable input with
		// kViewMainCull, and the HZB must leave the arms out).
		if (a_object < a_tables.setPhases.size())
			if (const auto root = static_cast<std::uint8_t>(a_tables.setPhases[a_object] & Extensions::Views().rootPhases))
				return ViewBitsOf(root);
		if (ObjectDecalGroup(flags))
			return kViewDecal;
		// A forward object's is kViewForward alone (with its member bit): the colour segment's forward range, culled by no other
		// view (never a depth segment input: it occludes nothing, as the engine's blended draws write no depth before the opaque ones).
		if (flags & kObjectForward) {
			// A refraction object's is kViewRefraction alone: the refraction range (its key, kRasterRefraction).
			const std::uint32_t pipeline = a_tables.objects[a_object].pipelineIndex;
			if (!(flags & kObjectNoBindings) && pipeline < a_tables.pipelines.size() && RasterRefraction(a_tables.pipelines[pipeline].rasterFlags))
				return kViewRefraction;
			return kViewForward;
		}
		// The main build's bits of a scene entry alone (MainSideBits): the shadow views' are the shadow build's.
		return (ViewMaskOf(a_tables, a_object, true) & MainSideBits() & ~kViewMain) | ((flags & kObjectMember) ? kViewMain : 0u);
	}
	/**
	 * @brief The view bits a shadow or occlusion view draws: its mode's, a clamped view's split by whether it is the sun's, an occlusion
	 * map's its entry's (Extensions::OcclusionMapOf).
	 */
	inline std::uint32_t ShadowViewBits(std::uint32_t a_mode, bool a_sun)
	{
		if (IsOcclusionMode(a_mode))
			return Extensions::OcclusionMapOf(OcclusionOfMode(a_mode)).viewBit;
		if (a_mode == kParabolicShadowMode)
			return kViewPointCaster;
		return a_sun ? kViewSunCaster : kViewSpotCaster;
	}
	/** @brief Every view bit of a mode's views: what an input the mode holds carries in its mask (the one shadow list). */
	inline std::uint32_t ModeViewBits(std::uint32_t a_mode) { return ShadowViewBits(a_mode, true) | ShadowViewBits(a_mode, false); }
	/** @brief The key word a mode's views read (kCullKeyShift): a caster's key, or its occlusion map's. */
	inline std::uint32_t KeyWordOf(std::uint32_t a_mode)
	{
		return (IsOcclusionMode(a_mode) ? 2u + OcclusionOfMode(a_mode) : 1u) << kCullKeyShift;
	}
	/** @brief The key slot a mode's views draw a shadow input by (KeyWordOf). */
	inline std::uint32_t ShadowKeyOf(std::uint32_t a_mode, const DrawInput::ViewWords& a_view)
	{
		return IsOcclusionMode(a_mode) ? a_view.occlusionKey[OcclusionOfMode(a_mode)] : a_view.shadowKey;
	}
	inline std::uint32_t ShadowKeyOf(std::uint32_t a_mode, const DrawInput& a_input) { return ShadowKeyOf(a_mode, a_input.view); }

	/**
	 * @brief The technique object a_object's key slot names under mode a_mode (Lookups::shadowSlots): an occlusion view's own, or
	 * its caster technique without any mode's bits - one slot for every caster mode. 0: none.
	 */
	inline std::uint32_t BaseTechnique(const SceneTables& a_tables, std::uint32_t a_mode, std::size_t a_object)
	{
		if (IsOcclusionMode(a_mode)) {
			const auto& column = a_tables.occlusionTechnique[OcclusionOfMode(a_mode)];
			return a_object < column.size() ? column[a_object] : 0u;
		}
		return (a_tables.objects[a_object].flags & kObjectNoShadow) ? 0u : a_tables.shadowTechnique[a_object];
	}

	/** @brief The techniques of the occlusion views among a_modeUsed object a_object draws into, ORed (its record, and whether alpha-tested). */
	inline std::uint32_t OcclusionTechniques(const SceneTables& a_tables, const std::array<bool, kShadowModeCount>& a_modeUsed, std::size_t a_object)
	{
		std::uint32_t techniques = 0;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			if (a_modeUsed[OcclusionModeOf(v)] && a_object < a_tables.occlusionTechnique[v].size())
				techniques |= a_tables.occlusionTechnique[v][a_object];
		return techniques;
	}

	// Whether an object's entry is outside every one of the sun's full-frustum processes, so the sun's cascade
	// culls never reach it (ShadowInputs::sunEntryProcesses): the CPU's verdict, from the entry node's bound now (render
	// thread, diagnostics only), which the shadow checks compare with the test BuildDraws makes on the placement row.
	inline bool OutsideSunEntry(const ShadowInputs& a_in, const SceneTables& a_tables, std::size_t a_object)
	{
		const auto* node = a_object < a_tables.listedRoot.size() ? a_tables.listedRoot[a_object] : nullptr;
		if (!node || node->worldBound.radius < 0.0f)
			return false;
		const auto& bound = node->worldBound;
		const float entry[4]{ bound.center.x, bound.center.y, bound.center.z, bound.radius };
		return OutsideSunEntryProcesses(a_in.sunEntryProcesses, entry);
	}

	/**
	 * @brief The shadow epoch's inputs and binding records, kept across frames (dclf-architecture.md, "Persistent draw
	 * state"). Per render mode, a region of the casters' inputs, changed only by the change log's entries for them,
	 * by a mode's views changing their rasterizer states, and by what was waiting (a pipeline or a diffuse texture not yet
	 * resolved) becoming ready; the per-frame list is only the face shapes. The material rows (ShadowMaterialRow): the plain
	 * one and one per alpha-tested material, at slots the materials keep, lowest free first, in one table every view reads
	 * (ShadowResources::materialRows, which grows when the slots outnumber it: a slot past its capacity waits a frame). The
	 * rows and each mode's inputs are KeptArrays: a buffer is sent what changed since the content epoch it holds.
	 */
	struct ShadowKept
	{
		static constexpr std::uint32_t kWaiting = ~0u - 1;  // an object's record while its material's texture is not resolved
		static constexpr std::uint32_t kNoRecord = ~0u;
		LogCursor cursor;
		const void* identity = nullptr;
		SceneFit fit;  // what the scene's buffers held at its last build: their growth reads every object again
		std::uint64_t build = 0;  // counts the builds: what ShadowKept::Mode::membership stamps
		KeptArray<ShadowMaterialRow> rows;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::uint32_t> slotOf;
		std::vector<const RE::BSShaderMaterial*> slotMaterial;
		std::vector<ID3D11ShaderResourceView*> slotDiffuse;
		std::vector<std::uint32_t> slotRefs;
		std::vector<std::uint8_t> slotReady;
		std::vector<std::shared_ptr<const void>> slotOwner;  // the diffuse texture's import, while the slot's row names it
		std::vector<std::uint32_t> freeSlots;  // a min-heap: the lowest free slot is taken first, so the table stays dense
		// The rows written on events, not every build: a slot is written when acquired, while it waits (its texture not
		// resolved, or past the table's capacity), while its material's texture transform moves (the controllers' events,
		// watched while its two buffers differ or for 2 builds after the last event), and all of them when the lookups or
		// the capacity change.
		std::vector<std::uint32_t> rowDirty;
		std::vector<std::uint8_t> rowDirtyMark;
		std::vector<std::uint32_t> transformWatch;
		std::vector<std::uint64_t> transformWatchBuild;  // per slot: the build of its last event, 0 unwatched
		std::uint64_t lookupsGeneration = ~0ull;
		// Every slot's owner, as one root for the payloads (bindingOwners), rebuilt when an owner changes.
		std::shared_ptr<const std::vector<std::shared_ptr<const void>>> owners;
		bool ownersChanged = true;
		std::vector<std::uint32_t> objectRecord;  // per object: its material row, 0 the plain one, kNoRecord, kWaiting
		std::vector<const RE::BSShaderMaterial*> objectMaterial;  // per object: the material its slot is held for
		// Per mode: what its views draw of the list (heldModes), and what waits. The inputs are the scene region's.
		struct Mode
		{
			bool active = false;
			ModeRasterStates rasterStates;
			std::vector<std::uint32_t> waiting;  // objects waiting for a pipeline or a texture
			std::vector<std::uint8_t> waitingMark;
			std::vector<std::uint32_t> faces;    // objects the frame's list writes (face shapes, second-stream positions)
			std::vector<std::uint8_t> faceMark;
			// The build version at which an object last joined or left the mode's inputs (the region or the faces): what
			// the sun exclusion's reuse reads (BuildSunExclusion).
			std::uint64_t membership = 0;
			/** @brief Empty, for every object to be read again. */
			void Reset() { *this = Mode{}; }
		};
		std::array<Mode, kShadowModeCount> modes;
		// The one shadow list: an entry per object some mode holds, its mask the views of those modes.
		KeptRegion scene;
		std::vector<std::uint8_t> heldModes;  // per object: the modes whose region inputs it is (bit m)
		std::vector<std::uint8_t> touchedMark;  // a build's scratch: the objects whose entry it writes again
		std::uint64_t sceneResets = ~0ull;      // the scene region's resets its parts were written after
		/**
		 * @brief CountModeDraws' counts of the scene region's entries, for every mode (a build reads its used modes'), kept across builds
		 * and brought up to the region by its journal: a build recounts the entries changed since, not the list. The payload's own inputs
		 * (inputList, the frame's) are counted every build.
		 */
		struct ModeCounts
		{
			struct Entry
			{
				DrawInput::ViewWords view;  // what the entry was counted as (its mask 0: nothing)
				std::uint32_t partitions = 0;
			};
			const void* source = nullptr;    // the region counted (the scene list, or this state's own)
			std::uint64_t contentEpoch = 0;  // the region's journal position counted (0: counted whole every build)
			std::array<std::uint32_t, kShadowModeCount> modeBits{};
			std::vector<Entry> entries;  // per region entry
			std::array<std::uint64_t, kShadowModeCount> draws{};
			std::array<std::uint32_t, kShadowModeCount> inputs{};
			std::array<std::vector<std::uint32_t>, kShadowModeCount> keyDraws, keyInputs;  // per mode, by key slot
		} modeCounts;
		bool Holds(std::uint32_t a_mode, std::uint32_t a_object) const { return a_object < heldModes.size() && ((heldModes[a_object] >> a_mode) & 1); }
		/** @brief Empty, for every object to be read again; the journals count on and the report's counters stay. */
		void Reset()
		{
			auto keptRows = std::move(rows);
			auto keptInputs = std::move(scene.inputs);
			const auto counters = std::tuple{ build, builds, entriesWritten, rowsWritten, resyncs, entriesCounted };
			*this = ShadowKept{};
			rows = std::move(keptRows);
			rows.Clear();
			scene.inputs = std::move(keptInputs);
			scene.inputs.Clear();
			std::tie(build, builds, entriesWritten, rowsWritten, resyncs, entriesCounted) = counters;
		}
		// Since the last report (entriesCounted: the region entries CountModeDraws recounted).
		std::uint64_t builds = 0, entriesWritten = 0, rowsWritten = 0, resyncs = 0, entriesCounted = 0;
	};

	/**
	 * @brief The shadow epoch's frame-shared part and its per-mode inputs, from the tables and the lookups:
	 * pure. The per-view blocks are the commit's, because the views are captured while the engine draws
	 * them, after the build may have started.
	 */
	void BuildShadowPayload(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
		std::shared_ptr<const StreamViews> a_streams = nullptr, ShadowKept* a_kept = nullptr, ResidentRegion* a_scene = nullptr);

	/**
	 * @brief What BuildShadowPayloadShared hands BuildShadowPayloadRegion: the objects it read again (the modes take them again), the
	 * objects whose entries the region half writes, in the order they were touched (the region's entry order follows it), and the
	 * build's numbers. Its vectors are reused across builds (the snapshot builder's).
	 */
	struct ShadowBuildSplit
	{
		std::vector<std::uint32_t> changed;
		std::vector<std::uint32_t> touched;
		std::uint64_t build = 0;  // ShadowKept::build
		std::uint32_t transformBuffer = 0;
		bool resync = false;   // the kept state was emptied: the region half clears the shadow words first
		bool pending = false;  // the shared half ran, the region half has not
	};

	/**
	 * @brief BuildShadowPayload in two halves, called in order on one thread at a time (the second may run on another thread, after the
	 * first). The shared half needs no region: from the tables, the lookups and the stream views alone it writes a_out's shared blocks
	 * (WriteSharedBlocks: its reset, the geometry table, the arena's frame record and blocks), the material rows and the kept state's
	 * records (TakeChangedRecords, UpdateMaterialRows); it writes a_kept and a_out only, never the main build's region, so it may run
	 * beside the main build (a_sharedRegion: whether the region half is handed it). The region half writes the shadow words into
	 * a_scene (the main build's region, once its build finished; null: the kept state's own), the modes' holdings, the frame's list,
	 * the counts and the kept parity's event.
	 */
	void BuildShadowPayloadShared(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
		std::shared_ptr<const StreamViews> a_streams, ShadowKept* a_kept, bool a_sharedRegion, ShadowBuildSplit& a_split);
	void BuildShadowPayloadRegion(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out, ShadowKept* a_kept,
		ResidentRegion* a_scene, ShadowBuildSplit& a_split);

	/**
	 * @brief What the shadow pipelines are resolved for that only the frame knows: the capability's modes with their
	 * views' rasterizer states, and the targets' formats. Posted by the frame's start when it changes (IndirectDraws::PostLookupInputs),
	 * read by the scene lane (latest wins); `enabled` false while DCLF draws no shadow view (the shadow pipelines then stand as they are).
	 */
	struct ShadowLookupInputs
	{
		bool enabled = false;
		std::array<bool, kShadowModeCount> modes{};
		std::array<ModeRasterStates, kShadowModeCount> rasterStates{};
		DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
		std::array<DXGI_FORMAT, kOcclusionViews> occlusionFormats{};

		bool operator==(const ShadowLookupInputs&) const = default;
	};

	/**
	 * @brief The scene lane: the pipeline entries of every pipeline slot a_tables use - the set index, the constant tables
	 * and the register usage, as a_catalog has them - each key asked of the pipeline lane once. Written only where they differ.
	 */
	void ResolvePipelineLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups);

	/**
	 * @brief The scene lane: the shadow pipelines of the modes in use (a_inputs), asked of the pipeline lane and resolved
	 * from a_catalog alone, which is published with a_lookups (GetShadowIndirectState of a_catalog binds its set).
	 */
	void ResolveShadowLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, const ShadowLookupInputs& a_inputs, LookupsResolveState& a_state,
		Lookups& a_lookups);

	// The resident region's resync reasons (MainBuild::UpdateRegionEntries): log, segment, shrunk, scope, fit.
	constexpr std::size_t kResyncReasons = 5;
	constexpr std::size_t kAsyncColour = 0;
	constexpr std::size_t kAsyncZPrepass = 1;
	constexpr std::size_t kAsyncShadow = 2;
}
