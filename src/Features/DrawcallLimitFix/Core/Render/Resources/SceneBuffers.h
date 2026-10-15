#pragma once

// The scene's GPU tables shared by every epoch (SceneBuffers: face positions, tree wind,
// fade roots and states, readback lists, tree LOD), and the commits' uploads into them (render thread). Included by Internal.h.

namespace DCLF::Draws
{
	/** @brief The face regions a face positions version holds (render thread): region -> snapshot generation, for the version filled. */
	struct FaceUploads
	{
		org::services::FilledVersion version;
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> regions;
	};

	/**
	 * @brief The scene's tables, each a buffer of the scene's set (SceneBufferSet): every capacity a commit, build or pass reads is
	 * its named version's units, which the revision producer requires to hold the tables (RequireSceneTables).
	 */
	struct SceneBuffers
	{
		// The object records, extras and geometry table are the installed payload's (PayloadVersions).
		Versioned facePositions;
		// The index pool every plain indexed draw binds; kept current by every commit that draws from it (UpdateIndexPool).
		std::shared_ptr<IndexPool> pool;
		FaceUploads faceUploaded;
		// Tree wind (TreeWindCS): tree rows (UploadTrees) and GPU-only clocks, by tree slot.
		Versioned trees, treeClocks;
		std::shared_ptr<org::Buffer> treeFrameBuffer;  // TreeWindFrameRow
		// Wind output (kTreeWindEntryRows per entry: nodeless trees, then tree slot s at s + 1), one buffer per frame parity: the
		// Z-prepass writes this frame's, draws read the other. Every version is zero-filled (a zero generation is never a listing's).
		std::array<Versioned, 2> treeWindRows;
		/** @brief The SRV index of the wind buffer a frame's draws read (the one the previous frame wrote), as a_sealed names it. */
		std::uint32_t TreeWindReadIndex(const NamedVersions& a_sealed, std::uint32_t a_frame) const { return a_sealed.Of(treeWindRows[(a_frame + 1) & 1]).srvIndex; }
		org::services::FilledContents<> treesFilled;  // the tree rows' content epoch (SceneTables::treesContentEpoch)
		/** @brief The tree rows' content epoch the trees version a_sealed names holds (~0: none). */
		std::uint64_t TreesContentEpoch(const NamedVersions& a_sealed) const { return treesFilled.Since(a_sealed.Of(trees)).value_or(~0ull); }
		std::uint32_t treeCount = 0;
		std::uint32_t treeFrame = 0;  // the scene frame of treeInputs
		TreeWindFrame treeInputs{};
		// The previous frame's, which wrote the buffer this frame's draws read. ~0u until a frame has run.
		std::uint32_t previousTreeFrame = ~0u;
		TreeWindFrame previousTreeInputs{};
		std::uint64_t previousTreesContentEpoch = ~0ull;  // the tree rows that frame's pass read
		std::uint64_t frameTreesContentEpoch = ~0ull;     // this frame's
		std::shared_ptr<const ComputeProgram> treeWind;
		// The features' GPU stages' states (Api::GpuStage::setup, by registration: Extensions::RegisteredStages); null: out of the graph.
		std::vector<std::shared_ptr<const void>> stages;
		// Fade roots (FadeStateCS): static rows by root slot, GPU state rows, the frame's inputs, and CS_DCLF_FADE_PARITY's log.
		Versioned fadeRoots, fadeStates;
		std::shared_ptr<org::Buffer> fadeFrameBuffer, fadeLog;
		// The main camera's portal programs (PortalViews::Encoded) and a mirror of what was sent into the version filled.
		Versioned portalPrograms;
		org::services::FilledVersion portalProgramsFilled;
		std::vector<std::uint32_t> portalProgramsSent;
		Versioned fadeRootPrograms;  // per root slot: its program (PortalViews::ProgramOf)
		Versioned fadeAnimated;    // per root slot: the scene frame whose animation batch updated it
		org::services::FilledVersion fadeAnimatedFilled;
		// CPU mirrors: only differing runs go up.
		std::vector<std::uint32_t> fadeAnimatedMirror;
		std::vector<std::uint32_t> fadeAnimatedCounts;  // per root slot: the batch's updates while it is counted, else 0
		std::vector<std::uint32_t> fadeRootProgramsMirror;
		org::services::FilledContents<> fadeRootProgramsFilled;  // the root and program versions it holds
		// What the programs mirror was judged against: its key (as filled), the roots' content epoch and PortalViews::ProgramsVersion.
		struct ProgramsMirrorOf
		{
			std::uint64_t key = 0, rootsEpoch = 0, portals = 0;
		} fadeRootProgramsMirrorOf;
		// FadeStateCS's published states per frame parity; builds read the previous frame's. Every version is zero-filled (builds
		// then take the static row's state).
		std::array<Versioned, 2> fadeStatesOut;
		/** @brief The SRV index of the published states a frame's builds read (the previous frame's), as a_sealed names them. */
		std::uint32_t FadeStatesReadIndex(const NamedVersions& a_sealed, std::uint32_t a_frame) const { return a_sealed.Of(fadeStatesOut[(a_frame + 1) & 1]).srvIndex; }
		org::services::FilledContents<> fadeRootsFilled;  // the root rows' content epoch (SceneTables::FadeRootsContentEpoch)
		/** @brief The root rows' content epoch the fadeRoots version a_sealed names holds (~0: none). */
		std::uint64_t FadeRootsContentEpoch(const NamedVersions& a_sealed) const { return fadeRootsFilled.Since(a_sealed.Of(fadeRoots)).value_or(~0ull); }
		std::uint32_t fadeRootCount = 0;
		std::uint32_t fadeFrameNumber = 0;  // the scene frame of fadeFrame
		FadeFrame fadeFrame{};
		std::uint32_t fadeLogBase = ~0u;
		std::shared_ptr<const ComputeProgram> fadeState;
		// Fade write-back: event list (count zeroed by every depth commit), each root's last reported generation and milestone
		// (every version zero-filled: no root reported yet, as a zero generation is never a listing's).
		Versioned fadeEvents, fadeReported;
		std::shared_ptr<FadeWriteBack> fadeWriteBack;
		// Visibility readback (null without its program): events (count zeroed by every depth commit), and each root's and
		// object's last reported verdict. Every version is zero-filled; the decoder restarts with the first batch of a version it
		// has not followed (visibilityReportedFollowed: root, object).
		Versioned visibilityEvents, visibilityRootReported, visibilityObjectReported;
		std::array<org::services::FilledVersion, 2> visibilityReportedFollowed;
		std::shared_ptr<const ComputeProgram> visibilityEventsProgram;
		std::shared_ptr<VisibilityReadback> visibilityReadback;
		// Tree LOD (dclf-lod.md): shape rows, instance records (75 per shape slot), mesh rows, draw row and visible list, read by
		// address in the vertex stage (the draw row's names the adopted set's versions). TreeLodCullPass fills the list in the depth
		// epoch. treeLodReady: the row names its slots.
		Versioned treeLodShapes, treeLodInstances, treeLodMeshes, treeLodVisible;
		std::array<org::services::FilledVersion, 3> treeLodFilled;  // shapes, instances, meshes
		std::shared_ptr<org::Buffer> treeLodDraw;
		std::uint64_t treeLodDrawAddress = 0;
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
	 * @brief The depth commit's tree LOD uploads, into the versions a_sealed (the adopted set) names: their capacities and the draw
	 * row's addresses are theirs. Retired mesh leases and bindings go to the execution (a_owners).
	 */
	template <class Uploads>
	void UploadTreeLod(const NamedVersions& a_sealed, TreeLod::Mirror& a_mirror, SceneBuffers& a_scene, Uploads& a_uploads, std::vector<std::shared_ptr<const void>>& a_owners,
		bool a_draw)
	{
		a_scene.treeLodReady.store(false, std::memory_order_release);
		if (!a_scene.treeLodCull || !a_scene.treeLodShapes)
			return;
		for (auto& owner : a_mirror.TakeRetired())
			a_owners.push_back(std::move(owner));
		const auto &shapes = a_sealed.Of(a_scene.treeLodShapes), &records = a_sealed.Of(a_scene.treeLodInstances), &meshRows = a_sealed.Of(a_scene.treeLodMeshes),
				   &visible = a_sealed.Of(a_scene.treeLodVisible);
		// The shape slots every shape-slot table holds (the visible list: its header, then a phase 1 and a retest entry per record).
		const std::uint64_t visibleRecords = visible.bytes / 4 > TreeLod::kVisibleHeaderWords ? (visible.bytes / 4 - TreeLod::kVisibleHeaderWords) / 2 : 0;
		const auto shapeSlots = static_cast<std::uint32_t>(std::min({ shapes.bytes / sizeof(TreeLod::ShapeRow),
			records.bytes / (TreeLod::kMaxGroupInstances * sizeof(TreeLod::Instance)), visibleRecords / TreeLod::kMaxGroupInstances }));
		const auto meshSlots = static_cast<std::uint32_t>(meshRows.bytes / sizeof(TreeLod::MeshRow));
		// Capacities are reserved before the epoch; a mirror past them keeps its changes and draws nothing this commit. Versions this
		// commit did not fill (a new backing holds nothing) are sent every slot and mesh.
		std::vector<std::uint32_t> slots, meshes;
		const bool fits = a_mirror.ShapeSlots() <= shapeSlots && a_mirror.MeshSlots() <= meshSlots;
		if (!fits) {
			a_draw = false;
		} else {
			auto& filled = a_scene.treeLodFilled;
			if (!filled[0].Holds(shapes) || !filled[1].Holds(records) || !filled[2].Holds(meshRows))
				a_mirror.MarkAllChanged();
			a_mirror.TakeChanges(slots, meshes);
			filled[0].Filled(shapes);
			filled[1].Filled(records);
			filled[2].Filled(meshRows);
		}
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
		row.instances = records.deviceAddress;
		row.shapes = shapes.deviceAddress;
		row.meshes = meshRows.deviceAddress;
		row.visible = visible.deviceAddress + TreeLod::kVisibleHeaderWords * sizeof(std::uint32_t);
		row.alphaRef = 128.0f / 255.0f;
		row.maxIndices = std::max(a_mirror.MaxIndices(), 1u);
		// The engine's tree LOD draws use the worldspace's atlas and alpha reference 128/255 (dclf-lod.md).
		if (a_draw && a_mirror.MaxIndices() && TreeLodTextureBinding(row, a_owners))
			row.shapeSlots = shapeSlots;
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
	 * @brief The depth commit's fade uploads into the versions a_sealed (the adopted set) names: missing static rows, and once a frame
	 * the inputs and changed portal program words.
	 */
	template <class Uploads>
	void UploadFadeRoots(const NamedVersions& a_sealed, const SceneTables& a_tables, std::uint32_t a_frame, const FadeFrame& a_inputs, std::uint32_t a_logBase,
		SceneBuffers& a_scene, Uploads& a_uploads, const std::vector<std::uint32_t>& a_programs)
	{
		if (!a_scene.fadeState || !a_scene.fadeRoots)
			return;
		const std::uint64_t current = a_tables.FadeRootsContentEpoch();
		const auto& roots = a_sealed.Of(a_scene.fadeRoots);
		const std::uint64_t contentEpoch = a_scene.fadeRootsFilled.Since(roots).value_or(~0ull);
		if (contentEpoch != current && a_tables.fadeRoots.size() <= roots.units) {
			const auto* rows = a_tables.fadeRoots.data();
			a_tables.fadeRootsJournal.Take().ForEachRun(contentEpoch, a_tables.fadeRoots.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
				a_uploads(a_scene.fadeRoots, rows + a_first, static_cast<std::size_t>(a_count * sizeof(FadeRootStatic)), a_first * sizeof(FadeRootStatic));
			});
			a_scene.fadeRootsFilled.Filled(roots, current);
		}
		a_scene.fadeRootCount = a_scene.FadeRootsContentEpoch(a_sealed) == current ? static_cast<std::uint32_t>(a_tables.fadeRoots.size()) : 0u;
		if (a_scene.fadeFrameNumber != a_frame) {
			a_scene.fadeFrameNumber = a_frame;
			a_scene.fadeFrame = a_inputs;
			// Past the named version's words (an encoding newer than the revision's requirement), send an empty header: roots take
			// the frustum alone that frame.
			if (a_scene.portalPrograms) {
				static const std::uint32_t kNone[kPortalHeaderBytes / 4]{};
				const auto& programs = a_sealed.Of(a_scene.portalPrograms);
				const bool fits = a_programs.size() <= programs.units;
				const auto* words = fits ? a_programs.data() : kNone;
				const std::size_t count = fits ? a_programs.size() : std::size(kNone);
				auto& sent = a_scene.portalProgramsSent;
				if (!a_scene.portalProgramsFilled.Holds(programs)) {
					sent.clear();
					a_scene.portalProgramsFilled.Filled(programs);
				}
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

	/**
	 * @brief A commit's tree uploads into the versions a_sealed (the adopted set) names. a_frameRow: also the frame row (only the
	 * Z-prepass's TreeWindPass reads it).
	 */
	template <class Uploads>
	void UploadTrees(const NamedVersions& a_sealed, const SceneTables& a_tables, std::uint32_t a_frame, SceneBuffers& a_scene, Uploads& a_uploads, bool a_frameRow)
	{
		if (!a_scene.treeWind || !a_scene.trees)
			return;
		const auto& trees = a_sealed.Of(a_scene.trees);
		if (a_scene.treesFilled.Since(trees) != a_tables.treesContentEpoch && a_tables.trees.size() <= trees.units) {
			if (!a_tables.trees.empty())
				a_uploads(a_scene.trees, a_tables.trees.data(), a_tables.trees.size() * sizeof(TreeStatic), 0);
			a_scene.treesFilled.Filled(trees, a_tables.treesContentEpoch);
		}
		a_scene.frameTreesContentEpoch = a_scene.TreesContentEpoch(a_sealed);
		a_scene.treeCount = a_scene.frameTreesContentEpoch == a_tables.treesContentEpoch ? static_cast<std::uint32_t>(a_tables.trees.size()) : 0u;
		if (a_scene.treeFrame != a_frame) {
			a_scene.previousTreeFrame = a_scene.treeFrame;
			a_scene.previousTreeInputs = a_scene.treeInputs;
			a_scene.previousTreesContentEpoch = a_scene.frameTreesContentEpoch;
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
