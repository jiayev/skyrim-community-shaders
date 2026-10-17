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
	 * host's retired callback (PersistentGraphHost::SetRetiredCallback): every submission up to a number is complete on the GPU, the
	 * submissions numbered in their order (PersistentGraphHost::SubmissionsMade): exact completion, not a frames-in-flight window.
	 * Never from host frame numbers: an async epoch's ticket is numbered when it is prepared, so a frame's last submission may carry a
	 * number the clock has passed, and a later-numbered one completing says nothing of one submitted after it. Created on first use;
	 * lives with the process.
	 *
	 * Points: submission counts, so point 0 is "nothing" and a lease bound at 0 waits for nothing.
	 */
	const std::shared_ptr<org::services::RetirementClock>& FrameRetirement();

	/**
	 * @brief Submitting (render) thread: the point that retires once everything submitted so far is done on the GPU
	 * (PersistentGraphHost::SubmissionsMade); 0 without a graph host. A frame binds what it read with this after its last submission
	 * (BackingLease::Bind), so a backing it used is not reused before that frame completes.
	 */
	std::uint64_t SubmittedRetirementPoint();
}
