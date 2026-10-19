#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBatch.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBatchLayout.h"

namespace DCLF::Features::Portals
{
	namespace
	{
		struct Constants
		{
			std::uint32_t queries, results, programs, counts, offsets, ranks, indices, tasks, dispatch;
			std::uint32_t queryCount, bucketCount, base, elements, sumsBase, parentBase;
			std::uint32_t latchIndex, latchOffset, padding[3];  // the depth latch, whose portalProgramsIndex names the programs
		};
		static_assert(sizeof(Constants) == 80);
		constexpr std::uint32_t kConstantWords = sizeof(Constants) / 4;
		struct Bindings
		{
			org::DeclaredViewToken queries, results, counts, offsets, ranks, indices, tasks, dispatch;
			org::ResourceBindingToken indirect;
		};
		struct Prepared
		{
			std::shared_ptr<const Portals::Batch> batch;
			std::shared_ptr<const org::LatchBlock> latch;
			Constants constants{};
			org::PreparedResourceReference indirect;
			bool evaluate = false;
		};

		class BatchPass final : public org::TypedRenderGraphPass<BatchPass, Prepared, Bindings>
		{
		public:
			BatchPass(std::shared_ptr<Draws::Resources> a_resources, std::shared_ptr<const Batch> a_batch, RenderGraphRuntime::Segment a_segment, bool a_evaluate) :
				resources(std::move(a_resources)), batch(std::move(a_batch)), segment(a_segment), evaluate(a_evaluate) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Compute);
				const auto& b = *batch;
				Bindings bindings{};
				bindings.queries = a_builder.ShaderResource(*b.queries).View();
				bindings.results = a_builder.UnorderedAccess(*b.results).View();
				// The programs are the frame's publication, named by the depth latch: an immutable pooled backing no pass declares.
				if (evaluate) {
					bindings.dispatch = a_builder.ShaderResource(b.dispatch).View();
					bindings.indices = a_builder.ShaderResource(*b.indices).View();
					bindings.tasks = a_builder.ShaderResource(*b.tasks).View();
					bindings.indirect = a_builder.IndirectArguments(b.dispatch);
				} else {
					bindings.counts = a_builder.UnorderedAccess(*b.counts).View();
					bindings.offsets = a_builder.UnorderedAccess(*b.offsets).View();
					bindings.ranks = a_builder.UnorderedAccess(*b.ranks).View();
					bindings.indices = a_builder.UnorderedAccess(*b.indices).View();
					bindings.tasks = a_builder.UnorderedAccess(*b.tasks).View();
					bindings.dispatch = a_builder.UnorderedAccess(b.dispatch).View();
				}
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
			{
				const auto frame = Draws::CurrentFrame(a_preparation, segment);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(evaluate);
				const auto& b = *batch;
				for (const auto* buffer : { &b.queries, &b.results, &b.counts, &b.offsets, &b.ranks, &b.indices, &b.tasks })
					a_out.push_back(Draws::NamedSerial(a_preparation, *buffer));
			}

			Prepared Prepare(const Bindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				Prepared prepared{};
				const auto frame = Draws::CurrentFrame(a_preparation, segment);
				if (!frame || !frame->latch)
					return prepared;
				prepared.batch = batch;
				prepared.latch = frame->latch;
				prepared.evaluate = evaluate;
				const auto& b = *prepared.batch;
				auto& c = prepared.constants;
				const auto index = [&](const org::DeclaredViewToken& token) { return token.layout ? Draws::CaptureViewIndex(a_preparation, token) : 0u; };
				c.queries = index(a_bindings.queries);
				c.results = index(a_bindings.results);
				c.latchIndex = frame->latch->SrvIndex();
				c.counts = index(a_bindings.counts);
				c.offsets = index(a_bindings.offsets);
				c.ranks = index(a_bindings.ranks);
				c.indices = index(a_bindings.indices);
				c.tasks = index(a_bindings.tasks);
				c.dispatch = index(a_bindings.dispatch);
				c.queryCount = static_cast<std::uint32_t>(Draws::NamedUnitsOf(a_preparation, b.queries));
				c.bucketCount = static_cast<std::uint32_t>(std::min(Draws::NamedUnitsOf(a_preparation, b.counts), Draws::NamedUnitsOf(a_preparation, b.offsets)) / 2);
				if (evaluate)
					prepared.indirect = a_preparation.CaptureResource(a_bindings.indirect);
				return prepared;
			}

			static void Record(const Bindings&, const Prepared& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.batch)
					return;
				const auto& batch = *a_prepared.batch;
				auto& commands = a_recording.Commands();
				auto c = a_prepared.constants;
				c.latchOffset = static_cast<std::uint32_t>(a_prepared.latch->Offset(a_recording.FrameSlot()));
				const auto bind = [&](std::size_t stage) {
					const auto& program = *batch.programs[stage];
					commands.BindLayout(program.layout->GetHandle());
					commands.BindPipeline(program.pipeline->GetHandle());
					commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&c));
				};
				if (a_prepared.evaluate) {
					bind(7);
					commands.ExecuteIndirect(batch.signature->GetHandle(), a_recording.Resolve(a_prepared.indirect).GetHandle(), 0, {}, 0, 1);
					return;
				}
				const auto dispatch = [&](std::size_t stage, std::uint32_t elements, std::uint32_t width = 64u) {
					const auto shape = DCLF::Portals::DispatchFor(elements, width);
					if (!shape.x)
						return;
					bind(stage);
					commands.Dispatch(shape.x, shape.y, 1);
					const auto barrier = rhi::FullMemoryBarrier();
					commands.Barriers(rhi::BarrierBatch{ {}, {}, { &barrier, 1 } });
				};
				dispatch(0, c.bucketCount);
				dispatch(1, c.queryCount);
				dispatch(2, c.bucketCount);
				const auto scan = DCLF::Portals::ScanFor(c.bucketCount);
				const auto& levels = scan.levels;
				for (std::size_t i = 0; i < levels.size(); ++i) {
					c.base = levels[i].base;
					c.elements = levels[i].elements;
					c.sumsBase = i + 1 < levels.size() ? levels[i + 1].base : 0;
					dispatch(3, c.elements, 256);
				}
				for (std::size_t i = levels.size() - 1; i > 0; --i) {
					c.base = levels[i - 1].base;
					c.elements = levels[i - 1].elements;
					c.parentBase = levels[i].base;
					dispatch(4, c.elements);
				}
				dispatch(5, c.queryCount);
				dispatch(6, c.bucketCount);
			}

		private:
			std::shared_ptr<Draws::Resources> resources;
			std::shared_ptr<const Batch> batch;
			RenderGraphRuntime::Segment segment;
			bool evaluate;
		};

	}

	std::shared_ptr<org::RenderPass> MakeBatchPass(std::shared_ptr<Draws::Resources> a_resources, std::shared_ptr<const Batch> a_batch,
		RenderGraphRuntime::Segment a_segment, bool a_evaluate)
	{
		if (!a_batch)
			return nullptr;
		return std::make_shared<BatchPass>(std::move(a_resources), std::move(a_batch), a_segment, a_evaluate);
	}
}
