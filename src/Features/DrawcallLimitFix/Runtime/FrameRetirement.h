#pragma once

#include <cstdint>
#include <memory>

namespace org::services
{
	class RetirementClock;
}

namespace DCLF
{
	/**
	 * @brief DCLF's one retirement clock (org::services::RetirementClock, D3): the GPU's progress over the graph host's executions,
	 * which every pooled version DCLF leases (the payload's parts, FrameValues' buffers) is bound against. Advanced from the graph
	 * host's completed-frame callback (RenderGraphRuntime's OnCompletedFrame), which reports each execution once it is complete on
	 * the GPU, in submission order: exact completion, not a frames-in-flight window. Created on first use; lives with the process.
	 *
	 * Points: a host frame number h (PersistentGraphHost::LastHostFrame, 0-based) retires as point RetirementPoint(h) = h + 1, so
	 * point 0 is "nothing" and a lease bound at 0 waits for nothing.
	 */
	const std::shared_ptr<org::services::RetirementClock>& FrameRetirement();

	/** @brief The clock point host frame a_hostFrame retires as. */
	constexpr std::uint64_t RetirementPoint(std::uint64_t a_hostFrame) noexcept { return a_hostFrame + 1; }

	/**
	 * @brief Submitting (render) thread: the point that retires once everything submitted so far is done on the GPU
	 * (RetirementPoint(LastHostFrame())); 0 without a graph host. A frame binds what it read with this after its last submission
	 * (BackingLease::Bind), so a backing it used is not reused before that frame completes.
	 */
	std::uint64_t SubmittedRetirementPoint();
}
