#pragma once

// The index pool's copy pass (IndexPoolCS.hlsl), made for the main and shadow epochs (CorePasses.cpp: core.z.index-pool; Passes/Shadow.cpp MakeShadowIndexPoolPass: shadows.index-pool, registered by Features/Shadows/Engine).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

namespace DCLF::Draws
{
	struct IndexPoolBindings
	{
		org::DeclaredViewToken copies, indices;
	};

	struct IndexPoolPrepared
	{
		std::shared_ptr<const IndexPool> pool;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t poolOffset = 0;  // ShadowLatchLayout::PoolOffset
		IndexPoolConstants constants{};
	};

	/** @brief Copies this frame's new index pool ranges (IndexPoolCS.hlsl) before draws read the pool; dispatched from the latch. */
	class IndexPoolPass final : public org::TypedRenderGraphPass<IndexPoolPass, IndexPoolPrepared, IndexPoolBindings>
	{
	public:
		// The latch holding the pool's dispatch, from the shape the epoch runs (shadow or depth: whichever commit gave out ranges).
		struct LatchOf
		{
			std::uint64_t generation = 0;
			std::shared_ptr<const org::LatchBlock> latch;
			std::uint32_t poolOffset = 0;
		};
		IndexPoolPass(std::shared_ptr<IndexPool> a_pool, std::function<LatchOf(const org::PassPrepareContext&)> a_latch) :
			pool(std::move(a_pool)), latchOf(std::move(a_latch)) {}

		IndexPoolBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			IndexPoolBindings bindings{};
			bindings.copies = a_builder.ShaderResource(*pool->copies).View();
			bindings.indices = a_builder.UnorderedAccess(*pool->indices).View();
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto latch = latchOf(a_preparation);
			a_out.push_back(latch.generation);
			a_out.push_back(latch.poolOffset);
			a_out.push_back(NamedSerial(a_preparation, pool->copies));
			a_out.push_back(NamedSerial(a_preparation, pool->indices));
		}

		IndexPoolPrepared Prepare(const IndexPoolBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			IndexPoolPrepared prepared{};
			const auto latch = latchOf(a_preparation);
			if (!latch.latch)
				return prepared;
			prepared.pool = pool;
			prepared.latch = latch.latch;
			prepared.poolOffset = latch.poolOffset;
			prepared.constants.latchIndex = latch.latch->SrvIndex();
			prepared.constants.copiesIndex = CaptureViewIndex(a_preparation, a_bindings.copies);
			prepared.constants.poolIndex = CaptureViewIndex(a_preparation, a_bindings.indices);
			return prepared;
		}

		static void Record(const IndexPoolBindings&, const IndexPoolPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.pool || !a_prepared.latch)
				return;
			auto constants = a_prepared.constants;
			const auto& layout = a_prepared.latch;
			const std::uint64_t offset = layout->Offset(a_recording.FrameSlot()) + a_prepared.poolOffset;
			constants.latchOffset = static_cast<std::uint32_t>(offset);
			auto& commands = a_recording.Commands();
			const auto& program = *a_prepared.pool->program;
			commands.BindLayout(program.layout->GetHandle());
			commands.BindPipeline(program.Pipeline());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kIndexPoolConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.ExecuteIndirect(a_prepared.pool->dispatchSignature->GetHandle(), layout->Resource()->GetAPIResource().GetHandle(), offset, {}, 0, 1);
		}

	private:
		std::shared_ptr<IndexPool> pool;
		std::function<LatchOf(const org::PassPrepareContext&)> latchOf;
	};
}
