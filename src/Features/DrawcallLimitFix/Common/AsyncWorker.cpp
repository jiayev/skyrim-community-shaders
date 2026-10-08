#include "AsyncWorker.h"
#include <Tracy/Tracy.hpp>

#include "SceneScheduler.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <Windows.h>

#include <map>
#include <optional>
#include <semaphore>
#include <vector>

namespace DCLF
{
	struct AsyncWorker::Job
	{
		enum class State : std::uint8_t
		{
			Queued,
			Running,
			Done,
			Failed,
			Cancelled
		};

		const char* name = "";
		std::function<void(std::stop_token)> run;
		std::stop_source stop;
		// Queued -> Running by the coordinator, Queued -> Cancelled by the render thread: whichever exchange wins. The end is
		// written by whoever ends the job, then `finished` is released once; a waiter that acquires it releases it again.
		std::atomic<State> state{ State::Queued };
		std::binary_semaphore finished{ 0 };
		std::chrono::steady_clock::time_point submitted;
		std::chrono::steady_clock::time_point started;  // written before `state` ends the job
		std::chrono::steady_clock::time_point ended;
		bool waited = false;  // the join happened (stats are recorded once); render thread

		bool Ended() const
		{
			const auto now = state.load(std::memory_order_acquire);
			return now != State::Queued && now != State::Running;
		}
		/** @brief Blocks until the job has ended (at most a_budget), then leaves `finished` released for the next waiter. */
		bool AwaitEnd(std::optional<std::chrono::microseconds> a_budget)
		{
			if (Ended())
				return true;
			const bool acquired = a_budget ? finished.try_acquire_for(*a_budget) : (finished.acquire(), true);
			if (acquired)
				finished.release();
			return acquired;
		}
	};

	namespace
	{
		/** @brief Ends a queued job before it runs; false when the coordinator took it first. */
		bool CancelQueued(AsyncWorker::Job& a_job)
		{
			auto expected = AsyncWorker::Job::State::Queued;
			const auto now = std::chrono::steady_clock::now();
			if (!a_job.state.compare_exchange_strong(expected, AsyncWorker::Job::State::Running, std::memory_order_acq_rel))
				return false;
			a_job.started = a_job.ended = now;
			a_job.state.store(AsyncWorker::Job::State::Cancelled, std::memory_order_release);
			a_job.finished.release();
			return true;
		}
	}

	struct AsyncWorker::Impl
	{
		// The jobs submitted and not yet seen ended (render thread only): what Drain covers.
		std::vector<std::shared_ptr<Job>> outstanding;
		std::map<const char*, JobStats> stats;  // render thread
		// The render thread's waits since the last report (RenderWaitReport): by site, blocked count and time.
		struct WaitSite
		{
			std::uint32_t blocked = 0;
			double ms = 0.0;
		};
		std::map<std::string, WaitSite> renderWaits;
		std::uint32_t frames = 0;
		void NoteWait(std::string_view a_site, const char* a_job, std::chrono::steady_clock::duration a_waited)
		{
			auto& site = renderWaits[fmt::format("{} {}", a_site, a_job)];
			++site.blocked;
			site.ms += std::chrono::duration<double, std::milli>(a_waited).count();
		}
		void Prune()
		{
			std::erase_if(outstanding, [](const std::shared_ptr<Job>& a_job) { return a_job->Ended(); });
		}

		/** @brief On the coordinator: the job, unless it was cancelled while queued. */
		static void Run(Job& a_job)
		{
			auto expected = Job::State::Queued;
			if (!a_job.state.compare_exchange_strong(expected, Job::State::Running, std::memory_order_acq_rel))
				return;  // cancelled while queued: its canceller ended it
			a_job.started = std::chrono::steady_clock::now();
			Job::State result = Job::State::Done;
			try {
				ZoneScopedN("CS.DCLF.Worker.Run");
				ZoneText(a_job.name, std::char_traits<char>::length(a_job.name));
				a_job.run(a_job.stop.get_token());
			} catch (...) {
				result = Job::State::Failed;
			}
			if (a_job.stop.stop_requested())
				result = Job::State::Cancelled;
			a_job.run = {};  // its captures are released here, on the coordinator
			a_job.ended = std::chrono::steady_clock::now();
			a_job.state.store(result, std::memory_order_release);
			a_job.finished.release();
		}
	};

	AsyncWorker& AsyncWorker::Get()
	{
		static AsyncWorker worker;
		return worker;
	}

	AsyncWorker::AsyncWorker() :
		impl(std::make_unique<Impl>())
	{
		(void)SceneScheduler::Executor();
	}

	AsyncWorker::~AsyncWorker() = default;

	AsyncWorker::JobHandle AsyncWorker::SubmitScene(const char* a_name, std::function<void(std::stop_token)> a_job)
	{
		auto job = std::make_shared<Job>();
		job->name = a_name;
		job->run = std::move(a_job);
		job->submitted = std::chrono::steady_clock::now();
		impl->Prune();
		impl->outstanding.push_back(job);
		++impl->stats[a_name].kicked;
		if (!SceneScheduler::SceneLane().Dispatch(SceneScheduler::SceneLaneScope(), PublishedSceneExecutor::Coordinator, org::async::TaskDispatch::Controlled,
				a_name, [job](const auto&) { Impl::Run(*job); }))
			CancelQueued(*job);
		JobHandle handle;
		handle.job = std::move(job);
		return handle;
	}

	AsyncWorker::WaitResult AsyncWorker::Wait(const JobHandle& a_handle, std::chrono::microseconds a_budget)
	{
		ZoneScopedN("CS.DCLF.Worker.Wait");
		auto& job = a_handle.job;
		if (!job)
			return WaitResult::None;
		const auto waitStart = std::chrono::steady_clock::now();
		const bool blocked = !job->Ended();
		const bool finished = job->AwaitEnd(a_budget);
		const auto waited = std::chrono::steady_clock::now() - waitStart;
		if (blocked)
			impl->NoteWait("join", job->name, waited);
		RenderGraphRuntime::AddEpochJoinWait(waited);
		const double waitedMs = std::chrono::duration<double, std::milli>(waited).count();
		auto& stats = impl->stats[job->name];
		if (!std::exchange(job->waited, true)) {
			stats.waitTotalMs += waitedMs;
			stats.waitMaxMs = (std::max)(stats.waitMaxMs, waitedMs);
			stats.windowTotalMs += std::chrono::duration<double, std::milli>(waitStart - job->submitted).count();
		}
		if (!finished) {
			++stats.late;
			return WaitResult::Late;
		}
		const double builtMs = std::chrono::duration<double, std::milli>(job->ended - job->started).count();
		stats.buildTotalMs += builtMs;
		stats.queuedTotalMs += std::chrono::duration<double, std::milli>(job->started - job->submitted).count();
		stats.buildMaxMs = (std::max)(stats.buildMaxMs, builtMs);
		switch (job->state.load(std::memory_order_acquire)) {
		case Job::State::Done:
			++stats.onTime;
			return WaitResult::Done;
		case Job::State::Failed:
			++stats.failed;
			return WaitResult::Failed;
		default:
			++stats.cancelled;
			return WaitResult::Cancelled;
		}
	}

	void AsyncWorker::Drain()
	{
		ZoneScopedN("CS.DCLF.Worker.Drain");
		for (auto& job : impl->outstanding)
			if (CancelQueued(*job))
				++impl->stats[job->name].cancelled;
		impl->Prune();
		for (auto& job : impl->outstanding) {
			if (job->Ended())
				continue;
			const auto start = std::chrono::steady_clock::now();
			job->stop.request_stop();
			job->AwaitEnd(std::nullopt);
			impl->NoteWait("drain", job->name, std::chrono::steady_clock::now() - start);
		}
		impl->Prune();
	}

	void AsyncWorker::NoteFrame()
	{
		++impl->frames;
	}

	std::string AsyncWorker::RenderWaitReport()
	{
		auto& d = *impl;
		if (!d.frames)
			return {};
		const double n = d.frames;
		std::uint32_t blocked = 0;
		double ms = 0.0;
		std::string sites;
		for (const auto& [site, wait] : d.renderWaits) {
			blocked += wait.blocked;
			ms += wait.ms;
			sites += fmt::format("{}{} {:.2f}/frame {:.3f} ms/frame", sites.empty() ? "" : ", ", site, wait.blocked / n, wait.ms / n);
		}
		const auto pool = SceneScheduler::Executor().GetStatistics();
		auto text = fmt::format(
			"[DCLF] render-thread waits over {} frames: {:.2f} blocked/frame, {:.3f} ms/frame{}{}{}; executor: coordinator queue high water {}, {} preparation threads "
			"(queue high water {}), {} accepted, {} rejected, {} failed\n",
			d.frames, blocked / n, ms / n, sites.empty() ? "" : "; by site: ", sites, blocked ? "" : " <- OK", pool.highWater[0], SceneScheduler::PreparationWorkers(),
			pool.highWater[1], pool.accepted, pool.rejected, pool.failed);
		d.renderWaits.clear();
		d.frames = 0;
		return text;
	}

	std::string AsyncWorker::Report()
	{
		std::string text;
		for (const auto& [name, s] : impl->stats) {
			if (!s.kicked)
				continue;
			const std::uint32_t joined = s.onTime + s.failed + s.cancelled;
			text += fmt::format("[DCLF] async {}: {} kicked, {} joined (waited {:.3f}/{:.3f} ms, built {:.3f}/{:.3f} ms, queued {:.3f} ms, window {:.3f} ms), {} late -> inline, {} failed, {} cancelled\n",
				name, s.kicked, joined,
				s.kicked ? s.waitTotalMs / s.kicked : 0.0, s.waitMaxMs,
				joined ? s.buildTotalMs / joined : 0.0, s.buildMaxMs,
				joined ? s.queuedTotalMs / joined : 0.0, s.kicked ? s.windowTotalMs / s.kicked : 0.0,
				s.late, s.failed, s.cancelled);
		}
		return text;
	}

	void AsyncWorker::ResetStats()
	{
		impl->stats.clear();
	}
}
