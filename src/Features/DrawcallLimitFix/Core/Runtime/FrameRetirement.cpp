// FrameRetirement: DCLF's retirement clock, advanced by the graph host's retired callback (RenderGraphRuntime.cpp) and by the CPU
// pipeline's frame timeline (Direct/Sites.cpp), over the points the frames took (see the header).
#include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"

#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <ORGModuleServices/BackingPool.h>
#include <OpenRenderGraph/PersistentGraphHost.h>
#include <rhi.h>

#include <algorithm>
#include <atomic>
#include <deque>

namespace DCLF
{
	namespace
	{
		/** @brief A point taken (SubmittedRetirementPoint) and the two counts it stands for. */
		struct TakenPoint
		{
			std::uint64_t point = 0, host = 0, cpu = 0;
		};

		struct Sources
		{
			// The points the render thread took, to the one advancing (PollRetirement's run), oldest first.
			EventQueue<TakenPoint, 256> taken;
			// The host's submissions retired (its callback, any thread), and the CPU frames signalled and their timeline (render thread;
			// the timeline is set once, before the first signal, and read by PollRetirement on any thread after an acquire of signalled).
			std::atomic<std::uint64_t> hostRetired{ 0 };
			std::atomic<std::uint64_t> cpuSignalled{ 0 };
			// The value the open CPU frame signals at its end (0: none opened yet): what it binds mid-frame retires after its own work.
			std::atomic<std::uint64_t> cpuOpen{ 0 };
			std::shared_ptr<rhi::TimelinePtr> cpuTimeline;
			// One run at a time: the noted points it has not passed yet are its own while it runs.
			std::atomic<bool> running{ false };
			std::deque<TakenPoint> pending;
			// The newest point noted: SubmittedRetirementPoint's callers are on several threads (the render thread, the builder's binds).
			std::atomic<std::uint64_t> lastTaken{ 0 };
		};

		Sources& State()
		{
			static Sources sources;
			return sources;
		}
	}

	const std::shared_ptr<org::services::RetirementClock>& FrameRetirement()
	{
		static const auto clock = std::make_shared<org::services::RetirementClock>();
		return clock;
	}

	std::uint64_t SubmittedRetirementPoint()
	{
		const auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return 0;
		auto& s = State();
		const std::uint64_t submissions = host->SubmissionsMade();
		// A point taken inside a CPU frame covers that frame's own work, which its signal (queued at its end) completes: a frame's sites
		// draw after their commits bound what they read, and the colour site can execute behind host submissions made meanwhile.
		const std::uint64_t cpu = std::max(s.cpuSignalled.load(std::memory_order_relaxed), s.cpuOpen.load(std::memory_order_relaxed));
		const std::uint64_t point = submissions + cpu;
		// Noted whatever the source (before any CPU frame a point is the host's count, which its callback also advances the clock to
		// directly), so the points taken before the first CPU frame retire after it as well.
		std::uint64_t last = s.lastTaken.load(std::memory_order_relaxed);
		while (point > last && !s.lastTaken.compare_exchange_weak(last, point, std::memory_order_relaxed)) {
		}
		if (point > last)
			s.taken.Push({ point, submissions, cpu });
		return point;
	}

	org::PersistentGraphHost::GpuPoint SubmittedGpuPoint()
	{
		const auto* host = RenderGraphRuntime::Get().Host();
		org::PersistentGraphHost::GpuPoint point = host ? host->SubmittedPoint() : org::PersistentGraphHost::GpuPoint{};
		auto& s = State();
		if (const std::uint64_t cpu = s.cpuSignalled.load(std::memory_order_relaxed); cpu && s.cpuTimeline)
			point.points.emplace_back(s.cpuTimeline, cpu);
		return point;
	}

	void NoteGraphSubmissionsRetired(std::uint64_t a_submissions)
	{
		auto& s = State();
		std::uint64_t seen = s.hostRetired.load(std::memory_order_relaxed);
		while (seen < a_submissions && !s.hostRetired.compare_exchange_weak(seen, a_submissions, std::memory_order_release, std::memory_order_relaxed)) {
		}
		// Before any CPU frame the points are the host's counts.
		if (!s.cpuOpen.load(std::memory_order_acquire))
			FrameRetirement()->Advance(a_submissions);
		PollRetirement();
	}

	void SetCpuFrameTimeline(std::shared_ptr<rhi::TimelinePtr> a_timeline)
	{
		State().cpuTimeline = std::move(a_timeline);
	}

	void NoteCpuFrameOpened(std::uint64_t a_value)
	{
		State().cpuOpen.store(a_value, std::memory_order_release);
	}

	void NoteCpuFrameSignalled(std::uint64_t a_value)
	{
		State().cpuSignalled.store(a_value, std::memory_order_release);
	}

	void PollHostRetirement()
	{
		if (const auto* host = RenderGraphRuntime::Get().Host()) {
			const std::uint64_t made = host->SubmissionsMade();
			if (made > State().hostRetired.load(std::memory_order_acquire) && host->SubmittedPoint().Reached()) {
				NoteGraphSubmissionsRetired(made);
				return;
			}
		}
		PollRetirement();
	}

	std::string RetirementState()
	{
		auto& s = State();
		const auto* host = RenderGraphRuntime::Get().Host();
		const std::uint64_t cpu = s.cpuTimeline && *s.cpuTimeline ? (*s.cpuTimeline)->GetCompletedValue() : 0;
		const auto front = s.pending.empty() ? TakenPoint{} : s.pending.front();
		return fmt::format("clock {} ({} waiting), last point taken {}, {} pending (first {}: host {} cpu {}), host retired {} of {} made, CPU frames "
						   "completed {} signalled {} open {}",
			FrameRetirement()->Completed(), FrameRetirement()->Waiting(), s.lastTaken.load(), s.pending.size(), front.point, front.host, front.cpu,
			s.hostRetired.load(), host ? host->SubmissionsMade() : 0, cpu, s.cpuSignalled.load(), s.cpuOpen.load());
	}

	void PollRetirement()
	{
		auto& s = State();
		if (s.running.exchange(true, std::memory_order_acquire))
			return;
		s.taken.Drain([&](TakenPoint&& a_point) { s.pending.push_back(a_point); });
		const std::uint64_t host = s.hostRetired.load(std::memory_order_acquire);
		const bool cpuFrames = s.cpuOpen.load(std::memory_order_acquire) != 0;
		const std::uint64_t cpu = cpuFrames && s.cpuTimeline && *s.cpuTimeline ? (*s.cpuTimeline)->GetCompletedValue() : 0;
		// Both counts rise with the points, so the noted points retire in order: the newest one both passed is the clock's.
		std::uint64_t reached = 0;
		while (!s.pending.empty() && s.pending.front().host <= host && s.pending.front().cpu <= cpu) {
			reached = s.pending.front().point;
			s.pending.pop_front();
		}
		if (reached)
			FrameRetirement()->Advance(reached);
		s.running.store(false, std::memory_order_release);
	}
}
