#pragma once

// The frame-value pool contract (Api::InstallContext::RegisterFrameValuePool): rows a feature samples every frame on DCLF's pool, sent
// into a pooled backing the frame's GPU work reads by descriptor. FrameValues (Core/Gpu) runs it beside its built-in parts.

#include <cstdint>
#include <memory>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/FrameValuePools.h), which the features inside DCLF include for them.
	class FrameValuePoolKick;
	class FrameValuePoolProducer;

	/** @brief A registered pool (RegisterFrameValuePool's result): FrameValues::PoolIndex names its frame's backing by it. */
	struct FrameValuePoolId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/** @brief What a pool's kick hands its frame's producer. */
	struct FrameValueInput
	{
		// Held by the frame's job until its producer is done, then let go on DCLF's pool (no engine reference may be in it). Kick runs
		// every frame: hand over a pointer the feature already holds, not a new allocation.
		std::shared_ptr<const void> input;
		// The rows the producer may leave: the frame's backing holds at least these (its class grows by doubling, never shrinks).
		std::uint64_t rows = 0;
	};

	/**
	 * @brief A frame-value pool: its rows, journalled by the feature (KeptWholeArray<Row>), each frame sent the runs its backing lacks.
	 *
	 * - kick: render thread, in FrameValues::Kick (the frame's start, after the engine's update), in registration order, after the
	 *   built-in parts' inputs. Not called on a frame without a producer.
	 * - sample: the frame's producer, on DCLF's preparation pool, after the built-in parts' sampling. The producers run one at a time in
	 *   frame order, so state only they touch needs no synchronisation. Opens the rows' build at the job's floor (BeginBuild), writes and
	 *   marks them; may fan out on SceneScheduler::Executor(); reads the engine only under an EngineReadWindow::Lease while Newest().
	 * - send: the same producer, after the built-in parts' sends: hands the rows to FrameValuePoolProducer::Send.
	 *
	 * The frame's GPU work reads the backing by FrameValues::PoolIndex (render thread, from Kick until the next; 0 without a producer).
	 * The backing is bound to the frame's last submission at EndFrame. A throw from sample or send fails the frame's values, as a built-in
	 * part's failure does (logged; the frame draws what its backings held).
	 */
	struct FrameValuePool
	{
		std::string_view name;    // the backing pool's name, cs.dclf.frame-values.<name>, and the report's
		std::uint32_t stride = 0;  // bytes a row
		FrameValueInput (*kick)(const FrameValuePoolKick& a_kick) = nullptr;
		void (*sample)(FrameValuePoolProducer& a_producer) = nullptr;
		void (*send)(FrameValuePoolProducer& a_producer) = nullptr;
	};
}
