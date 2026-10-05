#pragma once

#include <atomic>
#include <cstdint>
#include <string>

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
			Lease(const Lease&) = delete;
			Lease& operator=(const Lease&) = delete;
			explicit operator bool() const { return held; }

		private:
			bool held = false;
		};

		/** @brief Since the last report: closes, closes that found leases in flight and how long they waited, refused leases. */
		static std::string Report();

	private:
		static constexpr std::uint32_t kOpen = 1u << 31;
		static inline std::atomic<std::uint32_t> state{ 0 };
		static inline std::atomic<std::uint64_t> refused{ 0 };
		// Render thread.
		static inline std::uint64_t closes = 0, closesWaited = 0;
		static inline double waitedUs = 0.0, waitedMaxUs = 0.0;
	};
}
