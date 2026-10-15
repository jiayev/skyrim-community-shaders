// Tree LOD drawn per face by a trees view (Api::ViewShape::trees: the TreeInstances family's faces): the per-face cull (TreeLodCullCS's
// phase 1 into each face's list), the faces' draw pass's declarations and draw, each epoch commit's row, and the units the faces' lists
// hold with the tables. The lists, rows and latch are Core's view machinery's (ViewResources, ViewLatchLayout).

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodTables.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"

namespace DCLF::Draws
{
	// In DCLF::Draws' anonymous namespace, where it was defined before the feature moved: ORG's recipe telemetry names a pass by its type.
	namespace
	{
		std::shared_ptr<const ViewFrame> CurrentViewFrame(const org::PassPrepareContext& a_preparation, ViewKind a_kind)
		{
			if (const auto* shapes = RevisionShapesOf(a_preparation))
				return shapes->views[ViewIndex(a_kind)];
			return nullptr;
		}

		struct TreeBindings
		{
			org::DeclaredViewToken shapes, instances;
			std::array<org::DeclaredViewToken, kMaxViewFaces> rows, visible;
		};

		struct TreePrepared
		{
			std::shared_ptr<const ComputeProgram> program;
			std::shared_ptr<const org::LatchBlock> latch;
			TreeLodCullConstants constants{};
			std::uint32_t faces = 0;
			std::array<std::uint32_t, kMaxViewFaces> rowIndex{}, visibleIndex{};
			std::uint32_t groups = 0;
		};

		/**
		 * @brief A view's tree LOD: TreeLodCullCS's phase 1 per face, against the face's latch entry (cull mode 1: no occlusion), into
		 * the face's list; a face's row names no shape slot when it is not drawn.
		 */
		class TreeCullPass final : public org::TypedRenderGraphPass<TreeCullPass, TreePrepared, TreeBindings>
		{
		public:
			TreeCullPass(std::shared_ptr<ViewResources> a_resources, std::shared_ptr<const Features::TreeLod::TreeLodTables> a_tables) :
				resources(std::move(a_resources)), tables(std::move(a_tables)) {}

			TreeBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				TreeBindings bindings{};
				bindings.shapes = a_builder.ShaderResource(*tables->shapes).View();
				bindings.instances = a_builder.ShaderResource(*tables->instances).View();
				for (std::uint32_t f = 0; f < resources->traits->faces; ++f) {
					bindings.rows[f] = a_builder.ShaderResource(resources->treeRows[f]).View();
					bindings.visible[f] = a_builder.UnorderedAccess(*resources->treeVisible[f]).View();
				}
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
			{
				const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(NamedSerial(a_preparation, tables->shapes));
				a_out.push_back(NamedSerial(a_preparation, tables->instances));
				for (std::uint32_t f = 0; f < resources->traits->faces; ++f)
					a_out.push_back(NamedSerial(a_preparation, resources->treeVisible[f]));
			}

			TreePrepared Prepare(const TreeBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				TreePrepared prepared{};
				const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
				if (!frame || !frame->latch || !frame->treeGroups || !tables->cull)
					return prepared;
				prepared.program = tables->cull;
				prepared.latch = frame->latch;
				prepared.groups = frame->treeGroups;
				prepared.faces = resources->traits->faces;
				auto& constants = prepared.constants;
				constants.shapesIndex = CaptureViewIndex(a_preparation, a_bindings.shapes);
				constants.instancesIndex = CaptureViewIndex(a_preparation, a_bindings.instances);
				constants.latchIndex = frame->latch->SrvIndex();
				constants.phase = 1;
				constants.retestOffset = static_cast<std::uint32_t>(sizeof(TreeInstances::VisibleHeader));  // never written: no occlusion test
				for (std::uint32_t f = 0; f < prepared.faces; ++f) {
					prepared.rowIndex[f] = CaptureViewIndex(a_preparation, a_bindings.rows[f]);
					prepared.visibleIndex[f] = CaptureViewIndex(a_preparation, a_bindings.visible[f]);
				}
				return prepared;
			}

			static void Record(const TreeBindings&, const TreePrepared& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.program || !a_prepared.groups || !a_prepared.latch)
					return;
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_prepared.program->layout->GetHandle());
				commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
				const auto region = static_cast<std::uint32_t>(a_prepared.latch->Offset(a_recording.FrameSlot()));
				for (std::uint32_t f = 0; f < a_prepared.faces; ++f) {
					auto constants = a_prepared.constants;
					constants.latchOffset = region + f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
					constants.drawIndex = a_prepared.rowIndex[f];
					constants.visibleIndex = a_prepared.visibleIndex[f];
					commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kTreeLodCullConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
					commands.Dispatch(a_prepared.groups, 1, 1);
				}
			}

		private:
			std::shared_ptr<ViewResources> resources;
			std::shared_ptr<const Features::TreeLod::TreeLodTables> tables;
		};
	}
}

namespace DCLF::Features::TreeLod
{
	namespace
	{
		// Phase 1 per face into the face's list (its cull pass, in the view's depth epoch).
		std::shared_ptr<org::RenderPass> FacesCull(const Api::DrawFamilyViewContext& a_view)
		{
			auto tables = SharedTablesOf(*a_view.resources->main->scene);
			if (!tables)
				return nullptr;
			return std::make_shared<Draws::TreeCullPass>(a_view.resources, std::move(tables));
		}

		// The tables and the faces' rows and lists.
		void FacesDeclare(Api::DrawFamilyDeclare& a_declare)
		{
			const auto* tables = TablesOf(a_declare.scene);
			const auto& view = *a_declare.view;
			if (!tables || !view.treeVisible[0])
				return;
			const std::span<const org::SrvView> noViews{};
			auto& builder = a_declare.builder;
			for (const auto& table : { tables->shapes, tables->instances, tables->meshes })
				builder.ShaderResource(*table, noViews);
			for (std::uint32_t f = 0; f < view.traits->faces; ++f) {
				builder.ShaderResource(view.treeRows[f], noViews);
				a_declare.arguments[f] = builder.IndirectArguments(*view.treeVisible[f]).Resource();
				builder.ShaderResource(*view.treeVisible[f], noViews);
			}
		}

		// One instanced draw of the face's list, its row the face's (phase 1's arguments, its instances from the list's start).
		void FacesRecord(const Api::DrawFamilyRecord& a_record)
		{
			std::uint32_t words[kDrawPushWords]{ static_cast<std::uint32_t>(a_record.row), static_cast<std::uint32_t>(a_record.row >> 32), 0u };
			a_record.commands.BindPipeline(a_record.pipeline);
			a_record.commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
			a_record.commands.ExecuteIndirect(a_record.signature, a_record.arguments, offsetof(TreeInstances::VisibleHeader, phaseOne), {}, 0, 1);
		}

		// Its row as the last depth commit uploaded it, its texture bound again for this execution, when the view draws it (the
		// revision's faces have tree LOD: their lists hold the scene's shape slots, one unit group) and its pipelines and tables are in.
		// Without owners (the faces' decision): only whether that row names shape slots, its pipelines and tables in.
		bool FacesRow(Api::DrawFamilyFaceRow& a_row)
		{
			const auto& scene = a_row.scene;
			const auto* tables = TablesOf(scene);
			const auto treeLod = Draws::FamilyPipelinesOf(scene, Api::DrawFamily::TreeInstances);
			a_row.row = tables ? tables->row : TreeInstances::DrawRow{};
			const bool trees = a_row.viewDraws && treeLod && tables;
			if (!a_row.owners)
				return trees && a_row.row.shapeSlots;
			if (!(trees && a_row.row.shapeSlots && TreeLodTextureBinding(a_row.row, *a_row.owners)))
				a_row.row.shapeSlots = 0;
			return a_row.row.shapeSlots != 0;
		}

		// A face's list holds a unit per shape slot of the tables (the shape rows, the records and the main view's list, one unit group),
		// required at the mirror's posted shape slots.
		void FacesLists(Api::DrawFamilyLists& a_lists)
		{
			const auto* tables = TablesOf(a_lists.scene);
			if (!tables)
				return;
			if (a_lists.tables)
				a_lists.tables->Add(tables->shapes).Add(tables->instances).Add(tables->visible);
			if (!a_lists.inputs.empty())
				a_lists.required = a_lists.inputs[kShapeSlotsInput];
			const auto buffer = tables->shapes ? tables->shapes->Get() : nullptr;
			a_lists.current = buffer ? static_cast<std::uint32_t>(std::min<std::uint64_t>(buffer->GetSize() / sizeof(TreeInstances::ShapeRow), UINT32_MAX)) : 0u;
		}
	}

	Api::DrawFamilyContribution::Faces FacesContribution()
	{
		return { .makeCull = &FacesCull, .declare = &FacesDeclare, .record = &FacesRecord, .row = &FacesRow, .lists = &FacesLists };
	}
}
