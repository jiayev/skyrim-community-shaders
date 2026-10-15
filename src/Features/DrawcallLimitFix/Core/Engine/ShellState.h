#pragma once

// The shell's state that engine hooks outside it read (a feature's own thunks): whether DCLF runs this frame, and where the engine's passes
// are. The shell installs the query and writes the passes; the rest only read them.

namespace DCLF::ShellState
{
	namespace Detail
	{
		inline bool (*runningQuery)() = nullptr;
	}

	/**
	 * @brief Main thread, at PostPostLoad, before Extensions::InstallEarlyHooks (the shell): how Running asks the shell whether DCLF runs
	 * (installed and switched on). Once.
	 */
	inline void InstallRunningQuery(bool (*a_running)()) { Detail::runningQuery = a_running; }
	/** @brief Any hook: whether DCLF runs (the shell's Running); false before the shell installs its query. */
	inline bool Running() { return Detail::runningQuery && Detail::runningQuery(); }

	/** @brief Where the engine's passes are, render thread only: written by the shell's Main::RenderDepth thunk around the call. */
	struct EnginePasses
	{
		bool inDepthPass = false;
		bool depthControlDriven = false;  // RenderDepth's second argument (the first-person depth range's choice)
	};
	/** @brief Render thread only: the engine's passes as the shell's thunks last wrote them. */
	inline EnginePasses& Passes()
	{
		static EnginePasses passes;
		return passes;
	}
}
