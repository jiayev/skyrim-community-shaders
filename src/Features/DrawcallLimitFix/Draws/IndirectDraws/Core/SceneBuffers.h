#pragma once

// The scene's tables on the GPU, shared by every epoch, and their uploads.
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief The scene tables' capacities (Impl::ReserveSceneTables), changed with their buffers' growth as one (Growths::Change):
	 * what the commits send within and the builds bound their objects by (SceneFit). The palettes are FrameValues', in buffers of
	 * the frame's own.
	 */
	struct SceneSizing
	{
		std::uint32_t objectCapacity = 0, geometryRows = 0, extraRows = 0, faceVertices = 0;
		std::uint32_t treeCapacity = 0, fadeRootCapacity = 0, fadeEventCapacity = 0;
		std::uint32_t visibilityEventCapacity = 0;  // T7b1's event list (0: no visibility readback)
		std::uint32_t portalWords = 0;  // the portal programs' words (PortalViews::Encoded)
		std::uint32_t treeLodShapeCapacity = 0, treeLodMeshCapacity = 0;
		bool operator==(const SceneSizing&) const = default;
	};

	struct SceneBuffers : SceneSizing
	{
		Versioned objects, extras, geometries, facePositions;
		// The index pool every plain indexed draw binds (IndexPool): the shadow views' and the Z-prepass's, made by whichever
		// sets up first (EnsureIndexPool) and kept current by every commit that draws from it (UpdateIndexPool).
		std::shared_ptr<IndexPool> pool;
		// Their SRVs' descriptor heap indices, and the positions' address: a growth gives the buffer new ones (the old ones
		// are retired once the GPU is done with them), so every build takes them from here, after ReserveSceneTables.
		std::uint32_t objectsIndex = 0, extrasIndex = 0;
		std::uint64_t facePositionsAddress = 0;
		// What each holds (SceneSizing: GpuLayouts.h, kInitialObjects), grown by ReserveSceneTables; `generation` counts the
		// growths, so a batch staged before one is not submitted after it.
		std::uint64_t generation = 0;
		// The same count, for the invocation revisions of the passes that bind these buffers (read on the graph host's thread,
		// which prepares invocations ahead of the commits).
		std::atomic<std::uint64_t> layout{ 0 };
		std::uint32_t growths = 0;  // since the last report
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> faceUploaded;  // region -> generation (render thread)
		// The versions of the stores' records, rows and slots the buffers hold, written by the commit that uploads them.
		TablesHeld held;
		// Tree wind (TreeWindCS; Records.h, TreeStatic): the tree rows and their clocks by tree slot, and the members drawing
		// under a tree. The rows and the list are the commits' uploads (UploadTrees), against the tables' versions; the
		// clocks are the GPU's alone. The counts and the frame's inputs are what the passes dispatch with (the last commit's).
		Versioned trees, treeClocks;
		std::shared_ptr<org::Buffer> treeFrameBuffer;  // TreeWindFrameRow, one row, every commit's
		// The trees' wind (TreeWindCS), by entry (kTreeWindEntryRows float4 rows: the nodeless trees', then tree slot s at
		// s + 1), one buffer per frame parity: the Z-prepass epoch's compute writes the frame's, and every draw reads the
		// other's through the frame record (kTreeWindRegister, treeWindIndex). Zeroed once per backing (treeWindZeroed): a
		// zero generation is never a listing's, so a record draws its own values until its tree's entry is written.
		std::array<Versioned, 2> treeWindRows;
		std::array<std::uint32_t, 2> treeWindIndex{};
		bool treeWindZeroed = false;
		/** @brief The wind buffer a frame's draws read: the one the frame before wrote (TreeWindCS writes by the frame's parity). */
		std::uint32_t TreeWindReadIndex(std::uint32_t a_frame) const { return treeWindIndex[(a_frame + 1) & 1]; }
		std::uint64_t treesHeld = ~0ull;
		std::uint32_t treeCount = 0;
		std::uint32_t treeFrame = 0;  // the scene frame of treeInputs
		TreeWindFrame treeInputs{};
		// The frame before's: what wrote the buffer this frame's draws read (TreeWindReadIndex). ~0u until a frame has run.
		std::uint32_t previousTreeFrame = ~0u;
		TreeWindFrame previousTreeInputs{};
		std::uint64_t previousTreesHeld = ~0ull;  // the tree rows that frame's pass read
		std::uint64_t frameTreesHeld = ~0ull;     // this frame's
		std::shared_ptr<const ComputeProgram> treeWind;
		// Fade roots (FadeStateCS; Records.h, FadeRootStatic): the static rows by root slot (the commits' uploads, the runs
		// Tables::fadeRootsJournal names since fadeRootsHeld), the GPU's state rows, the frame's inputs (a one-row buffer the depth commit writes once
		// a frame), and CS_DCLF_FADE_PARITY's log (kFadeLogEntries roots from fadeLogBase, ~0u: none this frame).
		Versioned fadeRoots, fadeStates;
		std::shared_ptr<org::Buffer> fadeFrameBuffer, fadeLog;
		// The main camera's portal programs (Records.h, kPortal*; PortalViews::Encoded), the depth commit's, and what the buffer
		// holds as the commits sent it (the words that differ go up); a new version holds nothing.
		Versioned portalPrograms;
		std::vector<std::uint32_t> portalProgramsSent;
		Versioned fadeRootPrograms;  // per root slot: its program (PortalViews::ProgramOf)
		Versioned fadeAnimated;    // per root slot: the scene frame whose animation batch updated it
		// What fadeAnimated holds, as the commits wrote it (the GPU only reads it): a frame's words go up in runs of the roots it
		// stamped, runs closer than kFadeRunGap words merged, rather than a copy per root (about a hundred a frame at the bridge)
		// or the span from the lowest to the highest (16 KB a frame in motion).
		std::vector<std::uint32_t> fadeAnimatedMirror;
		// What fadeRootPrograms holds, as the commits sent it: the runs that differ go up, not the whole.
		std::vector<std::uint32_t> fadeRootProgramsMirror;
		std::uint64_t fadeRootProgramsHeld = ~0ull;  // the root and program versions it holds
		// The states FadeStateCS publishes, one buffer per scene frame parity (the state rows are its own): the builds read the
		// frame before's (FadeStatesReadIndex, through their latches). Zeroed once per backing (frameAheadZeroed): a zero
		// generation is never a listing's, and the builds take the static row's state for it.
		std::array<Versioned, 2> fadeStatesOut;
		std::array<std::uint32_t, 2> fadeStatesOutIndex{};
		std::uint32_t fadeRootsIndex = 0;  // fadeRoots' descriptor (the frame record's t119: kFadeRootsRegister)
		bool fadeStatesOutZeroed = false;
		/** @brief The published states a frame's builds read: the frame before's (FadeStateCS writes by the frame's parity). */
		std::uint32_t FadeStatesReadIndex(std::uint32_t a_frame) const { return fadeStatesOutIndex[(a_frame + 1) & 1]; }
		std::uint64_t fadeRootsHeld = ~0ull;
		std::uint32_t fadeRootCount = 0;
		std::uint32_t fadeFrameNumber = 0;  // the scene frame of fadeFrame
		FadeFrame fadeFrame{};
		std::uint32_t fadeLogBase = ~0u;
		std::shared_ptr<const ComputeProgram> fadeState;
		// The fade write-back: the event list (a count word, then FadeEvents; the count zeroed by every depth commit), the
		// events it holds, each root's last reported generation and milestone (zeroed once per backing), and the host side.
		Versioned fadeEvents, fadeReported;
		bool fadeReportedZeroed = false;
		std::shared_ptr<FadeWriteBack> fadeWriteBack;
		// T7b1 (CS_DCLF_ACCUMULATED_PARITY; null without it): the visibility events (VisibilityEventsCS: a header, then
		// VisibilityEvents; the count zeroed by every depth commit), each root's and each object slot's last reported verdict
		// (zeroed once per backing: visibilityReportedZeroed, which also starts the decoder again), and the host side.
		Versioned visibilityEvents, visibilityRootReported, visibilityObjectReported;
		bool visibilityReportedZeroed = false;
		std::shared_ptr<const ComputeProgram> visibilityEventsProgram;
		std::shared_ptr<VisibilityReadback> visibilityReadback;
		// Tree LOD (dclf-lod.md, "Tree LOD: the draws"; Scene/TreeLod.h): the shape rows, the instance records (75 a shape
		// slot), the mesh rows, the draw row and the visible list (the draw's arguments, then the culled records), every one read
		// through its address by the draws' vertex stage. The depth commit uploads what the mirror changed (UploadTreeLod);
		// TreeLodCullPass fills the list in the depth epoch, and both main passes draw it once the pipelines exist. treeLodReady: the
		// last depth commit's draw row named its slots (the frame draws tree LOD).
		Versioned treeLodShapes, treeLodInstances, treeLodMeshes, treeLodVisible;
		std::shared_ptr<org::Buffer> treeLodDraw;
		std::uint64_t treeLodInstancesAddress = 0, treeLodShapesAddress = 0, treeLodMeshesAddress = 0, treeLodDrawAddress = 0, treeLodVisibleAddress = 0;
		std::shared_ptr<const ComputeProgram> treeLodCull;
		// The pipelines the passes record with, published by the depth commit once built (read on the graph host's thread).
		std::atomic<std::shared_ptr<const TreeLodPipelines>> treeLodPipelines;
		std::atomic<bool> treeLodReady{ false };
		std::uint64_t treeLodUploads = 0, treeLodSlotsSent = 0;  // since the last report
		/**
		 * @brief Tree LOD's cull counts (TreeLodReadbackPass): the list's header copied to a host buffer per frame slot after the
		 * depth segment's second phase, read when the slot comes round again (the graph host's thread), summed for the report.
		 */
		struct TreeLodCounts
		{
			std::vector<std::shared_ptr<org::Buffer>> readback;
			std::vector<std::uint8_t> filled;
			std::atomic<std::uint64_t> frames{ 0 }, phaseOne{ 0 }, retests{ 0 }, phaseTwo{ 0 };
		};
		std::shared_ptr<TreeLodCounts> treeLodCounts;
		// The draw row the last depth commit uploaded (render thread): the reflection's faces draw from the same tables with
		// their own lists (ExecuteReflection).
		TreeLod::DrawRow treeLodRow{};
	};

	/**
	 * @brief Tree LOD's texture and sampler into a_row (UploadTreeLod): the engine's tree LOD atlas (BSDistantTreeShader::
	 * SetupTechnique's static at 0x1433dcd18) and slot 0's sampler as its draws leave it (address mode 0, filter 2). The bindings'
	 * owners go to the execution. False when there is no texture.
	 */
	inline bool TreeLodTextureBinding(TreeLod::DrawRow& a_row, std::vector<std::shared_ptr<const void>>& a_owners)
	{
		const auto* texture = Engine::Global<RE::NiSourceTexture*>(Engine::Address::kTreeLodAtlas);
		auto* view = texture && texture->rendererTexture ? texture->rendererTexture->resourceView : nullptr;
		if (!view)
			return false;
		auto& textures = GpuTextures::Get();
		auto textureBinding = textures.ResolveBinding(view, 0);
		auto samplerBinding = textures.SamplerBinding(0, 2);
		if (textureBinding.index == GpuTextures::kInvalid || samplerBinding.index == GpuTextures::kInvalid)
			return false;
		a_owners.push_back(std::move(textureBinding.owner));
		a_owners.push_back(std::move(samplerBinding.owner));
		a_row.textureIndex = textureBinding.index;
		a_row.samplerIndex = samplerBinding.index;
		return true;
	}

	/**
	 * @brief The depth commit's tree LOD uploads (render thread): the shape slots and meshes the mirror changed since the last
	 * commit, the draw row (the tables' addresses, the frame's texture and sampler) and the list's arguments with no instances.
	 * The leases of meshes no shape draws any more, and the texture's and sampler's bindings, go to the execution (a_owners).
	 */
	template <class Uploads>
	void UploadTreeLod(TreeLod::Mirror& a_mirror, SceneBuffers& a_scene, Uploads& a_uploads, std::vector<std::shared_ptr<const void>>& a_owners, bool a_draw)
	{
		a_scene.treeLodReady.store(false, std::memory_order_release);
		if (!a_scene.treeLodCull || !a_scene.treeLodShapes)
			return;
		for (auto& owner : a_mirror.TakeRetired())
			a_owners.push_back(std::move(owner));
		// Reserved before the epoch (ReserveSceneTables); a mirror past them waits a commit, its changes kept, drawing nothing.
		std::vector<std::uint32_t> slots, meshes;
		const bool fits = a_mirror.ShapeSlots() <= a_scene.treeLodShapeCapacity && a_mirror.MeshSlots() <= a_scene.treeLodMeshCapacity;
		if (!fits)
			a_draw = false;
		else
			a_mirror.TakeChanges(slots, meshes);
		for (const std::uint32_t slot : slots) {
			const auto& row = a_mirror.SlotRow(slot);
			a_uploads(a_scene.treeLodShapes, &row, sizeof(row), std::uint64_t(slot) * sizeof(TreeLod::ShapeRow));
			if (const auto* instances = a_mirror.SlotInstances(slot); instances && !instances->empty())
				a_uploads(a_scene.treeLodInstances, instances->data(), instances->size() * sizeof(TreeLod::Instance),
					std::uint64_t(slot) * TreeLod::kMaxGroupInstances * sizeof(TreeLod::Instance));
		}
		for (const std::uint32_t mesh : meshes) {
			const auto& row = a_mirror.MeshSlotRow(mesh);
			a_uploads(a_scene.treeLodMeshes, &row, sizeof(row), std::uint64_t(mesh) * sizeof(TreeLod::MeshRow));
		}
		a_scene.treeLodSlotsSent += slots.size();
		++a_scene.treeLodUploads;
		// The draw row and the list's header with no instances, every depth commit. The cull appends only while the row names
		// shape slots: 0 when tree LOD does not draw this frame (toggle off, no texture), so both passes draw nothing together.
		TreeLod::DrawRow row{};
		row.instances = a_scene.treeLodInstancesAddress;
		row.shapes = a_scene.treeLodShapesAddress;
		row.meshes = a_scene.treeLodMeshesAddress;
		row.visible = a_scene.treeLodVisibleAddress + TreeLod::kVisibleHeaderWords * sizeof(std::uint32_t);
		row.alphaRef = 128.0f / 255.0f;
		row.maxIndices = std::max(a_mirror.MaxIndices(), 1u);
		// The engine's texture for its tree LOD draws (the worldspace's tree LOD atlas, TreeLodTextureBinding) and the alpha
		// reference they draw with (128/255): measured at the engine's draws (dclf-lod.md, "Tree LOD: what the engine does").
		if (a_draw && a_mirror.MaxIndices() && TreeLodTextureBinding(row, a_owners))
			row.shapeSlots = a_scene.treeLodShapeCapacity;
		a_uploads(a_scene.treeLodDraw, &row, sizeof(row), 0);
		a_scene.treeLodRow = row;
		TreeLod::VisibleHeader header{};
		header.phaseOne[0] = header.phaseTwo[0] = header.colour[0] = row.maxIndices;
		a_uploads(a_scene.treeLodVisible, &header, sizeof(header), 0);
		if (row.shapeSlots)
			a_scene.treeLodReady.store(true, std::memory_order_release);
	}

	/**
	 * @brief Sorted word indices as runs, a_send(first, count), indices closer than kFadeRunGap merged into one run (a few words
	 * sent again rather than a copy each).
	 */
	constexpr std::uint32_t kFadeRunGap = 32;
	template <class Send>
	void SendWordRuns(const std::vector<std::uint32_t>& a_sorted, Send&& a_send)
	{
		for (std::size_t i = 0; i < a_sorted.size();) {
			const std::uint32_t first = a_sorted[i];
			std::uint32_t lastIndex = first;
			while (++i < a_sorted.size() && a_sorted[i] - lastIndex <= kFadeRunGap)
				lastIndex = a_sorted[i];
			a_send(first, lastIndex - first + 1);
		}
	}

	/**
	 * @brief The depth commit's fade uploads (render thread): the static rows the buffer lacks (the journal's runs since the
	 * version it holds; all of them for a new buffer), and the frame's inputs once a frame: of the list processes' cull tests, the
	 * parts of each block its counts use, where they changed. a_logBase: the parity log's first root this frame (~0u: none).
	 */
	template <class Uploads>
	void UploadFadeRoots(const SceneTables& a_tables, std::uint32_t a_frame, const FadeFrame& a_inputs, std::uint32_t a_logBase, SceneBuffers& a_scene,
		Uploads& a_uploads, const std::vector<std::uint32_t>& a_programs)
	{
		if (!a_scene.fadeState || !a_scene.fadeRoots)
			return;
		const std::uint64_t version = a_tables.FadeRootsVersion();
		if (a_scene.fadeRootsHeld != version && a_tables.fadeRoots.size() <= a_scene.fadeRootCapacity) {
			const auto* rows = a_tables.fadeRoots.data();
			a_tables.fadeRootsJournal.Take().ForEachRun(a_scene.fadeRootsHeld, a_tables.fadeRoots.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
				a_uploads(a_scene.fadeRoots, rows + a_first, static_cast<std::size_t>(a_count * sizeof(FadeRootStatic)), a_first * sizeof(FadeRootStatic));
			});
			a_scene.fadeRootsHeld = version;
		}
		a_scene.fadeRootCount = a_scene.fadeRootsHeld == version ? static_cast<std::uint32_t>(a_tables.fadeRoots.size()) : 0u;
		if (a_scene.fadeFrameNumber != a_frame) {
			a_scene.fadeFrameNumber = a_frame;
			a_scene.fadeFrame = a_inputs;
			// The frame's portal programs (PortalViews::Encoded), the words that differ from what the buffer holds. Past the
			// buffer's words (its growth is in flight), a header of no programs: the roots take the frustum alone that frame.
			if (a_scene.portalPrograms) {
				static const std::uint32_t kNone[kPortalHeaderBytes / 4]{};
				const bool fits = a_programs.size() <= a_scene.portalWords;
				const auto* words = fits ? a_programs.data() : kNone;
				const std::size_t count = fits ? a_programs.size() : std::size(kNone);
				auto& sent = a_scene.portalProgramsSent;
				std::vector<std::uint32_t> differing;
				for (std::uint32_t w = 0; w < count; ++w)
					if (w >= sent.size() || sent[w] != words[w])
						differing.push_back(w);
				SendWordRuns(differing, [&](std::uint32_t a_first, std::uint32_t a_count) {
					a_uploads(a_scene.portalPrograms, words + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
				sent.assign(words, words + count);
			}
			a_scene.fadeLogBase = a_logBase;
		}
		// The frame row, with the pass's per-frame values, every commit.
		a_scene.fadeFrame.rootCount = a_scene.fadeRootCount;
		a_scene.fadeFrame.sceneFrame = a_scene.fadeFrameNumber;
		a_scene.fadeFrame.logBase = a_scene.fadeLogBase;
		a_uploads(a_scene.fadeFrameBuffer, &a_scene.fadeFrame, sizeof(FadeFrame), 0);
		// The write-back's header (the count, and the frame its harvest matches) is ArmFadeReadback's (T7b6).
	}

	/**
	 * @brief A commit's tree uploads (render thread): the rows and the list where the buffers do not hold the tables'
	 * versions, whole (they change when a tree member joins or leaves), and the frame's inputs once a frame. a_frameRow: the
	 * frame row too, for the epoch whose TreeWindPass reads it (the Z-prepass's: no other pass reads it).
	 */
	/**
	 * @brief A commit's zeroing of the frame-ahead outputs (the trees' wind, the published fade states) in a new backing, before
	 * any epoch's draws or builds read them: whichever epoch commits first after a growth.
	 */
	template <class Uploads>
	void ZeroFrameAheadOutputs(SceneBuffers& a_scene, Uploads& a_uploads)
	{
		if (!a_scene.treeWindZeroed && a_scene.treeWindRows[0]) {
			const std::vector<std::byte> zeros(std::size_t(a_scene.treeCapacity + 1) * kTreeWindEntryRows * 16);
			for (const auto& rows : a_scene.treeWindRows)
				a_uploads(rows, zeros.data(), zeros.size(), 0);
			a_scene.treeWindZeroed = true;
		}
		if (!a_scene.fadeStatesOutZeroed && a_scene.fadeStatesOut[0]) {
			const std::vector<std::byte> zeros(std::size_t(a_scene.fadeRootCapacity) * sizeof(FadeNodeState));
			for (const auto& states : a_scene.fadeStatesOut)
				a_uploads(states, zeros.data(), zeros.size(), 0);
			a_scene.fadeStatesOutZeroed = true;
		}
		// No root reported yet: a zero generation is never a listing's.
		if (!a_scene.fadeReportedZeroed && a_scene.fadeReported) {
			const std::vector<std::byte> zeros(std::size_t(a_scene.fadeRootCapacity) * 2 * sizeof(std::uint32_t));
			a_uploads(a_scene.fadeReported, zeros.data(), zeros.size(), 0);
			a_scene.fadeReportedZeroed = true;
		}
	}

	template <class Uploads>
	void UploadTrees(const SceneTables& a_tables, std::uint32_t a_frame, SceneBuffers& a_scene, Uploads& a_uploads, bool a_frameRow)
	{
		if (!a_scene.treeWind || !a_scene.trees)
			return;
		if (a_scene.treesHeld != a_tables.treesVersion && a_tables.trees.size() <= a_scene.treeCapacity) {
			if (!a_tables.trees.empty())
				a_uploads(a_scene.trees, a_tables.trees.data(), a_tables.trees.size() * sizeof(TreeStatic), 0);
			a_scene.treesHeld = a_tables.treesVersion;
		}
		a_scene.treeCount = a_scene.treesHeld == a_tables.treesVersion ? static_cast<std::uint32_t>(a_tables.trees.size()) : 0u;
		a_scene.frameTreesHeld = a_scene.treesHeld;
		if (a_scene.treeFrame != a_frame) {
			a_scene.previousTreeFrame = a_scene.treeFrame;
			a_scene.previousTreeInputs = a_scene.treeInputs;
			a_scene.previousTreesHeld = a_scene.frameTreesHeld;
			a_scene.treeFrame = a_frame;
			a_scene.treeInputs = SampleTreeWindFrame();
			// The trees' clocks stand still (Policy::FreezeTreeWind).
			if (Policy::FreezeTreeWind())
				a_scene.treeInputs.deltaTime = 0.0f;
		}
		// The frame row (TreeWindFrameRow), every Z-prepass commit: the pass's invocation is prepared ahead of it.
		if (a_frameRow && a_scene.treeFrameBuffer) {
			const TreeWindFrameRow row{ a_scene.treeCount, 0, a_scene.treeFrame, FrameValues::Get().TreeSeedsIndex(), a_scene.treeInputs, {} };
			a_uploads(a_scene.treeFrameBuffer, &row, sizeof(row), 0);
		}
	}
}
