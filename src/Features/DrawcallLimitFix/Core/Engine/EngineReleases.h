#pragma once

#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

#include <memory>

namespace DCLF::EngineReleases
{
	/**
	 * @brief Engine references let go of off the render thread, released on it (Release, at Present). Dropping the last
	 * reference to a scene graph object runs the engine's destructors, which belong on its main thread; an immutable
	 * publication that holds references (SunCandidates) may lose its last owner on any thread, so it hands them over here, as
	 * the scene work does with what holds them (an applied event batch, a material reference: PushOwner).
	 */
	inline EventQueue<RE::NiPointer<RE::NiRefObject>> queue;
	inline EventQueue<std::shared_ptr<void>> owners;

	/** @brief Any thread. */
	inline void Push(RE::NiPointer<RE::NiRefObject>&& a_reference)
	{
		if (a_reference)
			queue.Push(std::move(a_reference));
	}

	/** @brief Any thread: an owner whose last release can run the engine's destructors. */
	inline void PushOwner(std::shared_ptr<void>&& a_owner)
	{
		if (a_owner)
			owners.Push(std::move(a_owner));
	}

	/** @brief Render thread, at Present: every owner and reference pushed so far, released. */
	inline void Release()
	{
		owners.Drain([](std::shared_ptr<void>&& a_owner) { const auto released = std::move(a_owner); });
		queue.Drain([](RE::NiPointer<RE::NiRefObject>&& a_reference) { RE::NiPointer<RE::NiRefObject> released = std::move(a_reference); });
	}
}
