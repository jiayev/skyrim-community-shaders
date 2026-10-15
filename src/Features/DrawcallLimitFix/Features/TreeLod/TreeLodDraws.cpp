// Tree LOD's draw family (Api::DrawFamily::TreeInstances, dclf-lod.md): the mirror's drain, the frame's decision and its program source,
// and its draws in the main passes (the depth segment's two phases and the colour segment); a trees view's faces are TreeLodFaces.cpp's.

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodTables.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodMirror.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"

namespace DCLF::Features::TreeLod
{
	namespace
	{
		// The frame: the mirror is the render thread's (the decision reads it at the frame's start), drained running or not.
		void Serve(const Api::DrawFamilyFrame& a_frame)
		{
			RenderMirror().Drain(a_frame.frame);
		}

		// DCLF's while there is something to draw (tables, the mirror's shapes) and, once the pipeline lane has built its pipelines (the
		// DistantTree shader as input; until the frame's catalog has them the engine draws tree LOD), the atlas it draws with.
		bool Decide(const Api::DrawFamilyFrame& a_frame)
		{
			if (!a_frame.scene || !TablesOf(*a_frame.scene) || !RenderMirror().Size())
				return false;
			return a_frame.Pipelines(TreeLodAtlasReady());
		}

		const void* ProgramSource()
		{
			return Engine::Global<RE::BSShader*>(0x33dcd10);  // the BSDistantTreeShader
		}

		/**
		 * @brief Records tree LOD's draw (dclf-lod.md): push data is the draw row's address and whether this is phase 2's depth draw;
		 * arguments come from the list's header (TreeInstances::VisibleHeader).
		 */
		void RecordTreeLodDraw(rhi::CommandList& a_commands, const TreeLodPipelines& a_pipelines, bool a_colour, bool a_phaseTwo, std::uint64_t a_draw,
			rhi::ResourceHandle a_visible)
		{
			using TreeInstances::VisibleHeader;
			std::uint32_t words[kDrawPushWords]{ static_cast<std::uint32_t>(a_draw), static_cast<std::uint32_t>(a_draw >> 32), a_phaseTwo ? 1u : 0u };
			const std::uint64_t arguments = a_colour ? offsetof(VisibleHeader, colour) : a_phaseTwo ? offsetof(VisibleHeader, phaseTwo) : offsetof(VisibleHeader, phaseOne);
			a_commands.BindPipeline(a_colour ? a_pipelines.colour : a_pipelines.depth);
			a_commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
			a_commands.ExecuteIndirect(a_pipelines.drawSignature, a_visible, arguments, {}, 0, 1);
		}

		// The main passes: its arguments, the draw row's address (in its push data) and the tables its vertex stage reads by address.
		void MainDeclare(Api::DrawFamilyDeclare& a_declare)
		{
			const auto* tables = TablesOf(a_declare.scene);
			if (!tables)
				return;
			const std::span<const org::SrvView> noViews{};  // Device-address reads need ordering, not a descriptor.
			auto& builder = a_declare.builder;
			a_declare.arguments[0] = builder.IndirectArguments(*tables->visible).Resource();
			a_declare.row = tables->drawAddress;
			for (const auto& table : { tables->shapes, tables->instances, tables->meshes })
				builder.ShaderResource(*table, noViews);
			builder.ShaderResource(tables->draw, noViews);
		}

		// Its pipelines (built late, rebuilt on a target change).
		void MainRevision(const Api::DrawFamilyRevision& a_revision, std::vector<std::uint64_t>& a_out)
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(a_revision.pipelines.get()));
		}

		// The depth passes: one instanced draw of the phase's culled records; the colour pass: EQUAL against its Z-prepass draw.
		void MainRecord(const Api::DrawFamilyRecord& a_record)
		{
			RecordTreeLodDraw(a_record.commands, a_record.Drawn<TreeLodPipelines>(), a_record.pass == Api::DrawFamilyPass::Colour,
				a_record.pass == Api::DrawFamilyPass::DepthPhaseTwo, a_record.row, a_record.arguments);
		}
	}

	void RegisterFamily(Api::InstallContext& a_context)
	{
		a_context.RegisterDrawFamily({ .family = Api::DrawFamily::TreeInstances,
			.objectClass = Api::ObjectClass::TreeLod,
			.resources = TablesId(),
			.colourRange = "cs.dclf.colour.tree-lod",
			.frame = { .serve = &Serve, .decide = &Decide, .programSource = &ProgramSource },
			.main = { .declare = &MainDeclare, .revision = &MainRevision, .record = &MainRecord },
			.faces = FacesContribution() });
	}
}
