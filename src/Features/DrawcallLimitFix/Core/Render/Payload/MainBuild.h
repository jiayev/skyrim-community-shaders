#pragma once

// The main build's types: per-pair binding verdicts, PerGeometry templates, the scene list (ResidentRegion) and the build cache.
// The builds run on DCLF's pool, one at a time (the snapshot builder, Revision/Snapshot.cpp). Included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief A (material, pipeline) pair's verdict for one build (MainBuild::AssembleRecord): its rows, or why it cannot draw.
	 * The skip counters are per draw, so a pair that cannot keeps the reason and every draw of it raises it.
	 */
	struct ResolvedBindings
	{
		std::uint32_t recordIndex = kNoRecord;  // RowsOf(pipeline, material)
		std::uint32_t skipReason = kNoSkip;
		bool deferred = false;
		std::uint32_t missingTexture = 0;  // the register that failed, for the skip sample
	};

	// The PerGeometry group, packed once per pipeline. Every object on a pipeline shares all of it but five
	// variables (the two world matrices and three shading values), so an object copies this and rewrites
	// only those instead of walking the whole variable table twice.
	struct GeometryTemplate
	{
		std::vector<std::byte> vs;
		std::vector<std::byte> ps;
		GeometryPatchOffsets offsets;
	};

	/**
	 * @brief A pipeline's PerGeometry template from its constants. Under DCLF_BINDLESS what those draws never read from
	 * the block (kVSBindlessGeometryUnread, kPSBindlessGeometryUnread: the frame lighting has a block of its own) packs
	 * as zero, so the block's bytes change only with the pipeline's own values; the group keeps its full size.
	 */
	inline void PackGeometryTemplate(const GeometryConstants& a_constants, std::span<const std::uint8_t> a_vsTable, std::span<const std::uint8_t> a_psTable, bool a_bindless,
		GeometryTemplate& a_out)
	{
		const auto vsSize = ConstantGroupSize(LightingVSLayout(), a_vsTable, kVSGroups[kPerGeometry], kVSFirstVariable[kPerGeometry]);
		const auto psSize = ConstantGroupSize(LightingPSLayout(), a_psTable, kPSGroups[kPerGeometry], kPSFirstVariable[kPerGeometry]);
		a_out.vs.assign(std::max<std::size_t>(vsSize, 16), std::byte{});
		a_out.ps.assign(std::max<std::size_t>(psSize, 16), std::byte{});
		const std::uint64_t vsUnread = a_bindless ? kVSBindlessGeometryUnread : 0, psUnread = a_bindless ? kPSBindlessGeometryUnread : 0;
		PackConstantGroup(a_constants.vs, LightingVSLayout(), a_vsTable, kVSGroups[kPerGeometry] & ~vsUnread, kVSFirstVariable[kPerGeometry], a_out.vs);
		PackConstantGroup(a_constants.ps, LightingPSLayout(), a_psTable, kPSGroups[kPerGeometry] & ~psUnread, kPSFirstVariable[kPerGeometry], a_out.ps);
		a_out.offsets = GeometryPatchOffsetsOf(a_vsTable, a_psTable);
		a_out.vs.resize(vsSize);
		a_out.ps.resize(psSize);
	}

	/** @brief ResidentRegion::indexOf: the object is not in the region. */
	constexpr std::uint32_t kNoRegion = ~0u;
	// ResidentRegion::drawsOf: a drawable decal entry (its sequence is its group's slot), and the draws an entry counts.
	constexpr std::uint8_t kRegionDecal = 0x80;
	constexpr std::uint8_t RegionDraws(std::uint8_t a_draws) { return a_draws & 0x7F; }
	constexpr std::uint64_t kNoPair = ~0ull;  // a region entry without bindings (a cull-only candidate)

	// ---- The scene list: one entry per object, its main part the main build's and its shadow words the shadow build's.
	// The view bits the main build writes of an entry (the main segments', refraction's, reflection's, first person's; MainMaskOf);
	// the rest are the shadow build's. A main bit missing here is dropped when an entry's parts merge (SetMainPart, SetShadowPart),
	// and BuildDraws' view test then skips that view's input entirely.
	// Core's own, and the views' (Extensions::ViewSet::viewBits, resolved at install: a plain read of the resolved set, no call).
	constexpr std::uint32_t kMainCoreSideBits = kViewMainCull | kViewMain | kViewDecal | kViewForward | kViewRefraction;
	inline std::uint32_t MainSideBits() { return kMainCoreSideBits | Extensions::Views().viewBits; }
	// Which builds' parts an entry holds (ResidentRegion::sideOf).
	constexpr std::uint8_t kSideMain = 1;
	constexpr std::uint8_t kSideShadow = 2;
	/** @brief A scene entry's main part from a_main (MainBuild::RegionEntry), its shadow words kept. */
	inline void SetMainPart(DrawInput& a_entry, const DrawInput& a_main)
	{
		const DrawInput::ViewWords view = a_entry.view;
		const std::uint32_t shadowRow = a_entry.shadowRow;
		const std::uint32_t mainSide = MainSideBits();
		a_entry = a_main;
		a_entry.view = view;
		a_entry.view.mask = (a_main.view.mask & mainSide) | (view.mask & ~mainSide);
		a_entry.shadowRow = shadowRow;
	}
	/** @brief A scene entry with no main part: what the main build wrote of it cleared (its object's fields stay). */
	inline void ClearMainPart(DrawInput& a_entry)
	{
		a_entry.pipelineIndex = a_entry.recordIndex = a_entry.decalOrdinal = 0;
		a_entry.fadeDistance = 0.0f;
		a_entry.flags &= ~kInputDrawable;
		a_entry.view.mask &= ~MainSideBits();
	}
	/** @brief A scene entry's shadow words from a_shadow (ShadowEntry), its main part kept; with none, a_shadow's object fields too. */
	inline void SetShadowPart(DrawInput& a_entry, const DrawInput& a_shadow, bool a_mainHeld)
	{
		if (!a_mainHeld) {
			a_entry = a_shadow;
			ClearMainPart(a_entry);
			a_entry.view.mask = a_shadow.view.mask & ~MainSideBits();
			return;
		}
		const std::uint32_t mask = a_entry.view.mask;
		const std::uint32_t mainSide = MainSideBits();
		a_entry.view = a_shadow.view;
		a_entry.view.mask = (mask & mainSide) | (a_shadow.view.mask & ~mainSide);
		a_entry.shadowRow = a_shadow.shadowRow;
	}
	/** @brief A scene entry with no shadow words: what the shadow build wrote of it cleared. */
	inline void ClearShadowPart(DrawInput& a_entry)
	{
		a_entry.view.shadowKey = a_entry.view.occlusionKey[0] = a_entry.view.occlusionKey[1] = ~0u;
		a_entry.shadowRow = 0;
		a_entry.view.mask &= MainSideBits();
	}
	/**
	 * @brief A shadow input as the shadow views read it, for comparing two builds' (CheckKeptShadow): without what the main build
	 * writes of a scene entry and without the row's number (the builds number them differently).
	 */
	inline DrawInput ShadowPartOf(DrawInput a_input)
	{
		a_input.pipelineIndex = a_input.recordIndex = a_input.decalOrdinal = a_input.shadowRow = 0;
		a_input.fadeDistance = 0.0f;
		a_input.flags &= ~(kInputDrawable | kObjectDecal | kObjectForward);
		a_input.view.mask &= ~MainSideBits();
		return a_input;
	}

	/**
	 * @brief Draw inputs kept densely by object slot, at the head of an input buffer: the main segments' resident regions
	 * (ResidentRegion) and each shadow mode's (ShadowKept::Mode). The inputs are journalled like any KeptArray; removing an
	 * entry moves the last one into its place.
	 */
	struct KeptRegion
	{
		KeptArray<DrawInput> inputs;
		std::vector<std::uint32_t> indexOf;  // per object slot: its entry, or kNoRegion

		std::uint32_t EntryOf(std::size_t a_object) const { return a_object < indexOf.size() ? indexOf[a_object] : kNoRegion; }
		bool Holds(std::size_t a_object) const { return EntryOf(a_object) != kNoRegion; }
		void Cover(std::size_t a_objects)
		{
			if (indexOf.size() < a_objects)
				indexOf.resize(a_objects, kNoRegion);
		}
		/** @brief A new entry for a_object at the end, marked; returns its index. */
		std::uint32_t Add(std::uint32_t a_object, const DrawInput& a_input = {})
		{
			Cover(std::size_t(a_object) + 1);
			auto& list = inputs.Mutable();
			const auto i = static_cast<std::uint32_t>(list.size());
			list.push_back(a_input);
			indexOf[a_object] = i;
			inputs.Mark(i);
			return i;
		}
		/** @brief The entry removed, and the last one moved into its place (the same index when none moved); kNoRegion when a_object had none. */
		struct Removal
		{
			std::uint32_t at = kNoRegion, from = kNoRegion;
		};
		Removal Remove(std::uint32_t a_object)
		{
			const std::uint32_t i = EntryOf(a_object);
			if (i == kNoRegion)
				return {};
			auto& list = inputs.Mutable();
			const auto tail = static_cast<std::uint32_t>(list.size() - 1);
			if (i != tail) {
				list[i] = list[tail];
				indexOf[list[i].objectIndex] = i;
				inputs.Mark(i);
			}
			list.pop_back();
			indexOf[a_object] = kNoRegion;
			return { i, tail };
		}
	};

	/**
	 * @brief One main segment's resident draws (dclf-architecture.md, "Persistent resident draws"): the draw inputs of
	 * the resident records (Coordinator's), dense, at the head of the segment's input buffer. Written by the main build only,
	 * from the tables' change feed; the payloads share the inputs copy-on-write, and PayloadBuilder catches a backing up from
	 * its content epoch.
	 */
	struct ResidentRegion : KeptRegion
	{
		LogCursor cursor;               // the change log, and the tables generation it was read from
		std::uint8_t segments = 0;      // the segments its pairs' verdicts are of (MainBuild::Segments)
		std::vector<std::uint64_t> pairOf;   // per entry: its (material, pipeline)
		// Per entry: its sequences, 0 when it cannot be drawn this frame; a drawable decal is kRegionDecal (its sequence is in
		// its group's range, not the draws').
		std::vector<std::uint8_t> drawsOf;
		struct Pair
		{
			std::uint32_t slot = 0;   // its rows (RowsOf), once it can draw
			// The objects whose main entries use it (the entries Pairs.Inputs rewrites when its verdict moves), and its place in its
			// material's and its pipeline's lists (materialPairs, pipelinePairs), for removal without a search.
			std::vector<std::uint32_t> objects;
			std::uint32_t materialAt = 0, pipelineAt = 0;
			bool ok = true;           // this build found its rows can draw it
			// What its resolution read (its pipeline's witness and its material row's state): it is resolved again only when that changes.
			// 0: never resolved.
			std::uint64_t witness = 0;
			std::uint32_t skip = 0;  // its skip reason when it cannot draw (the report counts it every build)
			std::array<std::uint64_t, 2> frameRegisters{};  // the frame textures its resolution asked for
			// The owners the region's bundle holds for it (its material's binding block, its pipeline's shadow mask), counted in
			// ownerCounts: a resolution that finds others swaps them there.
			const void* bindingSeen = nullptr;
			const void* maskSeen = nullptr;
		};
		ankerl::unordered_dense::map<std::uint64_t, Pair> pairs;
		// The build's own inputs the pairs' resolutions read (the frame slots bound, the segment), every pair's frame textures,
		// and every pair's binding owners in one bundle, which each build's payload holds. Kept by count as pairs are resolved,
		// added and removed (frameRegisterCounts per register, ownerCounts per owner); the bundle's list is made again only when an
		// owner joins or leaves.
		std::uint64_t frameWitness = 0;
		std::array<std::uint64_t, 2> frameRegisters{};
		std::array<std::uint32_t, 128> frameRegisterCounts{};
		ankerl::unordered_dense::map<const void*, std::pair<std::shared_ptr<const void>, std::uint32_t>> ownerCounts;
		std::shared_ptr<const void> owners;
		bool ownersChanged = true;
		// Which pairs a build checks (UpdateRegionPairs): those of the materials the tables' and the lookups' material logs name
		// since its last build, the pairs new since, and the pairs of each pipeline whose witness moved (pipelinePairs). A log it
		// cannot continue, or a frame witness that moved: every pair.
		LogCursor materialCursor, lookupCursor;
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint64_t>> materialPairs;  // material -> its pairs
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint64_t>> pipelinePairs;  // pipeline -> its pairs
		std::vector<std::uint32_t> pairObjectAt;  // per object slot: its place in its pair's objects
		std::vector<std::uint64_t> freshPairs;
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> pipelineWitness;  // the last build's, per pipeline
		std::vector<std::uint64_t> pipelineWitnessNow;  // this build's, per pipeline slot (kept for its storage)
		// The pairs that cannot draw, by skip reason: the report counts them every build without visiting them.
		std::array<std::uint32_t, static_cast<std::size_t>(IndirectDraws::Skip::Count)> skipCounts{};
		ankerl::unordered_dense::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> pipelines;  // pipeline -> (set index, entries)
		std::size_t draws = 0;
		std::size_t decals = 0;               // drawable decal entries
		std::size_t undrawable = 0;           // entries with no draw this frame
		// What every stream object's stream index is relative to (the geometry slots' count; StreamGeometry) and whether the
		// stream buffer exists: when either moves, every stream object's entry is taken again. ~0: not yet read.
		std::size_t streamBase = ~std::size_t(0);
		SceneFit fit;  // what the scene's buffers held at its last build: their growth reads every slot again
		std::array<std::uint32_t, kDecalGroups> decalCount{};  // the decal ranges it was built for (MainBuild::decalCount)
		std::vector<std::uint32_t> touched;  // this build: the slots whose entry it wrote or removed, or whose log entry it read
		// The whole scene: every object the loop would draw as one input with its pair's record, not only the residents,
		// and the depth segment's cull-only candidates, decals (the colour segment's, with their ordinals) and face shapes (with
		// their position streams). Every other live object is the per-frame loop's (loopList: a decal with no ordinal or of
		// several partitions, a shape whose stream is missing), or nobody's (a candidate the segment does not submit).
		bool wholeScene = false;
		std::vector<std::uint32_t> loopList;   // the slots the loop visits
		std::vector<std::uint32_t> loopIndex;  // per slot: its place in loopList, or kNoRegion
		std::vector<std::uint8_t> candidate;   // per slot: a culling candidate only (no bindings, or shadow-only)
		std::size_t candidates = 0;

		// The scene list: per entry, which builds' parts it holds (kSideMain, kSideShadow). The shadow build writes its words
		// into the entries (BuildKeptShadow) after the main build each publication; an entry stays while either holds it.
		std::vector<std::uint8_t> sideOf;
		std::uint64_t resets = 0;  // counts Reset: the shadow build writes its parts again after one (ShadowKept::sceneResets)
		bool begun = false;        // this publication's build of the journal began (Begin; the builds ahead clear it)
		bool MainHolds(std::size_t a_object) const
		{
			const std::uint32_t i = EntryOf(a_object);
			return i != kNoRegion && (sideOf[i] & kSideMain);
		}
		/** @brief The publication's build of the inputs' journal, once whichever build writes first. */
		void Begin(std::uint64_t a_oldestContentEpoch)
		{
			if (!begun)
				inputs.BeginBuild(a_oldestContentEpoch);
			begun = true;
		}
		/** @brief A new entry holding a_side's part, its columns with it. */
		std::uint32_t AddEntry(std::uint32_t a_object, const DrawInput& a_input, std::uint8_t a_side)
		{
			const std::uint32_t i = Add(a_object, a_input);
			pairOf.push_back(kNoPair);
			drawsOf.push_back(0);
			sideOf.push_back(a_side);
			return i;
		}
		/** @brief The entry removed, its columns with it (the last entry's moved into its place). */
		void RemoveEntry(std::uint32_t a_object)
		{
			const auto removal = Remove(a_object);
			if (removal.at == kNoRegion)
				return;
			pairOf[removal.at] = pairOf[removal.from];
			drawsOf[removal.at] = drawsOf[removal.from];
			sideOf[removal.at] = sideOf[removal.from];
			pairOf.pop_back();
			drawsOf.pop_back();
			sideOf.pop_back();
		}
		/** @brief Empty, for every slot to be read again; the inputs' journal counts on (KeptArray::Clear). */
		void Reset()
		{
			auto kept = std::move(inputs);
			const std::uint64_t resetsBefore = resets;
			const bool begunBefore = begun;
			*this = ResidentRegion{};
			inputs = std::move(kept);
			inputs.Clear();
			resets = resetsBefore + 1;
			begun = begunBefore;
		}
	};

	/**
	 * @brief What the main build keeps across publications: the resident region, which both segments share (one list), and the bodies.
	 * The blocks and descriptors a draw reads are the rows' (MainRows). Used by one build at a time.
	 */
	struct BuildCache
	{
		ResidentRegion region;
		// The builds' bodies (MainBody), each held by the payloads it was built for: one no payload holds is the next build's.
		std::vector<std::shared_ptr<MainBody>> bodies;
	};

	/**
	 * @brief The main-pass epoch's draws, from the tables and the lookups: pure.
	 *
	 * Everything a draw needs from a service - the pipeline set indices, the shaders' constant tables, the
	 * descriptor indices of textures and samplers - comes from the lookups;
	 * an entry the render thread has not resolved yet defers the draw (deferredTextures), and the frame's
	 * own textures are left as patches for the commit. The per-frame constant blocks are addressed by
	 * their fixed slots (FrameSlotOffset) for the slots the inputs say the commit supplies.
	 */
	void BuildMainPayload(const MainInputs& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups, MainPayload& a_out, MainRows& a_rows,
		BuildCache* a_cache = nullptr, std::shared_ptr<const StreamViews> a_streams = nullptr);
	/**
	 * @brief Both main segments' payloads in one build: one list, which both read through their view bits, and a member
	 * drawable in both or neither. a_in[j] / a_out[j] per segment (kAsyncColour, kAsyncZPrepass); a segment with no inputs is not built.
	 */
	void BuildMainPayloads(const std::array<const MainInputs*, 2>& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
		const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache, std::shared_ptr<const StreamViews> a_streams);
	/**
	 * @brief BuildMainPayloads in two stages, so the stream views (MakeStreamViews) can be made beside it: Run is the whole build but
	 * what reads the views, Finish takes them. The snapshot builder's (RunAhead); Run and Finish once each, in order, on any one thread at
	 * a time. Nothing is built when neither segment has inputs.
	 */
	class MainBuildStages
	{
	public:
		MainBuildStages(const std::array<const MainInputs*, 2>& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
			const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache);
		~MainBuildStages();
		MainBuildStages(const MainBuildStages&) = delete;
		MainBuildStages& operator=(const MainBuildStages&) = delete;
		void Run();
		void Finish(std::shared_ptr<const StreamViews> a_streams);

	private:
		struct State;
		std::unique_ptr<State> state;
	};

	// The cascades' render mode (0xE, ShadowMapClamped) as an index of the shadow modes.
	constexpr std::uint32_t kSunShadowMode = 0xE - PassCapture::kFirstShadowMode;
	// The point lights' render mode (0xF, ShadowMapParabolic) as an index of the shadow modes.
	constexpr std::uint32_t kParabolicShadowMode = 0xF - PassCapture::kFirstShadowMode;
}
