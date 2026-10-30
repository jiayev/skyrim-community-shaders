// The frame's object rows (Common/DCLFObjects.hlsli, DCLFPixelObjectRow): every object slot's pixel-stage values, MaterialData with its LOD
// fades and alpha fade, EmitColor and SSRParams.w, the cross-fade copy's alpha and the alpha test's reference, which Lighting.hlsl's pixel
// stages computed per pixel from the object's rows, its fade node's LOD metric and its fade root's state, though they are the draw's. A
// frame value pool the GPU alone writes ("pixel-rows", a row an object slot as the frame's shading rows hold), PixelRowsCS once a frame
// first in the depth segment (fades.pixel-rows, after the latched copies, which put the frame record and constants it reads in place), the
// pixel stages' t116 (a frame binding, kPixelObjectsRegister).

#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"

#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace DCLF::Draws
{
	namespace
	{
		// PixelRowsCS.hlsl: the latch's frame rows (BuildDrawsLatch::frameRows), the frame record's texture registers and the frame's
		// LodFadeFrame. Its threads stride over the rows.
		constexpr const char* kPixelRowsShader = "DrawcallLimitFix/Features/Fades/PixelRowsCS.hlsl";
		struct PixelRowsConstants
		{
			std::uint32_t latchIndex = 0, latchOffset = 0;
			std::uint32_t recordTexturesLo = 0, recordTexturesHi = 0;
			std::uint32_t lodFadeFrameLo = 0, lodFadeFrameHi = 0;
			std::uint32_t padding[2]{};
		};
		static_assert(sizeof(PixelRowsConstants) == 32);
		constexpr std::uint32_t kPixelRowsConstantWords = sizeof(PixelRowsConstants) / 4;
		constexpr std::uint32_t kPixelRowsGroups = 64;  // of PixelRowsCS's 64 threads
		constexpr std::uint32_t kPixelObjectRowBytes = 48;  // sizeof(DCLFPixelObjectRow)
	}

	struct PixelRowsBindings
	{};

	struct PixelRowsPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint64_t frameConstants = 0;  // Resources::frameConstantsAddress
		// The full barriers before and after: an external epoch's execution (GraphConditions::cpuPipeline) has its own at its entry
		// and exit, and no pass of it reads the rows.
		bool outerBarriers = true;
	};

	/**
	 * @brief The frame's object rows (PixelRowsCS.hlsl), first in the depth segment after the latched copies: every row of the latch's
	 * backing written before any of the frame's draws reads it. The backing is read through a frame binding no pass declares, so the
	 * pass orders itself as the texture transforms' does: full memory barriers before (the earlier frames' draws reading a backing the
	 * pool hands this frame again) and after (this frame's draws), on the graphics queue the draws run on; it reads the frame constants
	 * (after the latched copies) and declares the sequences as written (no view), which keeps it before the depth build and its draws.
	 */
	class PixelRowsPass final : public org::TypedRenderGraphPass<PixelRowsPass, PixelRowsPrepared, PixelRowsBindings>
	{
	public:
		PixelRowsPass(std::shared_ptr<Resources> a_resources, std::shared_ptr<const ComputeProgram> a_program, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), program(std::move(a_program)), segment(a_segment),
			outerBarriers(!CurrentGraphConditions().cpuPipeline) {}

		PixelRowsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noReads{};
			const std::span<const org::UavView> noViews{};
			(void)a_builder.ShaderResource(resources->frameConstants, noReads);
			(void)a_builder.UnorderedAccess(*resources->sequences, noViews);
			return {};
		}

		// The shape (and whether it builds draws) is the revision's (the ticket's host data).
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(resources->frameConstantsAddress); }

		PixelRowsPrepared Prepare(const PixelRowsBindings&, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			const auto frame = CurrentFrame(a_preparation, segment);
			if (!program || !frame || !frame->latch || !BuildsDraws(a_preparation, *resources, segment))
				return {};
			return { program, frame->latch, resources->frameConstantsAddress, outerBarriers };
		}

		static void Record(const PixelRowsBindings&, const PixelRowsPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			RecordRows(a_recording.Commands(), a_frame, a_recording.FrameSlot());
		}

		// The barriers and the dispatch, from the latch's region of a_frameSlot.
		static void RecordRows(rhi::CommandList& a_commands, const PixelRowsPrepared& a_frame, std::uint32_t a_frameSlot)
		{
			if (!a_frame.program || !a_frame.latch || !a_frame.frameConstants)
				return;
			const std::uint64_t textures = a_frame.frameConstants + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes + offsetof(DrawBindings, textures);
			const std::uint64_t lodFadeFrame = LodFadeFrameAddress(a_frame.frameConstants);
			PixelRowsConstants constants{};
			constants.latchIndex = a_frame.latch->SrvIndex();
			constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_frameSlot));
			constants.recordTexturesLo = static_cast<std::uint32_t>(textures);
			constants.recordTexturesHi = static_cast<std::uint32_t>(textures >> 32);
			constants.lodFadeFrameLo = static_cast<std::uint32_t>(lodFadeFrame);
			constants.lodFadeFrameHi = static_cast<std::uint32_t>(lodFadeFrame >> 32);
			const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
			const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
			if (!a_frame.outerBarriers) {
				RecordDispatch(a_commands, *a_frame.program, constants, kPixelRowsGroups);
				return;
			}
			a_commands.Barriers(batch);
			RecordDispatch(a_commands, *a_frame.program, constants, kPixelRowsGroups);
			a_commands.Barriers(batch);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const ComputeProgram> program;
		RenderGraphRuntime::Segment segment;
		bool outerBarriers = true;  // the graph's conditions at its build
	};
}

namespace DCLF::Features::Fades
{
	namespace
	{
		Api::FrameValuePoolId pixelRowsPool;  // install
		std::uint32_t kickedRows = 0;  // render thread: the pool's last kick's rows

		// A row an object slot the frame's shading rows hold. Nothing is sampled or sent: PixelRowsCS writes every row before the frame's
		// draws read any.
		Api::FrameValueInput PixelRowsKick(const Api::FrameValuePoolKick& a_kick)
		{
			kickedRows = static_cast<std::uint32_t>(a_kick.objectSlots);
			return { nullptr, a_kick.objectSlots };
		}
		void PixelRowsSample(Api::FrameValuePoolProducer&) {}
		void PixelRowsSend(Api::FrameValuePoolProducer&) {}

		// t116: the frame's backing (a frame value pool's: only a frame record's commit names it).
		void SelectPixelRowsBinding(Api::FrameBindingSelect& a_select)
		{
			if (a_select.renderThread)
				a_select.index = FrameValues::Get().PoolIndex(pixelRowsPool);
		}

		// The latches' frame rows (Api::PassDef::frameRows): the frame's backing (none before the program: the pass writes nothing).
		Api::FrameRows PixelFrameRows()
		{
			const std::uint64_t address = FrameValues::Get().PoolAddress(pixelRowsPool);
			return { address, address ? kickedRows : 0u };
		}

		std::shared_ptr<const void> PixelRowsSetup(const Api::PassSetup& a_setup)
		{
			auto program = ComputeProgram::Load(a_setup.device, { .source = Draws::kPixelRowsShader, .constantWords = Draws::kPixelRowsConstantWords });
			if (!program)
				logger::error("[DCLF] The object rows program could not be created; DCLF's pixel stages read no object rows");
			return program;
		}

		std::shared_ptr<org::RenderPass> MakePixelRows(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::PixelRowsPass>(a_context.Main(), a_context.State<ComputeProgram>(), a_context.segment);
		}
	}

	void RegisterPixelRows(Api::InstallContext& a_context)
	{
		pixelRowsPool = a_context.RegisterFrameValuePool({ .name = "pixel-rows", .stride = Draws::kPixelObjectRowBytes, .deviceAddress = true, .kick = &PixelRowsKick,
			.sample = &PixelRowsSample, .send = &PixelRowsSend });
		a_context.RegisterFrameBinding({ .name = "object rows SRV", .textureRegister = kPixelObjectsRegister, .select = &SelectPixelRowsBinding });
		a_context.RegisterPass({ .key = "fades.pixel-rows", .passName = "cs.dclf.z.pixel-rows", .make = &MakePixelRows, .setup = &PixelRowsSetup,
			.frameRows = &PixelFrameRows, .cpuPipeline = true });
	}
}
