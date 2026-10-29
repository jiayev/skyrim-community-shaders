#pragma once

#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::EngineReleases
{
	/**
	 * @brief Engine references let go of off the render thread, released on it (Release, at Present). Dropping the last
	 * reference to a scene graph object runs the engine's destructors, which belong on its main thread; the scene work hands
	 * over what holds them (a released batch of the ReleaseRing: tracker events, material references, entry roots: PushOwner).
	 */
	inline EventQueue<RE::NiPointer<RE::NiRefObject>> queue;
	inline EventQueue<std::shared_ptr<void>> owners;

	/**
	 * @brief The release horizon (dclf-scene-lifetime-survey.md, section 12). What the scene work lets go of is stamped with the first
	 * publication that no longer names it (ReleaseRing), and released once every frame of the last kReleaseFrames installed that one
	 * or a later one: a frame's reader of a publication up to kReleaseFrames old (a readback's writes, the cut's plans) finds its
	 * objects alive without holding them. An older reader asks Alive first.
	 */
	// A visibility readback's writes arrive up to a dozen frames after their arm under DXVK (the report's write age): the horizon covers
	// that with a margin, at the cost of a fraction of a second of what the scene let go of.
	inline constexpr std::size_t kReleaseFrames = 32;
	// Render thread: the installed publication at each of the last kReleaseFrames Presents.
	inline std::array<std::uint64_t, kReleaseFrames> installedAt{};
	inline std::size_t installedNext = 0;
	// What the ReleaseRing may release: stamps at or below it (the oldest of installedAt).
	inline std::atomic<std::uint64_t> releasable{ 0 };
	// The newest stamp handed to the queues (NoteReleased, before the push), and the newest one Release released: a publication from
	// it on names no freed object (a static scene's publications name what no stamp released, however far the horizon moved).
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

	/** @brief Render thread, at Present before Release: the publication the frame installed (0: none yet). */
	inline void NoteInstalled(std::uint64_t a_publication)
	{
		installedAt[installedNext] = a_publication;
		installedNext = (installedNext + 1) % kReleaseFrames;
		std::uint64_t oldest = a_publication;
		for (const auto at : installedAt)
			oldest = std::min(oldest, at);
		releasable.store(oldest, std::memory_order_release);
	}

	/** @brief The scene work: stamps at or below this are named by no frame of the horizon (ReleaseRing::Drain). */
	inline std::uint64_t Releasable() { return releasable.load(std::memory_order_acquire); }

	/** @brief The scene work, before it pushes what was stamped a_stamp (ReleaseRing::Drain). */
	inline void NoteReleased(std::uint64_t a_stamp)
	{
		std::uint64_t at = pushedThrough.load(std::memory_order_relaxed);
		while (at < a_stamp && !pushedThrough.compare_exchange_weak(at, a_stamp, std::memory_order_release, std::memory_order_relaxed)) {}
	}

	/**
	 * @brief Whether every object publication a_publication names is alive: no stamp past it was released. Read inside the engine-read
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
