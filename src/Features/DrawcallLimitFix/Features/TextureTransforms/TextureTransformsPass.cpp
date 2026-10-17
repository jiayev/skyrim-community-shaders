// The texture transforms' pass: this frame's TexcoordOffset and declared frame variables into the installed payload's material rows
// (TextureTransformsCS: the same row format for both), first in the depth segment, from the pool's rows the depth latch's stage word names.

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
		// TextureTransformsCS.hlsl (the frame's TexcoordOffset into the installed payload's material rows): the depth segment's
		// BuildDrawsLatch in its frame slot, which names the rows (materialRowsLo/Hi) and the pool's entries (stageWord). The dispatch is
		// fixed; its threads stride over the entries the header counts.
		constexpr const char* kShader = "DrawcallLimitFix/Features/TextureTransforms/TextureTransformsCS.hlsl";
		struct Constants
		{
			std::uint32_t latchIndex = 0, latchOffset = 0;
			std::uint32_t padding[2]{};
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
	 * @brief The frame's TexcoordOffset into the installed payload's material rows (TextureTransformsCS.hlsl), first in the depth segment:
	 * the pool's entries (the material's buffer the frame's shaders read) stored at their rows' addresses, which every DCLF draw of
	 * the frame then reads. The rows are read by address and declared by no pass, so the pass orders itself: full memory barriers around
	 * its dispatch, on the graphics queue the draws run on (before it, the earlier frames' draws reading the same payload backing; after
	 * it, this frame's). It declares the sequences as written (no view) so the graph keeps it before the depth build and its draws.
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
			if (!a_frame.program || !a_frame.latch)
				return;
			Constants constants{};
			constants.latchIndex = a_frame.latch->SrvIndex();
			// The depth segment's BuildDrawsLatch in this frame slot.
			constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
			auto& commands = a_recording.Commands();
			const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
			const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
			commands.Barriers(batch);
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
			return program;
		}

		std::shared_ptr<org::RenderPass> MakePass(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::TextureTransformsPass>(a_context.Main(), a_context.State<ComputeProgram>(), a_context.segment);
		}
	}

	void RegisterPass(Api::InstallContext& a_context)
	{
		// The frame's texture transforms into the payload's material rows, before anything of the frame draws from them; the depth latch's
		// stage word names the pool's rows (0: no producer this frame, the shader stores nothing).
		a_context.RegisterPass({ .key = "texture-transforms.apply", .passName = "cs.dclf.z.texture-transforms", .make = &MakePass, .setup = &Setup,
			.latchWord = &RowsIndex });
	}
}
