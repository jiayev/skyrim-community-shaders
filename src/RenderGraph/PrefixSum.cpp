#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "PrefixSum.h"

#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <rhi_helpers.h>

namespace PrefixSum
{
	namespace
	{
		constexpr const char* kSource = "RenderGraph/PrefixSumCS.hlsl";
		constexpr std::uint32_t kNoBuffer = ~0u;
		constexpr std::uint32_t kClearCounts = 1;

		// PrefixSumCS.hlsl's push constants.
		struct Constants
		{
			std::uint32_t countsIndex;
			std::uint32_t offsetsIndex;
			std::uint32_t blockSumsIndex;
			std::uint32_t totalIndex;
			std::uint32_t elements;
			std::uint32_t blocks;
			std::uint32_t flags;
			std::uint32_t padding;
		};
		constexpr std::uint32_t kConstantWords = sizeof(Constants) / 4;

		struct Bindings
		{
			org::ResourceBindingToken counts, offsets, blockSums, total;
		};

		struct Frame
		{
			std::shared_ptr<const Programs> programs;
			Constants constants{};
		};

		class Pass final : public org::TypedRenderGraphPass<Pass, Frame, Bindings>
		{
		public:
			explicit Pass(Desc a_desc) :
				desc(std::move(a_desc)) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				Bindings bindings{};
				bindings.counts = desc.clearCounts ? a_builder.BindUnorderedAccess(desc.counts) : a_builder.BindShaderResource(desc.counts);
				bindings.offsets = a_builder.BindUnorderedAccess(desc.offsets);
				bindings.blockSums = a_builder.BindUnorderedAccess(desc.blockSums);
				if (desc.total)
					bindings.total = a_builder.BindUnorderedAccess(desc.total);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(!desc.active || desc.active() ? 1 : 0);
				if (desc.revision)
					desc.revision(a_out);
			}

			Frame Prepare(const Bindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				Frame prepared{};
				if (!desc.programs || !desc.elements || desc.elements > kMaxElements || (desc.active && !desc.active()))
					return prepared;
				prepared.programs = desc.programs;
				auto& constants = prepared.constants;
				const auto countsView = desc.clearCounts ? org::BindlessViewKind::UnorderedAccess : org::BindlessViewKind::ShaderResource;
				constants.countsIndex = a_preparation.ResolveView(a_bindings.counts, { countsView }).index;
				constants.offsetsIndex = a_preparation.ResolveView(a_bindings.offsets, { org::BindlessViewKind::UnorderedAccess }).index;
				constants.blockSumsIndex = a_preparation.ResolveView(a_bindings.blockSums, { org::BindlessViewKind::UnorderedAccess }).index;
				constants.totalIndex = desc.total ? a_preparation.ResolveView(a_bindings.total, { org::BindlessViewKind::UnorderedAccess }).index : kNoBuffer;
				constants.elements = desc.elements;
				constants.blocks = Blocks(desc.elements);
				constants.flags = desc.clearCounts ? kClearCounts : 0u;
				return prepared;
			}

			static void Record(const Bindings&, const Frame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.programs)
					return;
				auto& commands = a_recording.Commands();
				const auto& scan = *a_frame.programs->blockScan;
				commands.BindLayout(scan.layout->GetHandle());
				commands.BindPipeline(scan.pipeline->GetHandle());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
				commands.Dispatch(a_frame.constants.blocks, 1, 1);
				// The block sums and local offsets must be complete before the second dispatch reads them. One pass with a barrier
				// rather than two passes: the second dispatch reads and writes the offsets the first wrote.
				const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
				commands.Barriers(rhi::BarrierBatch{ {}, {}, { &global, 1 } });
				const auto& offsets = *a_frame.programs->blockOffsets;
				commands.BindLayout(offsets.layout->GetHandle());
				commands.BindPipeline(offsets.pipeline->GetHandle());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
				commands.Dispatch(1, 1, 1);
			}

		private:
			Desc desc;
		};
	}

	std::shared_ptr<const Programs> Load(rhi::Device a_device)
	{
		auto programs = std::make_shared<Programs>();
		programs->blockScan = ComputeProgram::Load(a_device, { .source = kSource, .entry = L"BlockScan", .constantWords = kConstantWords });
		programs->blockOffsets = ComputeProgram::Load(a_device, { .source = kSource, .entry = L"BlockOffsets", .constantWords = kConstantWords });
		if (!programs->blockScan || !programs->blockOffsets)
			return {};
		return programs;
	}

	std::shared_ptr<org::RenderPass> CreatePass(Desc a_desc)
	{
		return std::make_shared<Pass>(std::move(a_desc));
	}
}

#endif
