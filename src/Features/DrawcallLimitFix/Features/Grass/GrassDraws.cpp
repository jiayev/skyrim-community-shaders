// Grass's draw family (Api::DrawFamily::Grass, dclf-architecture.md, "Grass: the draws"): the mirror's drain, the frame's decision (with
// GrassShared's stand-down of the features inside the engine's grass passes) and its program source, and its draws in the main passes
// (the depth segment's two phases and the colour segment): arrays of instanced draws with a GPU count, one per bucket and LOD tier.

// volk must precede every Vulkan header of the translation unit (DrawPipelinesRhi.h's FamilyPipelines).
#include <rhi_interop_vulkan.h>
#include "Features/DrawcallLimitFix/Features/Grass/GrassTables.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"

#include "GrassShared/GrassCapture.h"
#include "GrassShared/GrassFrame.h"

#include "ShaderCache.h"
#include <Render/RenderGraph/RenderGraph.h>
#include <rhi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace DCLF::Features::Grass
{
	namespace
	{
		// stage 0: the engine's grass colour passes blend as the main pass's opaque write mode (alphaBlendWriteMode 1).
		constexpr std::uint8_t kWriteMode = 1;

		// The frame: the mirror is the render thread's (the decision reads it at the frame's start), drained running or not. GrassShared
		// hears the grass is not DCLF's until the decision says otherwise.
		void Serve(const Api::DrawFamilyFrame& a_frame)
		{
			GrassShared::SetDclfOwned(false);
			RenderMirror().Drain(a_frame.frame);
		}

		/**
		 * @brief stage 0: whether the engine shows grass (ToggleGrass, tg): BGSGrassManager's grass node is not hidden (+0xF4 bit 0). False
		 * without the manager or its node.
		 */
		bool EngineGrassShown()
		{
			const auto* manager = Engine::Global<const std::byte*>(Address::kGrassManager);
			const auto* node = manager ? *reinterpret_cast<const std::byte* const*>(manager + Address::kGrassManagerNode) : nullptr;
			return node && (*reinterpret_cast<const std::uint32_t*>(node + 0xF4) & 1u) == 0;
		}

		/**
		 * @brief DCLF's while there is something to draw that it draws whole: tables, the mirror's buckets within the draws buffer's slots,
		 * no live shape the capture could not read (the engine draws it: withholding grass by its property would hide it), and the engine
		 * showing grass; then once the pipeline lane has built its pipelines (the grass shader as input; until the frame's catalog has
		 * them the engine draws grass). GrassShared hears the result, and on a frame DCLF owns grass the features inside the engine's grass
		 * passes do their frame's work from here (Grass Optimizations' pending captures, Grass Collision's update and publish), before
		 * the depth commit reads what they publish.
		 */
		bool Decide(const Api::DrawFamilyFrame& a_frame)
		{
			const auto& mirror = RenderMirror();
			if (!a_frame.scene || !TablesOf(*a_frame.scene) || !mirror.BucketSlots())
				return false;
			const bool owned = a_frame.Pipelines(mirror.BucketSlots() <= GrassInstances::kMaxBuckets && !mirror.RejectedLive() && EngineGrassShown());
			GrassShared::SetDclfOwned(owned);
			if (owned)
				GrassShared::RunStoodDownFrame();
			return owned;
		}

		// stage 0: the BSGrassShader singleton; before it is known, the first grass pass's shader GrassShared saw.
		const void* ProgramSource()
		{
			if (auto* shader = Engine::Global<RE::BSShader*>(Address::kGrassShader))
				return shader;
			return GrassShared::GrassShader();
		}

		// The engine's grass shapes: their shader property is a BSGrassShaderProperty.
		const void* EngineShapeVtable()
		{
			return reinterpret_cast<const void*>(RE::VTABLE_BSGrassShaderProperty[0].address());
		}

		// RunGrass's descriptors (ShaderCache.h, GrassShaderTechniques and GrassShaderFlags), as the engine's main view draws grass: colour
		// the lit technique without AlphaTest (EQUAL against the depth decides), depth RenderDepth, PBR grass's colour TruePbr (TruePBR's
		// kMenuScreen grass); no faces, so no forward program.
		using GrassTechnique = SIE::ShaderCache::GrassShaderTechniques;
		constexpr std::uint32_t kGrassColour = 0x3;
		constexpr Api::DrawFamilyProgram kProgram{ .shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::Grass),
			.name = "RunGrass",
			.label = "grass",
			.keyName = "grass",
			.sourcePath = "Data/Shaders/RunGrass.hlsl",
			.nativeNote = "grass stays native",
			.descriptors = { .colour = kGrassColour,
				.depth = static_cast<std::uint32_t>(GrassTechnique::RenderDepth),
				.forward = 0,
				.colour2 = static_cast<std::uint32_t>(GrassTechnique::TruePbr) },
			.colours = 2,
			.writeMode = kWriteMode,
			.build = Api::DrawFamilyBuild::DepthThenEqualColour,
			.signature = Api::DrawFamilySignature::DrawInstanced };
		static_assert(kProgram.descriptors.depth == 0x8 && kProgram.descriptors.colour2 == 0x9 &&
						  (kProgram.descriptors.colour & static_cast<std::uint32_t>(SIE::ShaderCache::GrassShaderFlags::AlphaTest)) == 0,
			"grass's descriptors are not the engine's (RenderDepth, TruePbr; the colour program without AlphaTest)");

		// The draws' push data (RunGrass.hlsl's DCLFGrassPush, b190): the draw row's address, the pass's phase and colour program.
		void Push(rhi::CommandList& a_commands, std::uint64_t a_draw, std::uint32_t a_phase, std::uint32_t a_program)
		{
			std::uint32_t words[kDrawPushWords]{ static_cast<std::uint32_t>(a_draw), static_cast<std::uint32_t>(a_draw >> 32), a_phase, a_program };
			a_commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
		}

		// The main passes: the draws buffer (its arrays and their counts), the draw row's address (in its push data) and the tables its
		// vertex and pixel stages read by address.
		void MainDeclare(Api::DrawFamilyDeclare& a_declare)
		{
			const auto* tables = TablesOf(a_declare.scene);
			if (!tables)
				return;
			const std::span<const org::SrvView> noViews{};  // Device-address reads need ordering, not a descriptor.
			auto& builder = a_declare.builder;
			a_declare.arguments[0] = builder.IndirectArguments(tables->draws);
			a_declare.row = tables->drawAddress;
			for (const auto& table : { tables->instances, tables->owners, tables->frame, tables->visible, tables->shapes, tables->buckets, tables->meshes })
				builder.ShaderResource(*table, noViews);
			builder.ShaderResource(tables->draw, noViews);
		}

		// Its pipelines (built late, rebuilt on a target change).
		void MainRevision(const Api::DrawFamilyRevision& a_revision, std::vector<std::uint64_t>& a_out)
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(a_revision.pipelines.get()));
		}

		/**
		 * @brief The depth passes: each phase's array of depth draws (a bucket tier's culled instances each); the colour pass: each colour
		 * program's array, EQUAL against the depth draws. Each ExecuteIndirect reads its draw count from the draws buffer's header.
		 */
		void MainRecord(const Api::DrawFamilyRecord& a_record)
		{
			using namespace GrassInstances;
			const auto& pipelines = a_record.Drawn<FamilyPipelines>();
			auto& commands = a_record.commands;
			if (a_record.pass != Api::DrawFamilyPass::Colour) {
				const bool phaseTwo = a_record.pass == Api::DrawFamilyPass::DepthPhaseTwo;
				commands.BindPipeline(pipelines.depth);
				Push(commands, a_record.row, phaseTwo ? 2u : 1u, 0u);
				commands.ExecuteIndirect(pipelines.drawSignature, a_record.arguments, phaseTwo ? kPhaseTwoArgs : kPhaseOneArgs, a_record.arguments,
					phaseTwo ? offsetof(DrawsHeader, phaseTwoDraws) : offsetof(DrawsHeader, phaseOneDraws), kMaxDrawSlots);
				return;
			}
			for (std::uint32_t program = 0; program < 2; ++program) {
				commands.BindPipeline(program ? pipelines.colour2 : pipelines.colour);
				Push(commands, a_record.row, 0u, program);
				commands.ExecuteIndirect(pipelines.drawSignature, a_record.arguments, kColourArgs[program], a_record.arguments,
					offsetof(DrawsHeader, colourDraws) + program * sizeof(std::uint32_t), kMaxDrawSlots);
			}
		}
	}

	void RegisterFamily(Api::InstallContext& a_context)
	{
		a_context.RegisterDrawFamily({ .family = Api::DrawFamily::Grass,
			.objectClass = Api::ObjectClass::Grass,
			.resources = TablesId(),
			.colourRange = "cs.dclf.colour.grass",
			.frame = { .serve = &Serve, .decide = &Decide, .programSource = &ProgramSource },
			.main = { .declare = &MainDeclare, .revision = &MainRevision, .record = &MainRecord },
			.program = kProgram,
			.engineShapes = { .propertyVtable = &EngineShapeVtable } });
	}
}
