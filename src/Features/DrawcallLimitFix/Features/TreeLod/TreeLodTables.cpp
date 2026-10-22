// Tree LOD's scene resources (the TreeInstances family's tables, its cull program and counts' readbacks, made with the scene's tables),
// what they post with the revision's inputs and latch in the depth commit, and the depth commit's uploads into them (the commit hook).

// Helpers.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodTables.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodMirror.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/Diagnostics/TreeLodEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Features::TreeLod
{
	namespace
	{
		using TreeInstances::DrawRow;
		using TreeInstances::Instance;
		using TreeInstances::kMaxGroupInstances;
		using TreeInstances::kStageSlots;
		using TreeInstances::kStageWords;
		using TreeInstances::kVisibleHeaderWords;
		using TreeInstances::MeshRow;
		using TreeInstances::ShapeRow;
		using TreeInstances::VisibleHeader;

		Api::SceneResourcesId tablesId;  // install

		// The tables' first capacities (shape slots, mesh slots), doubled as the mirror needs.
		constexpr std::uint32_t kInitialShapes = 1024, kInitialMeshes = 256;

		// The tables' sizes in words (TreeInstancesLayout.h): per shape slot its row, records and visible/retest list entries; per mesh
		// slot its row; plus the list header.
		std::uint32_t ShapeWords(std::uint32_t a_slots) { return a_slots * static_cast<std::uint32_t>(sizeof(ShapeRow) / 4); }
		std::uint32_t InstanceWords(std::uint32_t a_slots) { return a_slots * kMaxGroupInstances * static_cast<std::uint32_t>(sizeof(Instance) / 4); }
		std::uint32_t MeshWords(std::uint32_t a_slots) { return a_slots * static_cast<std::uint32_t>(sizeof(MeshRow) / 4); }
		std::uint32_t VisibleWords(std::uint32_t a_slots) { return kVisibleHeaderWords + 2 * a_slots * kMaxGroupInstances; }

		std::uint64_t AddressOf(rhi::Device a_device, org::Buffer& a_buffer)
		{
			return a_device.GetBufferDeviceAddress({ a_buffer.GetAPIResource().GetHandle(), 0 });
		}

		// The scene's tables are made (the first frame with a device): the cull program, then the tables, after the face positions.
		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			// Without the tree LOD cull program, tree LOD stays the engine's.
			auto tables = std::make_shared<TreeLodTables>();
			tables->cull = ComputeProgram::Load(a_setup.device, { .source = kTreeLodCullShader, .constantWords = kTreeLodCullConstantWords });
			if (!tables->cull) {
				logger::warn("[DCLF] The tree LOD cull program could not be created; tree LOD stays native");
				return nullptr;
			}
			// By shape slot: a row, its records, and a phase 1 and a retest entry per record after the visible list's header.
			const std::uint32_t shapes = a_setup.smallStart ? 4u : kInitialShapes;
			tables->shapes = Draws::MakeVersioned(Draws::CreateWords(ShapeWords(shapes), false, "cs.dclf.tree-lod-shapes"), { .extent = { 0, sizeof(ShapeRow), 4 } });
			tables->instances = Draws::MakeVersioned(Draws::CreateWords(InstanceWords(shapes), false, "cs.dclf.tree-lod-instances"),
				{ .extent = { 0, kMaxGroupInstances * sizeof(Instance), 4 } });
			tables->meshes = Draws::MakeVersioned(Draws::CreateWords(MeshWords(a_setup.smallStart ? 4u : kInitialMeshes), false, "cs.dclf.tree-lod-meshes"),
				{ .extent = { 0, sizeof(MeshRow), 4 } });
			tables->draw = Draws::CreateWords(sizeof(DrawRow) / 4, false, "cs.dclf.tree-lod-draw");
			tables->visible = Draws::MakeVersioned(Draws::CreateWords(VisibleWords(shapes), true, "cs.dclf.tree-lod-visible"),
				{ .extent = { kVisibleHeaderWords * 4, 2 * kMaxGroupInstances * 4, 4 } });
			const auto device = a_setup.device;
			tables->drawAddress = AddressOf(device, *tables->draw);
			if (!AddressOf(device, *tables->shapes->Get()) || !AddressOf(device, *tables->instances->Get()) || !AddressOf(device, *tables->meshes->Get()) ||
				!tables->drawAddress || !AddressOf(device, *tables->visible->Get())) {
				logger::warn("[DCLF] Tree LOD's tables have no device address; tree LOD stays native");
				return nullptr;
			}
			if (debugStages) {
				tables->stages = Draws::CreateWords(kStageSlots * kStageWords, false, "cs.dclf.tree-lod-stages");
				tables->stagesAddress = AddressOf(device, *tables->stages);
			}
			if (auto* host = RenderGraphRuntime::Get().Host()) {
				auto counts = std::make_shared<TreeLodCounts>();
				for (std::uint32_t i = 0; i < host->FrameSlots(); ++i)
					counts->readback.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, sizeof(VisibleHeader) + kStageSlots * kStageWords * 4));
				counts->filled.assign(host->FrameSlots(), 0u);
				tables->counts = std::move(counts);
			}
			return tables;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			const auto& tables = a_graph.State<TreeLodTables>();
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-lod-shapes"), tables.shapes);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-lod-instances"), tables.instances);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-lod-meshes"), tables.meshes);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-lod-draw"), tables.draw);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-lod-visible"), tables.visible);
		}

		// The tables by the mirror's posted shape and mesh slots (a trees view's faces' lists hold the same shape slots: Core's, through
		// the family's faces.lists).
		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			const auto& tables = a_requirements.State<TreeLodTables>();
			Draws::UnitGroup{}.Add(tables.shapes).Add(tables.instances).Add(tables.visible).Require(a_requirements.build, a_requirements.inputs[kShapeSlotsInput]);
			Draws::SceneBufferSet::Get().Require(a_requirements.build, tables.meshes, a_requirements.inputs[kMeshSlotsInput]);
		}

		// The mirror's shape and mesh slots: what the tables must hold.
		void PostInputs(std::span<std::uint32_t> a_words)
		{
			const auto& mirror = RenderMirror();
			a_words[kShapeSlotsInput] = mirror.ShapeSlots();
			a_words[kMeshSlotsInput] = mirror.MeshSlots();
		}

		// The depth commit's draw row and list header; not the next-frame state's frame rows (the features', the fades'), which the compute
		// queue reads.
		void Latched(Api::SceneResourcesLatch& a_latch)
		{
			const auto& tables = a_latch.State<TreeLodTables>();
			a_latch.Latch(tables.draw, sizeof(DrawRow));
			a_latch.Latch(tables.visible, sizeof(VisibleHeader));
		}

		/**
		 * @brief The depth commit's tree LOD uploads, into the versions a_sealed (the adopted set) names: their capacities and the draw
		 * row's addresses are theirs. Retired mesh leases and bindings go to the execution (a_owners).
		 */
		void UploadTreeLod(const Draws::NamedVersions& a_sealed, Mirror& a_mirror, TreeLodTables& a_tables, Draws::LatchedUploads& a_uploads,
			std::vector<std::shared_ptr<const void>>& a_owners, bool a_draw, std::uint32_t a_frame)
		{
			a_tables.ready.store(false, std::memory_order_release);
			if (!a_tables.cull || !a_tables.shapes)
				return;
			for (auto& owner : a_mirror.TakeRetired())
				a_owners.push_back(std::move(owner));
			const auto &shapes = a_sealed.Of(a_tables.shapes), &records = a_sealed.Of(a_tables.instances), &meshRows = a_sealed.Of(a_tables.meshes),
					   &visible = a_sealed.Of(a_tables.visible);
			// The shape slots every shape-slot table holds (the visible list: its header, then a phase 1 and a retest entry per record).
			const std::uint64_t visibleRecords = visible.bytes / 4 > kVisibleHeaderWords ? (visible.bytes / 4 - kVisibleHeaderWords) / 2 : 0;
			const auto shapeSlots = static_cast<std::uint32_t>(std::min({ shapes.bytes / sizeof(ShapeRow), records.bytes / (kMaxGroupInstances * sizeof(Instance)),
				visibleRecords / kMaxGroupInstances }));
			const auto meshSlots = static_cast<std::uint32_t>(meshRows.bytes / sizeof(MeshRow));
			// Capacities are reserved before the epoch; a mirror past them keeps its changes and draws nothing this commit. Versions this
			// commit did not fill (a new backing holds nothing) are sent every slot and mesh.
			std::vector<std::uint32_t> slots, meshes;
			const bool fits = a_mirror.ShapeSlots() <= shapeSlots && a_mirror.MeshSlots() <= meshSlots;
			if (!fits) {
				a_draw = false;
			} else {
				auto& filled = a_tables.filled;
				if (!filled[0].Holds(shapes) || !filled[1].Holds(records) || !filled[2].Holds(meshRows))
					a_mirror.MarkAllChanged();
				a_mirror.TakeChanges(slots, meshes);
				filled[0].Filled(shapes);
				filled[1].Filled(records);
				filled[2].Filled(meshRows);
			}
			for (const std::uint32_t slot : slots) {
				const auto& row = a_mirror.SlotRow(slot);
				a_uploads(a_tables.shapes, &row, sizeof(row), std::uint64_t(slot) * sizeof(ShapeRow));
				if (const auto* instances = a_mirror.SlotInstances(slot); instances && !instances->empty())
					a_uploads(a_tables.instances, instances->data(), instances->size() * sizeof(Instance), std::uint64_t(slot) * kMaxGroupInstances * sizeof(Instance));
			}
			for (const std::uint32_t mesh : meshes) {
				const auto& row = a_mirror.MeshSlotRow(mesh);
				a_uploads(a_tables.meshes, &row, sizeof(row), std::uint64_t(mesh) * sizeof(MeshRow));
			}
			a_tables.slotsSent += slots.size();
			++a_tables.uploads;
			// The cull appends only while the row names shape slots, so both passes skip tree LOD together.
			DrawRow row{};
			row.instances = records.deviceAddress;
			row.shapes = shapes.deviceAddress;
			row.meshes = meshRows.deviceAddress;
			row.visible = visible.deviceAddress + kVisibleHeaderWords * sizeof(std::uint32_t);
			row.alphaRef = 128.0f / 255.0f;
			row.maxIndices = std::max(a_mirror.MaxIndices(), 1u);
			row.statsLow = static_cast<std::uint32_t>(a_tables.stagesAddress);
			row.statsHigh = static_cast<std::uint32_t>(a_tables.stagesAddress >> 32);
			row.frame = a_frame;
			// The engine's tree LOD draws use the worldspace's atlas and alpha reference 128/255 (dclf-architecture.md, "Tree LOD: the draws").
			if (a_draw && a_mirror.MaxIndices() && TreeLodTextureBinding(row, a_owners))
				row.shapeSlots = shapeSlots;
			a_uploads(a_tables.draw, &row, sizeof(row), 0);
			a_tables.row = row;
			VisibleHeader header{};
			header.phaseOne[0] = header.phaseTwo[0] = header.colour[0] = row.maxIndices;
			a_uploads(a_tables.visible, &header, sizeof(header), 0);
			if (row.shapeSlots)
				a_tables.ready.store(true, std::memory_order_release);
		}

		// Tree LOD (dclf-architecture.md, "Tree LOD: the draws"): the mirror's changes, draw row and list arguments, for the depth cull and both passes' draws; the depth
		// commit's only, after the other features' hooks (its uploads go through the commit's latched uploads).
		void Commit(const Api::CommitContext& a_commit)
		{
			if (a_commit.kind != Api::CommitKind::Depth)
				return;
			auto* tables = TablesOf(a_commit.scene);
			if (!tables)
				return;
			auto& mirror = RenderMirror();
			const bool owned = IndirectDraws::Get().DrawFamilyOwned(Api::DrawFamily::TreeInstances);
			UploadTreeLod(a_commit.adopted, mirror, *tables, *a_commit.latched, a_commit.bindingOwners, owned, a_commit.frame);
			// The engine's passes were withheld: a commit that cannot draw leaves a hole.
			if (owned && !tables->ready.load(std::memory_order_acquire))
				Diagnostics::Notify(TreeLodMissed{ a_commit.frame, mirror.ShapeSlots(), a_commit.adopted.Of(tables->shapes).units });
		}
	}

	TreeLodTables* TablesOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SceneResourcesOf<TreeLodTables>(a_scene, tablesId);
	}

	std::shared_ptr<TreeLodTables> SharedTablesOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SharedSceneResourcesOf<TreeLodTables>(a_scene, tablesId);
	}

	Api::SceneResourcesId TablesId()
	{
		return tablesId;
	}

	bool TreeLodTextureBinding(TreeInstances::DrawRow& a_row, std::vector<std::shared_ptr<const void>>& a_owners)
	{
		const auto* texture = Engine::Global<RE::NiSourceTexture*>(Address::kTreeLodAtlas);
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

	bool TreeLodAtlasReady()
	{
		const auto* texture = Engine::Global<RE::NiSourceTexture*>(Address::kTreeLodAtlas);
		return texture && texture->rendererTexture && texture->rendererTexture->resourceView;
	}

	void RegisterTables(Api::InstallContext& a_context)
	{
		tablesId = a_context.RegisterSceneResources({ .name = "tree-lod", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .postInputs = &PostInputs,
			.inputWords = kInputWords, .latched = &Latched });
		a_context.RegisterCommitHook({ .name = "tree-lod", .commit = &Commit });
	}
}
