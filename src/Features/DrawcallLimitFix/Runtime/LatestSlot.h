#pragma once

#include <atomic>
#include <memory>

namespace DCLF
{
	/**
	 * @brief A latest-wins mailbox between threads: Post replaces what is there (freeing a value nobody took), Take empties it.
	 * Lock-free (one pointer exchange each way; MSVC's std::atomic<std::shared_ptr> is not); a value is owned by exactly one side at a
	 * time, so nothing is read after it is freed. A value nobody took is freed by the next Post or by the slot's end, on that thread:
	 * a value whose drop must happen on a given thread (an engine reference) is not posted here bare.
	 *
	 * Users: the pipeline lane's frame inputs (render thread -> lane) and its catalogs (lane -> the scene lane); the scene publication
	 * (the coordinator -> the frame's start), the frame inputs and the ahead context (the frame's start -> the coordinator).
	 */
	template <class T>
	struct LatestSlot
	{
		std::atomic<T*> slot{ nullptr };

		LatestSlot() = default;
		LatestSlot(const LatestSlot&) = delete;
		LatestSlot& operator=(const LatestSlot&) = delete;
		~LatestSlot() { delete slot.exchange(nullptr, std::memory_order_acquire); }

		void Post(std::unique_ptr<T> a_value) { delete slot.exchange(a_value.release(), std::memory_order_acq_rel); }
		std::unique_ptr<T> Take() { return std::unique_ptr<T>(slot.exchange(nullptr, std::memory_order_acq_rel)); }
		/** @brief Takes the newest into a_held when there is one (the taker's copy: kept while nothing newer is posted). */
		bool TakeInto(T& a_held)
		{
			if (auto taken = Take()) {
				a_held = std::move(*taken);
				return true;
			}
			return false;
		}
	};
}
