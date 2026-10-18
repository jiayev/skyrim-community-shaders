#pragma once

#include "Features/DrawcallLimitFix/Core/Runtime/SceneExecutor.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneGraph.h"

#include <functional>
#include <string_view>

namespace org::async
{
	class SerializedTaskPump;
}

namespace DCLF
{
	/**
	 * @brief DCLF's threads (dclf-architecture.md, "The design"): one executor for the whole feature.
	 *
	 * - The coordinator: one thread, its lane serialized. The frame's jobs run there in the order they are submitted. Above normal
	 *   priority unless CS_DCLF_ASYNC_PRIORITY=normal.
	 * - The scene lane: one thread, serialized, for the scene pump's passes (SceneLaneScope); above normal priority as the coordinator.
	 * - The preparation pool: hardware_concurrency - 2 threads (at least one), below normal priority, beside the engine's own
	 *   job threads; CS_DCLF_WORKERS sets the count. A worker running a ParallelFor chunk for a higher-priority caller runs it at the
	 *   caller's priority, restored afterwards (CS_DCLF_ASYNC_PRIORITY=noboost: never).
	 *
	 * Started on first use and never torn down: the process ends it.
	 */
	class SceneScheduler
	{
	public:
		static SceneExecutor& Executor();
		/**
		 * @brief The scene lane's scope: Executor()'s scene thread (SceneExecutor::Scene), serialized, for the scene work alone (the
		 * scene pump's passes, woken by their producers). The frame's builds keep the coordinator's thread, so a pass that spans the frame
		 * never holds one up. Its parallel loops run on the preparation pool.
		 */
		static const org::async::Scope& SceneLaneScope();
		/** @brief The scope every feature job runs under: never cancelled (a job's own stop source cancels it). */
		static const org::async::Scope& Scope();
		static unsigned PreparationWorkers();
		/** @brief DCLF's state graph on this executor (Runtime::SceneGraph). */
		static Runtime::SceneGraph& Graph();
		/**
		 * @brief A lane: a SerializedTaskPump (lock-free, level-triggered: wakes coalesce) whose passes run a_drain one at a time, each
		 * dispatched to a_executor as a_class / a_dispatch under a_scope (which outlives the pump), named a_name; a_refused when the
		 * executor refuses a pass. Never destroyed: a completion may still wake it while the process tears down.
		 */
		static org::async::SerializedTaskPump* MakeLanePump(SceneExecutor& a_executor, const org::async::Scope& a_scope, org::async::TaskClass a_class,
			org::async::TaskDispatch a_dispatch, std::string_view a_name, std::function<void()> a_drain, std::function<void()> a_refused);
	};
}
