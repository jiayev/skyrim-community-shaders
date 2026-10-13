#pragma once

// The scene's GPU tables shared by every epoch (SceneBuffers: object records, extras, geometry table, face positions, tree wind,
// fade roots and states, readback lists, tree LOD), and the commits' uploads into them (render thread). Included by Internal.h.

namespace DCLF::Draws
{
	/**
	 * @brief The scene tables' capacities (ReserveSceneTables), changed as one with their buffers' growth; commits and builds
	 * stay within them (SceneFit).
	 */
	struct SceneSizing
	{
		std::uint32_t objectCapacity = 0, geometryRows = 0, extraRows = 0, faceVertices = 0;
		std::uint32_t treeCapacity = 0, fadeRootCapacity = 0, fadeEventCapacity = 0;
		std::uint32_t visibilityEventCapacity = 0;  // the visibility events' list (0: no visibility readback)
		std::uint32_t portalWords = 0;  // the portal programs' words (PortalViews::Encoded)
		std::uint32_t treeLodShapeCapacity = 0, treeLodMeshCapacity = 0;
		bool operator==(const SceneSizing&) const = default;
	};

	struct SceneBuffers : SceneSizing
	{
		Versioned objects, extras, geometries, facePositions;
		// The index pool every plain indexed draw binds; kept current by every commit that draws from it (UpdateIndexPool).
		std::shared_ptr<IndexPool> pool;
		// A growth gives new SRV indices and addresses: builds read them here after ReserveSceneTables.
		std::uint32_t objectsIndex = 0, extrasIndex = 0;
		std::uint64_t facePositionsAddress = 0;
		// Counts the growths, so a batch staged before one is not submitted after it.
		std::uint64_t generation = 0;
		// The same count for pass invocation revisions (read on the graph host's thread, which prepares ahead of commits).
		std::atomic<std::uint64_t> layout{ 0 };
		std::uint32_t growths = 0;  // since the last report
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> faceUploaded;  // region -> generation (render thread)
		// The table versions the buffers hold, written by the uploading commit.
		TablesHeld held;
		// Tree wind (TreeWindCS): tree rows (UploadTrees) and GPU-only clocks, by tree slot.
		Versioned trees, treeClocks;
		std::shared_ptr<org::Buffer> treeFrameBuffer;  // TreeWindFrameRow
		// Wind output (kTreeWindEntryRows per entry: nodeless trees, then tree slot s at s + 1), one buffer per frame parity: the
		// Z-prepass writes this frame's, draws read the other. Zeroed per backing: a zero generation is never a listing's.
		std::array<Versioned, 2> treeWindRows;
		std::array<std::uint32_t, 2> treeWindIndex{};
		bool treeWindZeroed = false;
		/** @brief The wind buffer a frame's draws read: the one the previous frame wrote. */
		std::uint32_t TreeWindReadIndex(std::uint32_t a_frame) const { return treeWindIndex[(a_frame + 1) & 1]; }
		std::uint64_t treesHeld = ~0ull;
		std::uint32_t treeCount = 0;
		std::uint32_t treeFrame = 0;  // the scene frame of treeInputs
		TreeWindFrame treeInputs{};
		// The previous frame's, which wrote the buffer this frame's draws read. ~0u until a frame has run.
		std::uint32_t previousTreeFrame = ~0u;
		TreeWindFrame previousTreeInputs{};
		std::uint64_t previousTreesHeld = ~0ull;  // the tree rows that frame's pass read
		std::uint64_t frameTreesHeld = ~0ull;     // this frame's
		std::shared_ptr<const ComputeProgram> treeWind;
		// Fade roots (FadeStateCS): static rows by root slot, GPU state rows, the frame's inputs, and CS_DCLF_FADE_PARITY's log.
		Versioned fadeRoots, fadeStates;
		std::shared_ptr<org::Buffer> fadeFrameBuffer, fadeLog;
		// The main camera's portal programs (PortalViews::Encoded) and a mirror of what was sent.
		Versioned portalPrograms;
		std::vector<std::uint32_t> portalProgramsSent;
		Versioned fadeRootPrograms;  // per root slot: its program (PortalViews::ProgramOf)
		Versioned fadeAnimated;    // per root slot: the scene frame whose animation batch updated it
		// CPU mirrors: only differing runs go up.
		std::vector<std::uint32_t> fadeAnimatedMirror;
		std::vector<std::uint32_t> fadeRootProgramsMirror;
		std::uint64_t fadeRootProgramsHeld = ~0ull;  // the root and program versions it holds
		// FadeStateCS's published states per frame parity; builds read the previous frame's. Zeroed per backing (builds then
		// take the static row's state).
		std::array<Versioned, 2> fadeStatesOut;
		std::array<std::uint32_t, 2> fadeStatesOutIndex{};
		std::uint32_t fadeRootsIndex = 0;  // fadeRoots' descriptor (the frame record's t119: kFadeRootsRegister)
		bool fadeStatesOutZeroed = false;
		/** @brief The published states a frame's builds read: the previous frame's. */
		std::uint32_t FadeStatesReadIndex(std::uint32_t a_frame) const { return fadeStatesOutIndex[(a_frame + 1) & 1]; }
		std::uint64_t fadeRootsHeld = ~0ull;
		std::uint32_t fadeRootCount = 0;
		std::uint32_t fadeFrameNumber = 0;  // the scene frame of fadeFrame
		FadeFrame fadeFrame{};
		std::uint32_t fadeLogBase = ~0u;
		std::shared_ptr<const ComputeProgram> fadeState;
		// Fade write-back: event list (count zeroed by every depth commit), each root's last reported generation and milestone.
		Versioned fadeEvents, fadeReported;
		bool fadeReportedZeroed = false;
		std::shared_ptr<FadeWriteBack> fadeWriteBack;
		// Visibility readback (null without its program): events (count zeroed by every depth commit), and each root's and
		// object's last reported verdict (zeroing restarts the decoder).
		Versioned visibilityEvents, visibilityRootReported, visibilityObjectReported;
		bool visibilityReportedZeroed = false;
		std::shared_ptr<const ComputeProgram> visibilityEventsProgram;
		std::shared_ptr<VisibilityReadback> visibilityReadback;
		// Tree LOD (dclf-lod.md): shape rows, instance records (75 per shape slot), mesh rows, draw row and visible list, read by
		// address in the vertex stage. TreeLodCullPass fills the list in the depth epoch. treeLodReady: the row names its slots.
		Versioned treeLodShapes, treeLodInstances, treeLodMeshes, treeLodVisible;
		std::shared_ptr<org::Buffer> treeLodDraw;
		std::uint64_t treeLodInstancesAddress = 0, treeLodShapesAddress = 0, treeLodMeshesAddress = 0, treeLodDrawAddress = 0, treeLodVisibleAddress = 0;
		std::shared_ptr<const ComputeProgram> treeLodCull;
		// Published by the depth commit once built; read on the graph host's thread.
		std::atomic<std::shared_ptr<const TreeLodPipelines>> treeLodPipelines;
		std::atomic<bool> treeLodReady{ false };
		std::uint64_t treeLodUploads = 0, treeLodSlotsSent = 0;  // since the last report
		/**
		 * @brief Tree LOD cull counts (TreeLodReadbackPass), read back per frame slot on the graph host's thread.
		 */
		struct TreeLodCounts
		{
			std::vector<std::shared_ptr<org::Buffer>> readback;
			std::vector<std::uint8_t> filled;
			std::atomic<std::uint64_t> frames{ 0 }, phaseOne{ 0 }, retests{ 0 }, phaseTwo{ 0 };
		};
		std::shared_ptr<TreeLodCounts> treeLodCounts;
		// The last depth commit's draw row; the reflection reuses the tables with its own lists.
		TreeLod::DrawRow treeLodRow{};
	};

	/**
	 * @brief Tree LOD's texture and sampler into a_row: the engine's tree LOD atlas (BSDistantTreeShader::SetupTechnique's static at
	 * 0x1433dcd18) and slot 0's sampler as its draws leave it (address mode 0, filter 2). Binding owners go to the execution.
	 * False when there is no texture.
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
	 * @brief The depth commit's tree LOD uploads. Retired mesh leases and bindings go to the execution (a_owners).
	 */
	template <class Uploads>
	void UploadTreeLod(TreeLod::Mirror& a_mirror, SceneBuffers& a_scene, Uploads& a_uploads, std::vector<std::shared_ptr<const void>>& a_owners, bool a_draw)
	{
		a_scene.treeLodReady.store(false, std::memory_order_release);
		if (!a_scene.treeLodCull || !a_scene.treeLodShapes)
			return;
		for (auto& owner : a_mirror.TakeRetired())
			a_owners.push_back(std::move(owner));
		// Capacities are reserved before the epoch; a mirror past them keeps its changes and draws nothing this commit.
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
		// The cull appends only while the row names shape slots, so both passes skip tree LOD together.
		TreeLod::DrawRow row{};
		row.instances = a_scene.treeLodInstancesAddress;
		row.shapes = a_scene.treeLodShapesAddress;
		row.meshes = a_scene.treeLodMeshesAddress;
		row.visible = a_scene.treeLodVisibleAddress + TreeLod::kVisibleHeaderWords * sizeof(std::uint32_t);
		row.alphaRef = 128.0f / 255.0f;
		row.maxIndices = std::max(a_mirror.MaxIndices(), 1u);
		// The engine's tree LOD draws use the worldspace's atlas and alpha reference 128/255 (dclf-lod.md).
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

	/** @brief Sends sorted word indices as runs a_send(first, count), merging gaps up to kFadeRunGap. */
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
	 * @brief The depth commit's fade uploads: missing static rows, and once a frame the inputs and changed portal program words.
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
			// Past the buffer's words (growth in flight), send an empty header: roots take the frustum alone that frame.
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
		a_scene.fadeFrame.rootCount = a_scene.fadeRootCount;
		a_scene.fadeFrame.sceneFrame = a_scene.fadeFrameNumber;
		a_scene.fadeFrame.logBase = a_scene.fadeLogBase;
		a_uploads(a_scene.fadeFrameBuffer, &a_scene.fadeFrame, sizeof(FadeFrame), 0);
		// The write-back's header is ArmFadeReadback's.
	}

	/** @brief Zeroes the frame-ahead outputs in a new backing; done by whichever epoch commits first after a growth. */
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

	/**
	 * @brief A commit's tree uploads. a_frameRow: also the frame row (only the Z-prepass's TreeWindPass reads it).
	 */
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
			if (Policy::FreezeTreeWind())
				a_scene.treeInputs.deltaTime = 0.0f;
		}
		// Every Z-prepass commit: the pass's invocation is prepared ahead of it.
		if (a_frameRow && a_scene.treeFrameBuffer) {
			const TreeWindFrameRow row{ a_scene.treeCount, 0, a_scene.treeFrame, FrameValues::Get().TreeSeedsIndex(), a_scene.treeInputs, {} };
			a_uploads(a_scene.treeFrameBuffer, &row, sizeof(row), 0);
		}
	}
}
