// Tree LOD's draw family (Api::DrawFamily::TreeInstances, dclf-architecture.md, "Tree LOD: the draws"): the mirror's drain, the frame's decision and its program source,
// and its draws in the main passes (the depth segment's two phases and the colour segment); a trees view's faces are TreeLodFaces.cpp's.

// volk must precede every Vulkan header of the translation unit (DrawPipelinesRhi.h's FamilyPipelines).
#include <rhi_interop_vulkan.h>
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodTables.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodMirror.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"

#include "ShaderCache.h"
#include <Render/RenderGraph/RenderGraph.h>
#include <rhi.h>

#include <cstdint>
#include <span>
#include <vector>

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

		// The engine's tree LOD shapes: their shader property is a BSDistantTreeShaderProperty.
		const void* EngineShapeVtable()
		{
			return reinterpret_cast<const void*>(RE::VTABLE_BSDistantTreeShaderProperty[0].address());
		}

		// DistantTree's descriptors (ShaderCache.h, DistantTreeShaderTechniques and DistantTreeShaderFlags), as the engine's main view draws
		// tree LOD (BSDistantTreeShader::SetupTechnique): colour Deferred and AlphaTest, depth the Depth technique (bit 0) and AlphaTest; a
		// view's forward program AlphaTest alone.
		using TreeTechnique = SIE::ShaderCache::DistantTreeShaderTechniques;
		using TreeFlag = SIE::ShaderCache::DistantTreeShaderFlags;
		constexpr std::uint32_t kTreeAlphaTest = static_cast<std::uint32_t>(TreeFlag::AlphaTest);
		constexpr Api::DrawFamilyProgram kProgram{ .shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::DistantTree),
			.name = "DistantTree",
			.label = "tree LOD",
			.keyName = "tree-lod",
			.sourcePath = "Data/Shaders/DistantTree.hlsl",
			.nativeNote = "tree LOD stays native",
			.descriptors = { .colour = static_cast<std::uint32_t>(TreeFlag::Deferred) | kTreeAlphaTest,
				.depth = static_cast<std::uint32_t>(TreeTechnique::Depth) | kTreeAlphaTest,
				.forward = kTreeAlphaTest },
			.build = Api::DrawFamilyBuild::DepthThenEqualColour,
			.signature = Api::DrawFamilySignature::DrawInstanced };
		static_assert(kProgram.descriptors.colour == 0x10100 && kProgram.descriptors.depth == 0x10001 && kProgram.descriptors.forward == 0x10000,
			"tree LOD's descriptors are not the engine's (Deferred | AlphaTest, Depth | AlphaTest, AlphaTest)");

		/**
		 * @brief Records tree LOD's draw (dclf-architecture.md, "Tree LOD: the draws"): push data is the draw row's address and whether this is phase 2's depth draw;
		 * arguments come from the list's header (TreeInstances::VisibleHeader).
		 */
		void RecordTreeLodDraw(rhi::CommandList& a_commands, const FamilyPipelines& a_pipelines, bool a_colour, bool a_phaseTwo, std::uint64_t a_draw,
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
			RecordTreeLodDraw(a_record.commands, a_record.Drawn<FamilyPipelines>(), a_record.pass == Api::DrawFamilyPass::Colour,
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
			.faces = FacesContribution(),
			.program = kProgram,
			.engineShapes = { .propertyVtable = &EngineShapeVtable } });
	}
}
