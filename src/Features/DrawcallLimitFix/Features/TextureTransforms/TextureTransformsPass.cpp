// The texture transforms' pass: the installed payload's animated material rows copied into the frame's copy, then this frame's
// TexcoordOffset and declared frame variables into the copy (TextureTransformsCS: the same row format for both), first in the depth
// segment, from the pool's rows the depth latch's stage word names; the installed rows are only read.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF::Draws
{
	namespace
	{
		// TextureTransformsCS.hlsl (the animated rows into the frame's copy, then the frame's TexcoordOffset into the copy): the depth
		// segment's BuildDrawsLatch in its frame slot, which names the installed rows (materialRowsLo/Hi), the frame's copy (animatedRows)
		// and the pool's entries (stageWord). Two fixed dispatches, the copy (phase 0) then the stores (phase 1); their threads stride over
		// the rows and entries the header counts.
		constexpr const char* kShader = "DrawcallLimitFix/Features/TextureTransforms/TextureTransformsCS.hlsl";
		struct Constants
		{
			std::uint32_t latchIndex = 0, latchOffset = 0;
			std::uint32_t phase = 0;
			std::uint32_t padding = 0;
		};
		static_assert(sizeof(Constants) == 16);
		constexpr std::uint32_t kConstantWords = sizeof(Constants) / 4;
		constexpr std::uint32_t kGroups = 32;  // of TextureTransformsCS's 64 threads
	}

	// In DCLF::Draws, where it was defined before the feature moved: ORG's recipe telemetry names a pass by its type (typeid).
	struct TextureTransformsBindings
	{};

	struct TextureTransformsPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
	};

	/**
	 * @brief The frame's copy of the installed payload's animated material rows, and the frame's TexcoordOffset into it
	 * (TextureTransformsCS.hlsl), first in the depth segment: every animated row copied whole from the installed rows (immutable, shared
	 * by the frames in flight), then the pool's entries (the material's buffer the frame's shaders read) stored into their rows' copies,
	 * which every DCLF draw of those rows then reads (BuildDraws points them there: MaterialRowAddress). The copy is read by address and
	 * declared by no pass, so the pass orders itself: full memory barriers before the copy (the earlier frames' draws reading a backing
	 * the pool hands this frame again), between the copy and the stores, and after them (this frame's draws), on the graphics queue the
	 * draws run on. It declares the sequences as written (no view) so the graph keeps it before the depth build and its draws.
	 */
	class TextureTransformsPass final : public org::TypedRenderGraphPass<TextureTransformsPass, TextureTransformsPrepared, TextureTransformsBindings>
	{
	public:
		// a_segment: its epoch's (the depth epoch's: the PassContext it was made with).
		TextureTransformsPass(std::shared_ptr<Resources> a_resources, std::shared_ptr<const ComputeProgram> a_program, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), program(std::move(a_program)), segment(a_segment) {}

		TextureTransformsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::UavView> noViews{};
			(void)a_builder.UnorderedAccess(*resources->sequences, noViews);
			return {};
		}

		// The recording depends on the depth shape (its latch); the rows, the entries and their count are the latch's and the entries'.
		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, segment);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(BuildsDraws(a_preparation, *resources, segment) ? 1u : 0u);
		}

		TextureTransformsPrepared Prepare(const TextureTransformsBindings&, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			TextureTransformsPrepared prepared{};
			// Where the depth build runs: its commit wrote the latch (the payload's rows, the pool's entries) in the frame slot.
			const auto frame = CurrentFrame(a_preparation, segment);
			if (!program || !frame || !frame->latch || !BuildsDraws(a_preparation, *resources, segment))
				return prepared;
			prepared.program = program;
			prepared.latch = frame->latch;
			return prepared;
		}

		static void Record(const TextureTransformsBindings&, const TextureTransformsPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			RecordApply(a_recording.Commands(), a_frame, a_recording.FrameSlot());
		}

		// The barriers and both dispatches, from the latch's region of a_frameSlot (Record's and the CPU mode's).
		static void RecordApply(rhi::CommandList& a_commands, const TextureTransformsPrepared& a_frame, std::uint32_t a_frameSlot)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			Constants constants{};
			constants.latchIndex = a_frame.latch->SrvIndex();
			// The depth segment's BuildDrawsLatch in this frame slot.
			constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_frameSlot));
			auto& commands = a_commands;
			const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
			const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
			commands.Barriers(batch);
			// Every animated row copied before any entry is stored (an entry's row may be another group's).
			constants.phase = 0;
			RecordDispatch(commands, *a_frame.program, constants, kGroups);
			commands.Barriers(batch);
			constants.phase = 1;
			RecordDispatch(commands, *a_frame.program, constants, kGroups);
			commands.Barriers(batch);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const ComputeProgram> program;
		RenderGraphRuntime::Segment segment;
	};
}

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		// The scene's GPU setup: the program (the scene lane no longer writes the transforms into records).
		std::shared_ptr<const void> Setup(const Api::PassSetup& a_setup)
		{
			auto program = ComputeProgram::Load(a_setup.device, { .source = Draws::kShader, .constantWords = Draws::kConstantWords });
			if (!program)
				logger::warn("[DCLF] The texture transforms program could not be created; DCLF's draws keep their materials' captured TexcoordOffset");
			// Without it nothing writes the frame's copy: no latch names it (the draws read the installed rows).
			SetCopyAvailable(program != nullptr);
			return program;
		}

		std::shared_ptr<org::RenderPass> MakePass(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::TextureTransformsPass>(a_context.Main(), a_context.State<ComputeProgram>(), a_context.segment);
		}

		// The CPU mode's (Api::PassDef::direct): Prepare's conditions on the committed depth shape, then the same commands.
		void DirectApply(const Api::DirectPass& a_pass, Api::DirectWork& a_out)
		{
			auto program = a_pass.State<ComputeProgram>();
			const auto frame = Draws::CurrentFrame(a_pass.shapes, a_pass.segment);
			if (!program || !frame || !frame->latch || !Draws::BuildsDraws(a_pass.shapes, *a_pass.Main(), a_pass.segment))
				return;
			a_out.Add([prepared = Draws::TextureTransformsPrepared{ std::move(program), frame->latch }, slot = a_pass.frameSlot](rhi::CommandList& a_commands) {
				Draws::TextureTransformsPass::RecordApply(a_commands, prepared, slot);
			});
		}
	}

	void RegisterPass(Api::InstallContext& a_context)
	{
		// The animated material rows into the frame's copy and the frame's texture transforms into it, before anything of the frame draws
		// from them; the depth latch's stage word names the pool's rows (0: no producer this frame, the shader writes nothing), every main-rows
		// latch the copy (animatedRows: 0 then too).
		a_context.RegisterPass({ .key = "texture-transforms.apply", .passName = "cs.dclf.z.texture-transforms", .make = &MakePass, .setup = &Setup,
			.latchWord = &RowsIndex, .animatedRows = &AnimatedRowsAddress, .direct = &DirectApply });
	}
}
