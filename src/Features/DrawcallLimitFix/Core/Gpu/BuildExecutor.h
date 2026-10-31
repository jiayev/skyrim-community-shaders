#pragma once

// The executors DCLF's pipeline work runs on: the width-limited build pump (shader compiles and pipeline builds, on the preparation
// pool) and the pipeline lane's serialized drain (on the coordinator executor). Callable from any thread.

#include <functional>

namespace DCLF
{
	/**
	 * @brief The executor DCLF's shader compiles and pipeline builds run on (ORGModuleServices' ShaderCompiler::Configure and
	 * PipelineService::Configure): an org::services::ServiceSubmit, spelled out so this header needs no ORG include.
	 *
	 * Tasks queue without bound (lock-free, never refused) and run on DCLF's preparation pool (SceneScheduler::Executor(),
	 * SceneExecutor::Preparation), at most BuildWidth() at once: a compile may block in DXC for seconds, and the
	 * pool's other workers stay free for the frame's jobs and the scene walk's parallel loops. A runner takes one task per
	 * dispatch and then goes to the back of the pool's queue, so jobs queued meanwhile run between builds.
	 *
	 * The tasks are not waited for anywhere (no scope wait, no drain): a queued build runs to completion whatever happens to
	 * the frame (the toggle, a load screen), and its completion is admitted, or ignored by its generation, by the next Update.
	 */
	std::function<bool(std::function<void()>)> BuildSubmitter();

	/** @brief How many builds run at once: half the preparation workers, at least one. */
	unsigned BuildWidth();

	/**
	 * @brief The pipeline lane: the coordinator of the Lighting programs and pipelines (DrawPipelinesImpl.h, Impl::Lane; DrawPipelines.cpp drains it).
	 * One drain pass at a time on DCLF's coordinator (SceneScheduler::Executor(), SceneExecutor::Coordinator), scheduled by
	 * WakePipelineLane, which any thread calls after it posts something the lane takes: a request, a compile's or a build's
	 * completion, the frame's inputs. Wakes coalesce (org::async::SerializedTaskPump: lock-free, level-triggered), and a wake
	 * that arrives while a pass runs makes another pass. Nothing waits for the lane: its results are published (PipelineCatalog).
	 *
	 * SetPipelineLaneDrain names the pass once (DrawPipelines, at its construction); a wake before that runs an empty pass, and
	 * what it was for waits in its queue for the next one.
	 */
	void SetPipelineLaneDrain(void (*a_drain)());
	void WakePipelineLane();
}
