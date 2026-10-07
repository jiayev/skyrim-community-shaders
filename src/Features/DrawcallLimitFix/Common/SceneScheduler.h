#pragma once

#include "Features/DrawcallLimitFix/Published/PublishedSceneExecutor.h"
#include "Features/DrawcallLimitFix/Published/SceneGraph.h"

namespace DCLF
{
	/**
	 * @brief DCLF's threads (dclf-async-publication.md, "The design"): one executor for the whole feature.
	 *
	 * - The coordinator: one thread, its lane serialized. The frame's jobs run there in the order they are submitted
	 *   (AsyncWorker), and the scene's own state will be owned there. Above normal priority unless
	 *   CS_DCLF_ASYNC_PRIORITY=normal, as the one worker was.
	 * - The preparation pool: hardware_concurrency - 2 threads (at least one), below normal priority, beside the engine's own
	 *   job threads; CS_DCLF_WORKERS sets the count.
	 *
	 * Started on first use and never torn down: the process ends it.
	 */
	class SceneScheduler
	{
	public:
		static PublishedSceneExecutor& Executor();
		/**
		 * @brief The scene's lane (step 6c): one thread, serialized, for the scene work alone (the walk and the accumulate phase,
		 * SceneStore::KickSceneTask). The frame's builds keep the coordinator's lane, so a scene task that spans the frame never
		 * holds one up. Its parallel loops run on Executor()'s preparation pool.
		 */
		static PublishedSceneExecutor& SceneLane();
		static const org::async::Scope& SceneLaneScope();
		/** @brief The scope every feature job runs under: never cancelled (a job's own stop source cancels it). */
		static const org::async::Scope& Scope();
		static unsigned PreparationWorkers();
		/** @brief DCLF's AsyncStateGraph on this executor (Published::SceneGraph); its producers come with the phases that need them. */
		static Published::SceneGraph& Graph();
	};
}
