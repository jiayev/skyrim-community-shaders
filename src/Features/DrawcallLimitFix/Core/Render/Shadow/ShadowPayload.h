#pragma once

// The shadow payload the shadow build (DCLF's pool) makes for the shadow epoch (its shadow views and occlusion maps), which its commit
// reads on the render thread, and the inputs its shapes are made from. Included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief A row of the shadow views' material table: the Utility vertex shader's PerMaterial block (b1, its texture
	 * offset), which the draw's pushed address names directly, and the diffuse's descriptor index for t0 after it
	 * (kShadowRowDiffuseOffset). One constant-buffer block (256 bytes): the table is an array of them. Row 0 is every caster
	 * without alpha testing (no offset, the null texture).
	 */
	struct ShadowMaterialRow
	{
		std::array<float, 4> texcoord{};  // offset xy, scale zw
		std::uint32_t diffuse = 0;
		std::array<std::uint32_t, 59> unused{};

		bool operator==(const ShadowMaterialRow& o) const { return texcoord == o.texcoord && diffuse == o.diffuse; }
	};
	static_assert(sizeof(ShadowMaterialRow) == kConstantAlignment && offsetof(ShadowMaterialRow, diffuse) == kShadowRowDiffuseOffset);

	/**
	 * @brief A shadow render mode's view rasterizer states this frame (DrawPipelines::ShadowRasterStateId), ascending: those
	 * of the views of ordinary casters, and those of the views of the volumetric lighting copy, which draw the
	 * volumetric-only casters alone (kObjectVolumetricOnly). A caster is an input only when its pipeline is ready under
	 * every state of the views that draw its class.
	 */
	struct ModeRasterStates
	{
		std::vector<std::uint32_t> casters, volumetric;

		void Add(std::uint32_t a_state, bool a_volumetric)
		{
			auto& ids = a_volumetric ? volumetric : casters;
			if (const auto it = std::lower_bound(ids.begin(), ids.end(), a_state); it == ids.end() || *it != a_state)
				ids.insert(it, a_state);
		}
		const std::vector<std::uint32_t>& Of(bool a_volumetric) const { return a_volumetric ? volumetric : casters; }
		bool Empty() const { return casters.empty() && volumetric.empty(); }
		/** @brief Every state of either class, once each. */
		std::vector<std::uint32_t> All() const
		{
			std::vector<std::uint32_t> all;
			std::set_union(casters.begin(), casters.end(), volumetric.begin(), volumetric.end(), std::back_inserter(all));
			return all;
		}
		bool operator==(const ModeRasterStates&) const = default;
	};

	struct ShadowInputs
	{
		std::uint32_t frameNumber = 0;
		std::array<bool, kShadowModeCount> modeUsed{};
		// Per mode, the rasterizer states of its views.
		std::array<ModeRasterStates, kShadowModeCount> modeRasterStates{};
		// The sun's full-frustum culling processes, as the full-frustum cull (FUN_141511f30) has just used them: an object
		// whose entry (SceneTables::listedRoot) is outside every one is no candidate of the sun's cascade culls. However
		// many there are: the sun views' latches name them in a shared region of the latch block (ReserveShadowLatch).
		std::vector<SunEntryProcess> sunEntryProcesses;
		// The sun entries DCLF can take out of the cascade culls (the Sun source's accumulation): the build turns them into the next
		// frame's exclusion.
		std::shared_ptr<const EntrySpace> sunCandidates;
		// And those it could take out of the point lights' culls (the Point source's): the paraboloid exclusion's.
		std::shared_ptr<const EntrySpace> lightCandidates;
		ResourceAddresses addresses{};
		// Community Shaders' SharedData (b5) and FeatureData (b6), copied from the structs CS keeps.
		std::vector<std::byte> sharedData, featureData;
		std::uint32_t lookupGeneration = 0, tablesGeneration = 0;
		TablesContentEpochs tablesOldestContentEpochs;  // PayloadBuilder::OldestContentEpoch of each kept table
		// What the shadow journals keep changes back to (a build ahead's: the payload pools' oldest content epochs; else 0, kept whole).
		std::uint64_t inputsOldestContentEpoch = 0;  // the shadow list's
		std::uint64_t materialRowsOldestContentEpoch = 0;
	};

	struct ShadowPayload
	{
		ShadowInputs inputs;
		std::vector<std::shared_ptr<const void>> bindingOwners;
		std::shared_ptr<const StreamViews> streams;  // the frame's stream views the build read
		ConstantArena arena;  // the view slots' head and the frame record are reserved; the commit writes the views into it
		DrawBindings frameRecord{};  // every view's textures and samplers but the diffuse (kShadowFrameRecordOffset)
		// Process-unique per build (Reset): the arena's bytes past the frame record are this build's, which a commit stages only into a
		// constants buffer that does not hold them yet.
		std::uint64_t arenaSerial = 0;
		// The rows the inputs name: the kept state's (journalled), or the build's own (content epoch 0: sent whole).
		KeptView<ShadowMaterialRow> materialRows;
		// Per mode, the draws its inputs can produce (a skin draws once per partition): its views' max count.
		std::array<std::uint32_t, kShadowModeCount> modeDraws{};
		// Per mode, the same by key slot (DrawInput::pipelineIndex): what sizes a view's buckets (ShadowBucket).
		std::array<std::vector<std::uint32_t>, kShadowModeCount> keySlotDraws;
		std::vector<std::uint32_t> objectRecord;  // per object: its material row, or ~0u when it cannot draw
		// The one shadow list: an input per caster or occluder, for every mode it is drawn in at once, its view mask the
		// views of those modes (ModeViewBits). The kept state's region (ShadowKept::scene) at the head of the buffer, sent as what
		// changed since the content epoch the buffer holds, then the frame's own (face shapes; everything without the kept state).
		bool kept = false;
		KeptView<DrawInput> regionInputs;
		std::vector<DrawInput> inputList;
		std::array<std::uint32_t, kShadowModeCount> modeBits{};    // per mode: its views' bits (ModeViewBits), what selects its inputs
		std::array<std::uint32_t, kShadowModeCount> modeInputs{};  // per mode: its inputs (CountModeDraws)
		// Per mode, the build version at which an object last joined or left its inputs (ShadowKept::Mode::membership);
		// 0 without the kept state.
		std::array<std::uint64_t, kShadowModeCount> membership{};
		// Per mode, the objects that joined and left its region inputs (ShadowKept::heldModes) in this kept build (keptBuild, the
		// ShadowKept::build it was): what the exclusions' inputs follow (BuildSunExclusion) instead of reading the region again. A mode
		// the build retook whole (a resync, its view states changed) has movesValid false: its inputs are read whole. 0 and empty
		// without the kept state.
		std::uint64_t keptBuild = 0;
		std::array<std::vector<std::uint32_t>, kShadowModeCount> joined, left;
		std::array<bool, kShadowModeCount> movesValid{};
		// The scene list: the main payloads' tail (their frame's own inputs) sits between the region and this one's, all one
		// buffer; the shadow views scan all of it (their bits select their own).
		std::size_t mainTail = 0;
		std::size_t RegionCount() const { return regionInputs.Count(); }
		/** @brief The list's inputs: what every view's dispatch scans (its view bits select its own). */
		std::size_t Inputs() const { return RegionCount() + mainTail + inputList.size(); }
		std::size_t ModeInputs(std::uint32_t a_mode) const { return modeInputs[a_mode]; }
		template <class F>
		void ForEachInput(F&& a_visit) const
		{
			if (regionInputs.elements)
				for (const auto& input : *regionInputs.elements)
					a_visit(input);
			for (const auto& input : inputList)
				a_visit(input);
		}
		/** @brief The mode's inputs: the list's whose mask has its views' bits. */
		template <class F>
		void ForEachInput(std::uint32_t a_mode, F&& a_visit) const
		{
			const std::uint32_t bits = modeBits[a_mode];
			ForEachInput([&](const DrawInput& a_input) {
				if (a_input.view.mask & bits)
					a_visit(a_input);
			});
		}
		/** @brief The mode's inputs as one list (the diagnostics' copy). */
		std::vector<DrawInput> Flat(std::uint32_t a_mode) const
		{
			std::vector<DrawInput> out;
			out.reserve(ModeInputs(a_mode));
			ForEachInput(a_mode, [&](const DrawInput& a_input) { out.push_back(a_input); });
			return out;
		}
		GeometryDrawsOut geometries;
		// The tables' vertex stream rows, for the commit's uploads: the publication's tables', which the draws holding the payload hold
		// (DrawPublication::tables), as the main payload's.
		std::span<const Api::VertexStreamRow> streamRows;
		std::uint32_t skippedTexture = 0, skippedPipeline = 0, deferredTextures = 0, deferredPipelines = 0;
		// Set members a mode could not draw (waiting on a pipeline or a texture): a defect, the engine having withheld them.
		std::uint32_t setWaiting = 0;
		std::string setWaitingFirst;  // the first, and why
		// The build ahead also builds the next frame's exclusions, which the epoch publishes; empty for a build made at the epoch,
		// which builds them at the publish.
		// The exclusion contribution's own type, opaque here (Api::ShadowExclusionContribution: built by build, handed back by publish).
		std::shared_ptr<void> sunExclusion;  // from the cascades' mode
		std::shared_ptr<void> parabolicExclusion;  // and from the paraboloid mode (the Point source's culls)

		void Reset()
		{
			setWaiting = 0;
			setWaitingFirst.clear();
			sunExclusion.reset();
			parabolicExclusion.reset();
			arena.Reset(kShadowConstantBytes);
			frameRecord = {};
			static std::atomic<std::uint64_t> arenaSerials{ 0 };
			arenaSerial = arenaSerials.fetch_add(1, std::memory_order_relaxed) + 1;
			materialRows.Reset();
			modeDraws = {};
			for (auto& slots : keySlotDraws)
				slots.clear();
			bindingOwners.clear();
			objectRecord.clear();
			inputList.clear();
			kept = false;
			regionInputs = {};
			mainTail = 0;
			modeBits = {};
			modeInputs = {};
			membership = {};
			keptBuild = 0;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				joined[m].clear();
				left[m].clear();
			}
			movesValid = {};
			geometries.Reset();
			streams.reset();
			streamRows = {};
			skippedTexture = skippedPipeline = deferredTextures = deferredPipelines = 0;
		}
	};

	/**
	 * @brief A shadow or occlusion view as its epoch's shape names it: its slot, mode, target, slice and rectangle, and the
	 * rasterizer state whose pipeline map row its buckets are (ShadowRowBuckets). What the shape producer (MakeShadowShape) makes
	 * the rest of the view from; a frame's views are one of the layouts its revision has seen (the recent shapes).
	 */
	struct ShadowViewLayout
	{
		std::uint32_t slot = 0, modeIndex = 0, target = 0, slice = 0;
		std::uint32_t x = 0, y = 0, width = 0, height = 0;
		std::uint32_t rasterState = 0;
		bool operator==(const ShadowViewLayout&) const = default;
	};
	std::vector<ShadowViewLayout> LayoutOf(const ShadowFrame& a_frame);

	/**
	 * @brief Per mode, the draws the scene's objects can cast in it, in all and per key slot (DrawBoundStore::modeKeyDraws through
	 * the lookups' key slots; ShadowBoundsOf): a bound on any frame's payload (ShadowPayload::modeDraws, keySlotDraws) while the
	 * scene's objects stand.
	 */
	struct ShadowBounds
	{
		std::array<std::uint32_t, kShadowModeCount> modeDraws{};
		std::array<std::vector<std::uint32_t>, kShadowModeCount> keySlotDraws;
	};
	ShadowBounds ShadowBoundsOf(const DrawBoundStore& a_bound, const Lookups& a_lookups);

	/**
	 * @brief What the shadow epoch's shape is made from (MakeShadowShape): the views' layout (the occlusion maps' after the placements),
	 * each view's map row's buckets, and the revision's payload (its modes' and key slots' draws, the arena's blocks), with the slots'
	 * resources.
	 */
	struct ShadowShapeInputs
	{
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		ShadowIndirectState indirect{};
		std::vector<ShadowViewLayout> views;
		std::vector<RowBuckets> rows;  // per view
		const ShadowPayload* payload = nullptr;
		// What the capacities are sized for, with the payload's draws: the scene's (the revision's draws stay within them).
		const ShadowBounds* bounds = nullptr;
		// What the shadow buffers' named versions hold, and the view blocks' address (the revision's set).
		ShadowCapacity capacity;
		std::uint64_t viewBlocks = 0;
		std::shared_ptr<const ShadowFrame> previous;  // the last published shape: its slots' capacities only grow
		LatchedList latched;
		// The latch block, its layout and the counters' zeros (the blocks the revision's set names: NamedVersions::Latch; a commit's
		// parity names its revision's).
		std::shared_ptr<const org::LatchBlock> latch, zeros;
		ShadowLatchLayout latchLayout;
		std::shared_ptr<const std::vector<ShadowSlotEntry>> slotLayouts;  // ShadowFrame::slotLayouts
	};
	std::shared_ptr<ShadowFrame> MakeShadowShape(const ShadowResources& a_resources, const ShadowShapeInputs& a_in);
	/**
	 * @brief The shadow commit's latched copies (CS's SharedData and FeatureData at their places in the constants, ShadowArenaBlocksOf),
	 * as its LatchedUploads makes them.
	 */
	std::vector<LatchedCopy> ShadowLatchedLayout(const ShadowResources& a_resources);
	/**
	 * @brief The blocks every shadow view's push data names besides its own (kShadowPushZeros and after), as offsets into the shadow
	 * constants: the zero block, then CS's SharedData and FeatureData at the places their sizes (fixed for the session) give. A
	 * block CS does not have reads the zero block.
	 */
	struct ShadowArenaBlocks
	{
		std::uint64_t zeros = 0, sharedData = 0, featureData = 0;
		std::uint32_t sharedBytes = 0, featureBytes = 0;
	};
	ShadowArenaBlocks ShadowArenaBlocksOf();
}
