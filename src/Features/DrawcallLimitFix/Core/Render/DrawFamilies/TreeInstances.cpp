// Core's tree LOD as a draw family (Api::DrawFamily::TreeInstances, dclf-lod.md), registered by Core after the features
// (Extensions::InstallFeatures) while its class has a toggle (Api::ObjectClass::TreeLod, Features/TreeLod's): its cull stages in the depth
// segment's two phases and its counts' readback (Api::GpuStageSlot's depth segment slots), and the family's callbacks - the mirror's
// drain, the frame's decision and its program source, its draws in the main passes and in a trees view's faces. Its tables, their
// uploads, latch entries and requirements are still Core's (SceneBuffers' treeLod*, UploadTreeLod, MainLatchedTargets,
// RequireSceneTables), and its report lines are Core's where they print (the AsyncWorkFamilies and SceneFrame points).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"
#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/GpuStages.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"

namespace DCLF::Draws
{
	namespace
	{
		// The stages: the cull's program and the counts' readbacks, as EnsureSceneBuffers made them (fixed with the scene's tables).
		std::shared_ptr<const void> CullSetup(const Api::GpuStageSetup& a_setup)
		{
			return a_setup.scene.treeLodCull;
		}

		std::shared_ptr<const void> CountsSetup(const Api::GpuStageSetup& a_setup)
		{
			if (!a_setup.scene.treeLodCull)
				return nullptr;
			return a_setup.scene.treeLodCounts;
		}

		std::shared_ptr<org::RenderPass> MakeCullPhaseOne(const Api::GpuStageContext& a_context)
		{
			return MakeTreeLodCullPass(a_context.resources, 1);
		}

		std::shared_ptr<org::RenderPass> MakeCullPhaseTwo(const Api::GpuStageContext& a_context)
		{
			return MakeTreeLodCullPass(a_context.resources, 2);
		}

		std::shared_ptr<org::RenderPass> MakeCounts(const Api::GpuStageContext& a_context)
		{
			return MakeTreeLodReadbackPass(a_context.resources->scene);
		}

		// The frame: the mirror is the render thread's (DecideTreeLod reads it at the frame's start).
		void Serve(const Api::DrawFamilyFrame& a_frame)
		{
			Scene::FrameView::Get().TreeLodMirror().Drain(a_frame.frame);
		}

		bool Decide(const Api::DrawFamilyFrame&)
		{
			return IndirectDraws::Get().DecideTreeLod();
		}

		void* ProgramSource()
		{
			return Engine::Global<RE::BSShader*>(0x33dcd10);  // the BSDistantTreeShader
		}

		/**
		 * @brief Records tree LOD's draw (dclf-lod.md): push data is the draw row's address and whether this is phase 2's depth draw;
		 * arguments come from the list's header (TreeLod::VisibleHeader).
		 */
		void RecordTreeLodDraw(rhi::CommandList& a_commands, const TreeLodPipelines& a_pipelines, bool a_colour, bool a_phaseTwo, std::uint64_t a_draw, rhi::ResourceHandle a_visible)
		{
			std::uint32_t words[kDrawPushWords]{ static_cast<std::uint32_t>(a_draw), static_cast<std::uint32_t>(a_draw >> 32), a_phaseTwo ? 1u : 0u };
			const std::uint64_t arguments = a_colour ? offsetof(TreeLod::VisibleHeader, colour) :
			                                a_phaseTwo ? offsetof(TreeLod::VisibleHeader, phaseTwo) : offsetof(TreeLod::VisibleHeader, phaseOne);
			a_commands.BindPipeline(a_colour ? a_pipelines.colour : a_pipelines.depth);
			a_commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
			a_commands.ExecuteIndirect(a_pipelines.drawSignature, a_visible, arguments, {}, 0, 1);
		}

		// The main passes: its arguments and the tables its vertex stage reads by address.
		void MainDeclare(Api::DrawFamilyDeclare& a_declare)
		{
			const auto& scene = a_declare.scene;
			if (!scene.treeLodCull)
				return;
			const std::span<const org::SrvView> noViews{};  // Device-address reads need ordering, not a descriptor.
			auto& builder = a_declare.builder;
			a_declare.arguments[0] = builder.IndirectArguments(*scene.treeLodVisible).Resource();
			for (const auto& table : { scene.treeLodShapes, scene.treeLodInstances, scene.treeLodMeshes })
				builder.ShaderResource(*table, noViews);
			builder.ShaderResource(scene.treeLodDraw, noViews);
		}

		// Its pipelines (built late, rebuilt on a target change).
		void MainRevision(const Api::DrawFamilyPrepare& a_prepare, std::vector<std::uint64_t>& a_out)
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(a_prepare.scene.treeLodPipelines.load(std::memory_order_acquire).get()));
		}

		// Its pipelines and the draw row's address (in its push data).
		void MainPrepare(Api::DrawFamilyPrepare& a_prepare)
		{
			const auto& scene = a_prepare.scene;
			if (!scene.treeLodCull)
				return;
			a_prepare.drawn = scene.treeLodPipelines.load(std::memory_order_acquire);
			a_prepare.row = scene.treeLodDrawAddress;
		}

		// The depth passes: one instanced draw of the phase's culled records; the colour pass: EQUAL against its Z-prepass draw.
		void MainRecord(const Api::DrawFamilyRecord& a_record)
		{
			RecordTreeLodDraw(a_record.commands, a_record.Drawn<TreeLodPipelines>(), a_record.pass == Api::DrawFamilyPass::Colour,
				a_record.pass == Api::DrawFamilyPass::DepthPhaseTwo, a_record.row, a_record.arguments);
		}

		// A trees view's faces: phase 1 per face into the face's list (its cull pass), the tables and the faces' rows and lists.
		std::shared_ptr<org::RenderPass> FacesCull(const Api::DrawFamilyViewContext& a_view)
		{
			if (!a_view.resources->main->scene->treeLodCull)
				return nullptr;
			return MakeViewTreeCullPass(a_view.resources);
		}

		void FacesDeclare(Api::DrawFamilyDeclare& a_declare)
		{
			const auto& scene = a_declare.scene;
			const auto& view = *a_declare.view;
			if (!scene.treeLodCull || !view.treeVisible[0])
				return;
			const std::span<const org::SrvView> noViews{};
			auto& builder = a_declare.builder;
			for (const auto& table : { scene.treeLodShapes, scene.treeLodInstances, scene.treeLodMeshes })
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
			a_record.commands.ExecuteIndirect(a_record.signature, a_record.arguments, offsetof(TreeLod::VisibleHeader, phaseOne), {}, 0, 1);
		}

		// Its row as the last depth commit uploaded it, its texture bound again for this execution, when the view draws it (the
		// revision's faces have tree LOD: their lists hold the scene's shape slots, one unit group) and its pipelines and cull are in.
		bool FacesRow(Api::DrawFamilyFaceRow& a_row)
		{
			const auto& scene = a_row.scene;
			const auto treeLod = scene.treeLodPipelines.load(std::memory_order_acquire);
			a_row.row = scene.treeLodRow;
			const bool trees = a_row.viewDraws && treeLod && scene.treeLodCull;
			if (!(trees && a_row.row.shapeSlots && TreeLodTextureBinding(a_row.row, a_row.owners)))
				a_row.row.shapeSlots = 0;
			return a_row.row.shapeSlots != 0;
		}
	}

	void InstallCoreDrawFamilies(Api::InstallContext& a_context)
	{
		// Only with its class taken: its toggle gates every part (DrawFamilyOn).
		if (!Extensions::RegisteredObjectClasses()[static_cast<std::size_t>(Api::ObjectClass::TreeLod)])
			return;
		// Phase 1's list before the depth draw; phase 2's retest against the HZB rebuilt this frame; the list's counts read back once both
		// phases are done.
		a_context.RegisterGpuStage({ .passName = "cs.dclf.z.tree-lod-cull", .slot = Api::GpuStageSlot::DepthSegmentBeforeDepthDraw, .setup = &CullSetup,
			.makePass = &MakeCullPhaseOne });
		a_context.RegisterGpuStage({ .passName = "cs.dclf.tree-lod-cull-phase2", .slot = Api::GpuStageSlot::DepthSegmentPhaseTwoBeforeDraw, .setup = &CullSetup,
			.makePass = &MakeCullPhaseTwo });
		a_context.RegisterGpuStage({ .passName = "cs.dclf.z.tree-lod-counts", .slot = Api::GpuStageSlot::DepthSegmentAfterPhaseTwo, .queue = Api::GpuQueue::None,
			.setup = &CountsSetup, .makePass = &MakeCounts, .kind = Api::GpuStageKind::Copy });
		a_context.RegisterDrawFamily({ .family = Api::DrawFamily::TreeInstances,
			.objectClass = Api::ObjectClass::TreeLod,
			.colourRange = "cs.dclf.colour.tree-lod",
			.frame = { .serve = &Serve, .decide = &Decide, .programSource = &ProgramSource },
			.main = { .declare = &MainDeclare, .revision = &MainRevision, .prepare = &MainPrepare, .record = &MainRecord },
			.faces = { .makeCull = &FacesCull, .declare = &FacesDeclare, .record = &FacesRecord, .row = &FacesRow } });
	}
}
