#include "AsyncWorker.h"
#include <Tracy/Tracy.hpp>

#include "Switches.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <Windows.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace DCLF
{
	AsyncMode AsyncModeSetting()
	{
		static const AsyncMode mode = [] {
			// On unless turned off: the full featureset is what testers and every validation run exercise.
			const auto value = SwitchValue(Switch::Async);
			if (value == "off" || value == "0")
				return AsyncMode::Off;
			if (value == "probe")
				return AsyncMode::Probe;
			return AsyncMode::On;
		}();
		return mode;
	}

	std::chrono::microseconds AsyncWaitBudget()
	{
		static const std::chrono::microseconds budget = [] {
			const auto value = SwitchValue(Switch::AsyncWaitMs);
			const double ms = value.empty() ? 3.0 : std::strtod(value.c_str(), nullptr);
			return std::chrono::microseconds(static_cast<long long>(ms * 1000.0));
		}();
		return budget;
	}

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
		std::mutex mutex;
		std::condition_variable finished;
		State state = State::Queued;
		std::chrono::steady_clock::time_point submitted;
		std::chrono::steady_clock::time_point started;
		std::chrono::steady_clock::time_point ended;
		bool waited = false;  // the join happened (stats are recorded once)
	};

	namespace
	{
		/** @brief Asks a started job to stop and waits for it to end: its run returns at its next stop check. */
		void StopAndJoin(AsyncWorker::Job& a_job)
		{
			a_job.stop.request_stop();
			std::unique_lock lock(a_job.mutex);
			a_job.finished.wait(lock, [&] { return a_job.state != AsyncWorker::Job::State::Queued && a_job.state != AsyncWorker::Job::State::Running; });
		}
	}

	struct AsyncWorker::Impl
	{
		std::mutex queueMutex;
		std::condition_variable queued;
		std::deque<std::shared_ptr<Job>> queue;
		std::shared_ptr<Job> running;  // under queueMutex
		std::condition_variable idle;  // notified whenever a job ends; WaitIdle checks the queue under queueMutex
		bool stopping = false;
		std::map<const char*, JobStats> stats;  // render thread
		// The render thread's waits since the last report (RenderWaitReport): by site, blocked count and time.
		struct WaitSite
		{
			std::uint32_t blocked = 0;
			double ms = 0.0;
		};
		std::map<std::string, WaitSite> renderWaits;
		std::uint64_t renderLocks = 0;
		std::uint32_t frames = 0;
		void NoteWait(std::string_view a_site, const char* a_job, std::chrono::steady_clock::duration a_waited)
		{
			auto& site = renderWaits[fmt::format("{} {}", a_site, a_job)];
			++site.blocked;
			site.ms += std::chrono::duration<double, std::milli>(a_waited).count();
		}
		std::jthread thread;

		void Loop(std::stop_token a_stop)
		{
			SetThreadDescription(GetCurrentThread(), L"CS DCLF worker");
			// The engine's job threads run at normal priority; a starved worker costs one inline fallback, never
			// correctness, so the worker only goes above them when asked not to.
			if (SwitchValue(Switch::AsyncPriority) != "normal")
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
			for (;;) {
				std::shared_ptr<Job> job;
				{
					std::unique_lock lock(queueMutex);
					queued.wait(lock, [&] { return stopping || a_stop.stop_requested() || !queue.empty(); });
					if (queue.empty())
						return;
					job = std::move(queue.front());
					queue.pop_front();
					running = job;
				}
				{
					std::lock_guard lock(job->mutex);
					job->state = Job::State::Running;
					job->started = std::chrono::steady_clock::now();
				}
				Job::State result = Job::State::Done;
				try {
					ZoneScopedN("CS.DCLF.Worker.Run");
					ZoneText(job->name, std::char_traits<char>::length(job->name));
					job->run(job->stop.get_token());
				} catch (...) {
					result = Job::State::Failed;
				}
				if (job->stop.stop_requested())
					result = Job::State::Cancelled;
				{
					std::lock_guard lock(job->mutex);
					job->state = result;
					job->ended = std::chrono::steady_clock::now();
				}
				job->finished.notify_all();
				{
					std::lock_guard lock(queueMutex);
					running.reset();
				}
				idle.notify_all();
			}
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
		impl->thread = std::jthread([this](std::stop_token a_stop) { impl->Loop(a_stop); });
	}

	AsyncWorker::~AsyncWorker()
	{
		Drain();
		{
			std::lock_guard lock(impl->queueMutex);
			impl->stopping = true;
		}
		impl->thread.request_stop();
		impl->queued.notify_all();
	}

	AsyncWorker::JobHandle AsyncWorker::Submit(const char* a_name, std::function<void(std::stop_token)> a_job)
	{
		auto job = std::make_shared<Job>();
		job->name = a_name;
		job->run = std::move(a_job);
		job->submitted = std::chrono::steady_clock::now();
		{
			std::lock_guard lock(impl->queueMutex);
			impl->queue.push_back(job);
		}
		++impl->renderLocks;
		impl->queued.notify_one();
		++impl->stats[a_name].kicked;
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
		std::unique_lock lock(job->mutex);
		++impl->renderLocks;
		const bool blocked = job->state == Job::State::Queued || job->state == Job::State::Running;
		const bool finished = job->finished.wait_for(lock, a_budget, [&] {
			return job->state != Job::State::Queued && job->state != Job::State::Running;
		});
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
		switch (job->state) {
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

	void AsyncWorker::CancelPending()
	{
		std::deque<std::shared_ptr<Job>> dropped;
		{
			std::lock_guard lock(impl->queueMutex);
			dropped.swap(impl->queue);
		}
		for (auto& job : dropped) {
			{
				std::lock_guard lock(job->mutex);
				job->state = Job::State::Cancelled;
				job->started = job->ended = std::chrono::steady_clock::now();
			}
			job->finished.notify_all();
			++impl->stats[job->name].cancelled;
		}
	}

	void AsyncWorker::Cancel(const JobHandle& a_handle)
	{
		ZoneScopedN("CS.DCLF.Worker.CancelAndJoin");
		const auto& job = a_handle.job;
		if (!job)
			return;
		bool dequeued = false;
		{
			std::lock_guard lock(impl->queueMutex);
			if (const auto it = std::find(impl->queue.begin(), impl->queue.end(), job); it != impl->queue.end()) {
				impl->queue.erase(it);
				dequeued = true;
			}
		}
		if (dequeued) {
			{
				std::lock_guard lock(job->mutex);
				job->state = Job::State::Cancelled;
				job->started = job->ended = std::chrono::steady_clock::now();
			}
			job->finished.notify_all();
			++impl->stats[job->name].cancelled;
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		StopAndJoin(*job);
		impl->NoteWait("cancel", job->name, std::chrono::steady_clock::now() - start);
	}

	void AsyncWorker::Drain()
	{
		ZoneScopedN("CS.DCLF.Worker.Drain");
		CancelPending();
		std::shared_ptr<Job> running;
		{
			std::lock_guard lock(impl->queueMutex);
			running = impl->running;
		}
		if (!running)
			return;
		const auto start = std::chrono::steady_clock::now();
		StopAndJoin(*running);
		impl->NoteWait("drain", running->name, std::chrono::steady_clock::now() - start);
	}

	void AsyncWorker::WaitIdle()
	{
		ZoneScopedN("CS.DCLF.Worker.WaitIdle");
		const auto start = std::chrono::steady_clock::now();
		std::unique_lock lock(impl->queueMutex);
		const bool blocked = !impl->queue.empty() || impl->running;
		impl->idle.wait(lock, [&] { return impl->queue.empty() && !impl->running; });
		lock.unlock();
		if (blocked)
			impl->NoteWait("wait idle", "", std::chrono::steady_clock::now() - start);
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
		auto text = fmt::format("[DCLF] render-thread waits over {} frames: {:.2f} blocked/frame, {:.3f} ms/frame{}{}; worker mutexes taken {:.1f}/frame{}\n", d.frames,
			blocked / n, ms / n, sites.empty() ? "" : "; by site: ", sites, d.renderLocks / n, blocked ? "" : " <- OK");
		d.renderWaits.clear();
		d.renderLocks = 0;
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
