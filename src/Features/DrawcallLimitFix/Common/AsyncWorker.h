#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace DCLF
{
	/**
	 * @brief The scene lane's host (SceneScheduler::SceneLane): the scene work and the accumulate work, submitted at the frame's
	 * start and joined at Present and the next frame's start. The threading plan's F5 replaces the joins with an intent mailbox
	 * and F6 deletes this class.
	 *
	 * Render thread: SubmitScene, Wait, Drain and the stats. No mutex on its side: a job's state is one atomic, its end a
	 * semaphore.
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

		/** @brief Queues a job on the scene's lane. `a_name` must outlive the worker (a string literal): it keys the stats. */
		JobHandle SubmitScene(const char* a_name, std::function<void(std::stop_token)> a_job);

		/**
		 * @brief Waits for a job, at most `a_budget`. Late leaves the job running; the caller builds inline and
		 * ignores the job's result when it lands.
		 */
		WaitResult Wait(const JobHandle& a_handle, std::chrono::microseconds a_budget);

		/** @brief Drops the queued jobs, then waits (unbounded) for the running one. Teardown, the live toggle, a load screen. */
		void Drain();

		/** @brief The report lines (one per job name) since the last reset, or empty when nothing ran. */
		std::string Report();

		/**
		 * @brief The render thread's waits (Phase D's gate: none): every Wait or Drain that found its job
		 * unfinished and blocked, by site, and the worker mutexes the render thread took. Counted per frame (NoteFrame).
		 */
		void NoteFrame();
		std::string RenderWaitReport();

		void ResetStats();

		~AsyncWorker();

	private:
		AsyncWorker();

		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
