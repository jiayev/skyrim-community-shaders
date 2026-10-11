#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace DCLF
{
	/**
	 * @brief When a worker may touch the engine's scene graph (dclf-async-publication.md, "The engine-read window").
	 *
	 * From Main::Draw to Present the engine reads its scene graph but does not write it (BeginSceneFrame lists the writes
	 * that do happen: currentFade, billboards, texture transforms). The window is open over exactly that span: Open at
	 * BeginSceneFrame, Close at Present, both on the render thread. A worker reading or writing engine memory takes a Lease
	 * per item and does the item only while it holds one; a refused lease means the window has closed, and the worker
	 * leaves the rest for whoever resumes it (by index, as RunPlacements and the fade write-back do).
	 *
	 * Close waits for the leases in flight - an item each, never a task - so the update that follows Present never runs
	 * beside a worker inside engine memory. Lock-free: one atomic word holds the open bit and the lease count, so a lease
	 * can never be taken after Close has seen the count.
	 *
	 * T6b1d: the scene work is not joined at Present, so it may run beside the engine's update; it reads the mirror alone, and its
	 * parity observers' live reads each take a lease. The engine's read paths (SceneCapture's captures, the live helpers) Touch the
	 * window: on a scene work thread without a lease, the access is a defect, counted and named (the report's LANE ENGINE ACCESS).
	 */
	class EngineReadWindow
	{
	public:
		/** @brief Render thread, Main::Draw (BeginSceneFrame): the engine's update is done; workers may read. */
		static void Open();
		/** @brief Render thread, Present: no new lease from here; returns once the leases in flight are released. */
		static void Close();
		static bool IsOpen() { return (state.load(std::memory_order_acquire) & kOpen) != 0; }

		/** @brief One item's access to engine memory. False: the window is closed and the item must not be done. */
		class Lease
		{
		public:
			Lease();
			~Lease();
			Lease(Lease&& a_other) noexcept :
				held(std::exchange(a_other.held, false)) {}
			Lease(const Lease&) = delete;
			Lease& operator=(const Lease&) = delete;
			Lease& operator=(Lease&&) = delete;
			explicit operator bool() const { return held; }

		private:
			bool held = false;
		};

		/** @brief A thread running DCLF's scene work (the lane, or a parallel loop's chunk of it): set by the scene work. */
		static inline thread_local bool sceneWork = false;
		/** @brief An access to engine memory (a_site: a static name): counted when a scene work thread holds no lease. */
		static void Touch(const char* a_site)
		{
			if (sceneWork && !heldHere)
				NoteUnleased(a_site);
		}

		/** @brief Since the last report: closes, closes that found leases in flight and how long they waited, refused leases. */
		static std::string Report();

	private:
		static void NoteUnleased(const char* a_site);

		static constexpr std::uint32_t kOpen = 1u << 31;
		static inline thread_local std::uint32_t heldHere = 0;  // the leases this thread holds
		static inline std::atomic<std::uint64_t> unleased{ 0 };
		static inline std::atomic<const char*> unleasedFirst{ nullptr };
		static inline std::atomic<std::uint32_t> state{ 0 };
		static inline std::atomic<std::uint64_t> refused{ 0 };
		// Render thread.
		static inline std::uint64_t closes = 0, closesWaited = 0;
		static inline double waitedUs = 0.0, waitedMaxUs = 0.0;
	};

	/**
	 * @brief T6b3e, CS_DCLF_RELEASE_GUARD=1: an observer of the engine's last releases on DCLF's own threads. Dropping an engine
	 * object's last reference runs its destructors, which belong on the engine's threads (a node's takes its collision object out of the
	 * Havok world); the scene lane and the pool hand their references back instead (the retirement chain, EngineReleases,
	 * batchesReleased). With the switch, NiRefObject::DeleteThis (AE 0x140d27520: the refcount-zero path of every class that keeps the
	 * base's) is replaced by the same call with a count: a release on the scene lane (EngineReadWindow::sceneWork) or another thread of
	 * DCLF's executor (MarkThread) is counted with the object's class name (its RTTI) and reported (LANE ENGINE RELEASE). Counted only.
	 */
	class ReleaseGuard
	{
	public:
		/** @brief Render thread, at install: patches DeleteThis under the switch, once, its bytes checked first. */
		static void Install();
		/** @brief On each of DCLF's executor threads, at its start (SceneScheduler): a_kind 1 the scene lane, 2 the coordinator or a pool worker. */
		static void MarkThread(std::uint8_t a_kind) { dclfThread = a_kind; }
		/** @brief Since the last report, or empty while not installed. */
		static std::string Report();

	private:
		static void DeleteThis(void* a_object);
		static void Note(void* a_object);

		static inline thread_local std::uint8_t dclfThread = 0;
		static inline bool installed = false;
		static inline std::atomic<std::uint64_t> laneReleases{ 0 }, poolReleases{ 0 }, unnamed{ 0 };
		static inline std::atomic<const char*> first{ nullptr }, last{ nullptr };
	};
}
