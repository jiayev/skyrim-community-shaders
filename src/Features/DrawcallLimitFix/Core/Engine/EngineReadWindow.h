#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace DCLF
{
	/**
	 * @brief When a worker may touch the engine's scene graph (dclf-architecture.md, "The engine-read window").
	 *
	 * From Main::Draw to Present the engine reads its scene graph but does not write it (the writes that do happen there:
	 * currentFade, billboards, texture transforms). The window is open over exactly that span: Open at FrameDriver::BeginFrame, Close at Present, both on the render thread. A worker touching engine memory takes a Lease per item, or per bounded chunk of items (a parallel loop's grain), and
	 * does the item only while it holds one; a refused lease means the window has closed, and the worker leaves the rest, by index, for
	 * the next window.
	 *
	 * Close waits for the leases in flight - an item or a chunk each, never a task - so the update that follows Present never runs beside a worker
	 * inside engine memory. Lock-free: one atomic word holds the open bit and the lease count, so a lease can never be taken after Close
	 * has seen the count.
	 *
	 * The scene work is not joined at Present, so it may run beside the engine's update; it reads the mirror alone, and its observers'
	 * live reads each take a lease. The engine's read paths (SceneCapture's captures, the live helpers) Touch the window: on a scene work
	 * thread without a lease, the access is a defect, counted and named (the report's LANE ENGINE ACCESS).
	 */
	class EngineReadWindow
	{
	public:
		/** @brief Render thread, Main::Draw (FrameDriver::BeginFrame): the engine's update is done; workers may read. */
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

		/** @brief What the report reads since the last Take: closes, closes that found leases in flight and how long they waited, refused leases, the scene work's accesses without a lease. */
		struct Counters
		{
			std::uint64_t closes = 0, closesWaited = 0, refused = 0, unleased = 0;
			double waitedUs = 0.0, waitedMaxUs = 0.0;
			const char* unleasedFirst = nullptr;
			bool any = false;  // a close or an unleased access since the last Take (else nothing else was taken)
		};
		/** @brief Render thread: the counters since the last Take, reset (Reports::EngineReadWindowLine). */
		static Counters Take();

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
}

// The release guard is a diagnostic (Diagnostics/Engine/ReleaseGuard.h): included here until its callers include it.
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ReleaseGuard.h"
