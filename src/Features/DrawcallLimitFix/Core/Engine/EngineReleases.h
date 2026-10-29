#pragma once

#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::EngineReleases
{
	/**
	 * @brief Engine references let go of off the render thread, released on it (Release, at Present). Dropping the last
	 * reference to a scene graph object runs the engine's destructors, which belong on its main thread; the scene work hands
	 * over what holds them (a retired batch of the coordinator's chain: tracker events, material references, entry roots: PushOwner).
	 */
	inline EventQueue<RE::NiPointer<RE::NiRefObject>> queue;
	inline EventQueue<std::shared_ptr<void>> owners;

	/**
	 * @brief Which publications' objects may be released (dclf-scene-lifetime-survey.md, section 12). The scene work's engine references
	 * go back through its retirement chain, whose nodes the publications hold (ScenePublication::retirementHold): a batch is handed
	 * here once no publication up to its node lives, and with it the last publication that held the node (NoteReleased). A reader
	 * that holds a publication's node needs nothing more; one that keeps only its data (a cached exclusion's candidates) asks Alive.
	 */
	// The newest publication whose node was handed to the queues (NoteReleased, before the push), and the newest one Release released.
	inline std::atomic<std::uint64_t> pushedThrough{ 0 };
	inline std::atomic<std::uint64_t> releasedThrough{ 0 };

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

	/** @brief The scene work, before it pushes a retired batch: a_publication was the last publication that held its node. */
	inline void NoteReleased(std::uint64_t a_stamp)
	{
		std::uint64_t at = pushedThrough.load(std::memory_order_relaxed);
		while (at < a_stamp && !pushedThrough.compare_exchange_weak(at, a_stamp, std::memory_order_release, std::memory_order_relaxed)) {}
	}

	/**
	 * @brief Whether every object publication a_publication names is alive: no node it held was released. Read inside the engine-read
	 * window (a lease) or on the render thread, where it stays true until the next Present.
	 */
	inline bool Alive(std::uint64_t a_publication) { return releasedThrough.load(std::memory_order_acquire) <= a_publication; }

	/** @brief Render thread, at Present: every owner and reference pushed so far, released. */
	inline void Release()
	{
		// Taken first, then released: every stamp among them was noted before its push, so pushedThrough read after the take covers
		// them all.
		static std::vector<std::shared_ptr<void>> takenOwners;
		static std::vector<RE::NiPointer<RE::NiRefObject>> takenReferences;
		owners.Drain([](std::shared_ptr<void>&& a_owner) { takenOwners.push_back(std::move(a_owner)); });
		queue.Drain([](RE::NiPointer<RE::NiRefObject>&& a_reference) { takenReferences.push_back(std::move(a_reference)); });
		releasedThrough.store(pushedThrough.load(std::memory_order_acquire), std::memory_order_release);
		takenOwners.clear();
		takenReferences.clear();
	}
}
