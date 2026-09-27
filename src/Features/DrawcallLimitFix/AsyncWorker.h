#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace DCLF
{
	/** @brief `CS_DCLF_ASYNC`: on (the default: the enabled jobs build on the worker), off (inline), probe (both, compared). */
	enum class AsyncMode : std::uint8_t
	{
		Off,
		On,
		Probe
	};

	/** @brief The `CS_DCLF_ASYNC` setting, read once. */
	AsyncMode AsyncModeSetting();

	/**
	 * @brief Whether a job takes the asynchronous path: the mode is on or probe, and the job is in
	 * `CS_DCLF_ASYNC_JOBS` (unset: every job). Job names: `colour`, `zprepass`, `shadow`, `scene`.
	 */
	bool AsyncJobEnabled(const char* a_job);

	/** @brief `CS_DCLF_ASYNC_WAIT_MS` (default 3): how long a join waits before the render thread builds inline. */
	std::chrono::microseconds AsyncWaitBudget();

	/**
	 * @brief The one worker thread DCLF's per-frame builds run on.
	 *
	 * The jobs of a frame form a serial chain, each kicked at the point of the frame where its inputs are final
	 * and joined at the hook that consumes it, so one thread with a short FIFO gives deterministic latency and a
	 * trivial cancellation story. It is deliberately not ORG's task service (that is what the graph's own frame
	 * preparation fans onto while the render thread waits on it) nor the shader cache's pool (saturated at
	 * startup and at every new cell, exactly when a late join would bring the bubble back).
	 *
	 * A job that is late at its join is not cancelled: the render thread builds inline instead and the worker's
	 * result is dropped by the caller. `Drain` is the only unbounded wait, for teardown and the live toggle.
	 *
	 * Render thread: Submit, Wait, CancelPending, Drain, Idle and the stats. The worker thread only runs jobs.
	 */
	class AsyncWorker
	{
	public:
		enum class WaitResult : std::uint8_t
		{
			Done,
			Late,
			Failed,
			Cancelled,
			None  // no job
		};

		struct Job;

		/** @brief An opaque reference to a submitted job; empty when nothing was submitted. */
		class JobHandle
		{
		public:
			explicit operator bool() const { return job != nullptr; }

		private:
			friend class AsyncWorker;
			std::shared_ptr<Job> job;
		};

		struct JobStats
		{
			std::uint32_t kicked = 0;
			std::uint32_t onTime = 0;
			std::uint32_t late = 0;
			std::uint32_t failed = 0;
			std::uint32_t cancelled = 0;
			double waitTotalMs = 0.0;
			double waitMaxMs = 0.0;
			double buildTotalMs = 0.0;
			double buildMaxMs = 0.0;
			double queuedTotalMs = 0.0;  // submitted to started: time spent behind other jobs
			double windowTotalMs = 0.0;  // submitted to the join: the window the job had to hide in
		};

		static AsyncWorker& Get();

		/** @brief Queues a job. `a_name` must outlive the worker (a string literal): it keys the stats. */
		JobHandle Submit(const char* a_name, std::function<void(std::stop_token)> a_job);

		/**
		 * @brief Waits for a job, at most `a_budget`. Late leaves the job running; the caller builds inline and
		 * ignores the job's result when it lands.
		 */
		WaitResult Wait(const JobHandle& a_handle, std::chrono::microseconds a_budget);

		/** @brief Drops every queued job that has not started; the running one is left to finish. */
		void CancelPending();

		/**
		 * @brief Ends one job: dropped if still queued, otherwise asked to stop and waited for (unbounded), so its
		 * payload is no longer written to when this returns. Other jobs are untouched.
		 */
		void Cancel(const JobHandle& a_handle);

		/** @brief CancelPending, then waits (unbounded) for the running job. Teardown, the live toggle, a load screen. */
		void Drain();

		/** @brief Whether nothing is queued or running. Checked once per frame at Present (a leaked job is a defect). */
		bool Idle() const;

		/**
		 * @brief Waits (unbounded) until nothing is queued or running, cancelling nothing: every queued job runs.
		 * Before the render thread rewrites data a queued job may still read.
		 */
		void WaitIdle();

		/** @brief The report lines (one per job name) since the last reset, or empty when nothing ran. */
		std::string Report();

		void ResetStats();

		~AsyncWorker();

	private:
		AsyncWorker();

		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
