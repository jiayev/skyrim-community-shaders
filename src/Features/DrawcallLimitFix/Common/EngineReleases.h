#pragma once

#include "Features/DrawcallLimitFix/Common/EventQueue.h"

namespace DCLF::EngineReleases
{
	/**
	 * @brief Engine references let go of off the render thread, released on it (Release, at Present). Dropping the last
	 * reference to a scene graph object runs the engine's destructors, which belong on its main thread; an immutable
	 * publication that holds references (SunCandidates) may lose its last owner on any thread, so it hands them over here.
	 */
	inline EventQueue<RE::NiPointer<RE::NiRefObject>> queue;

	/** @brief Any thread. */
	inline void Push(RE::NiPointer<RE::NiRefObject>&& a_reference)
	{
		if (a_reference)
			queue.Push(std::move(a_reference));
	}

	/** @brief Render thread, at Present: every reference pushed so far, released. */
	inline void Release()
	{
		queue.Drain([](RE::NiPointer<RE::NiRefObject>&& a_reference) { RE::NiPointer<RE::NiRefObject> released = std::move(a_reference); });
	}
}
