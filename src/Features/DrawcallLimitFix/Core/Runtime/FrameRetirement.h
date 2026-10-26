#pragma once

#include <OpenRenderGraph/PersistentGraphHost.h>

#include <cstdint>
#include <string>
#include <memory>

namespace org::services
{
	class RetirementClock;
}

namespace DCLF
{
	/**
	 * @brief DCLF's one retirement clock (org::services::RetirementClock, D3): the GPU's progress over everything DCLF submits, which every
	 * pooled version DCLF leases (the payload's parts, FrameValues' buffers) is bound against. Two sources: the graph host's executions,
	 * from its retired callback (PersistentGraphHost::SetRetiredCallback: every submission up to a number is complete on the GPU, the
	 * submissions numbered in their order, PersistentGraphHost::SubmissionsMade), and the CPU pipeline's frames, whose work is in DXVK's
	 * own command buffers (Direct/Sites.cpp: each frame's signal on its CPU-frame timeline). Exact completion, not a frames-in-flight
	 * window. Never from host frame numbers: an async epoch's ticket is numbered when it is prepared, so a frame's last submission may
	 * carry a number the clock has passed, and a later-numbered one completing says nothing of one submitted after it. Created on first
	 * use; lives with the process.
	 *
	 * Points: the host's submissions plus the CPU frames signalled, counted when a point is taken (SubmittedRetirementPoint), so point 0 is
	 * "nothing" and a lease bound at 0 waits for nothing. A point retires once both of its counts have: each taken point is noted with
	 * them, and the clock advances to the newest noted point both sources passed (both counts only rise, so the noted points retire in
	 * order). Without CPU frames the points are the host's submission counts, as before.
	 */
	const std::shared_ptr<org::services::RetirementClock>& FrameRetirement();

	/**
	 * @brief Submitting (render) thread: the point that retires once everything submitted so far is done on the GPU (the host's
	 * submissions and the CPU frames signalled); 0 without a graph host. A frame binds what it read with this after its last submission
	 * (BackingLease::Bind), so a backing it used is not reused before that frame completes.
	 */
	std::uint64_t SubmittedRetirementPoint();

	/**
	 * @brief Submitting (render) thread: the GPU points SubmittedRetirementPoint's point stands for (the host's SubmittedPoint, and the
	 * CPU-frame timeline at its last signal), for a GPU wait on it (FrameValues' uploads into a copy a frame in flight bound).
	 */
	org::PersistentGraphHost::GpuPoint SubmittedGpuPoint();

	/** @brief The graph host's retired callback (any thread): every host submission up to a_submissions is complete. */
	void NoteGraphSubmissionsRetired(std::uint64_t a_submissions);

	/**
	 * @brief Render thread (Direct/Sites.cpp): the CPU pipeline's frame timeline (made once), and each frame's signal on it queued behind
	 * the frame's callbacks (a_value: 1, 2, ...).
	 */
	void SetCpuFrameTimeline(std::shared_ptr<rhi::TimelinePtr> a_timeline);
	void NoteCpuFrameSignalled(std::uint64_t a_value);
	/**
	 * @brief Render thread, at a CPU frame's start: a_value is the signal its end will queue (its last value signalled when it is not one),
	 * which every point taken until then includes (SubmittedRetirementPoint), so nothing the frame binds retires before its work.
	 */
	void NoteCpuFrameOpened(std::uint64_t a_value);

	/**
	 * @brief Any thread: advances the clock to the newest noted point both sources passed (the CPU frames by their timeline's completed
	 * value). The host's callback and each frame's start run it; one runs at a time, another caller returns at once (the next run
	 * catches up), so nobody waits.
	 */
	void PollRetirement();
	/**
	 * @brief The host's submitting thread (the render thread), while it waits for GPU work submitting nothing: the host's submissions
	 * retired once everything it submitted is done (SubmittedPoint reached), then PollRetirement. The host reports them only when it
	 * reuses a slot (its next submission), so a wait for a point they cover would otherwise wait for itself.
	 */
	void PollHostRetirement();
	/** @brief Any thread, read racily (for a stall's message): the clock, the points noted and both sources' counts. */
	std::string RetirementState();
}
