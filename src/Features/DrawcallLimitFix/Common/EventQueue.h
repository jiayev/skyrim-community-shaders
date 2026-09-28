#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace DCLF
{
	/**
	 * @brief The events the engine's hooks push, from any thread, for one consumer to drain: DCLF's scene and material
	 * events (SceneStore's fade, property, node, LOD fade and switch events; MaterialSources' writes).
	 *
	 * A bounded ring with a sequence per cell (Vyukov's), so a push allocates nothing while the ring has room. When it is
	 * full a push spills onto a lock-free stack instead, which allocates, so no event is ever lost: the consumers have no
	 * path that rebuilds what a lost event would have told them. A drain visits the ring's events in push order, then the
	 * spilled ones in push order.
	 */
	template <class T, std::size_t Capacity = 4096>
	class EventQueue
	{
		static_assert((Capacity & (Capacity - 1)) == 0, "a power of two");

	public:
		EventQueue()
		{
			for (std::size_t i = 0; i < Capacity; ++i)
				cells[i].sequence.store(i, std::memory_order_relaxed);
		}
		// The queues live as long as the process, and what they hold at its end (engine objects' references among them) is
		// left alone: nothing may release engine objects while the process is torn down.
		~EventQueue() = default;
		EventQueue(const EventQueue&) = delete;
		EventQueue& operator=(const EventQueue&) = delete;

		/** @brief Any thread. */
		void Push(T a_value)
		{
			std::uint64_t position = head.load(std::memory_order_relaxed);
			for (;;) {
				auto& cell = cells[position & (Capacity - 1)];
				const std::uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
				const auto difference = static_cast<std::int64_t>(sequence) - static_cast<std::int64_t>(position);
				if (difference == 0) {
					if (head.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
						cell.value = std::move(a_value);
						cell.sequence.store(position + 1, std::memory_order_release);
						return;
					}
				} else if (difference < 0) {
					// Full: spilled, never dropped.
					auto* node = new Spill{ std::move(a_value), spill.load(std::memory_order_relaxed) };
					while (!spill.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {
					}
					spilled.fetch_add(1, std::memory_order_relaxed);
					return;
				} else {
					position = head.load(std::memory_order_relaxed);
				}
			}
		}

		/** @brief The one consumer: every event pushed before the call, oldest first (a_visit(T&&)). Returns how many. */
		template <class F>
		std::size_t Drain(F&& a_visit)
		{
			std::size_t count = 0;
			for (;;) {
				auto& cell = cells[tail & (Capacity - 1)];
				if (cell.sequence.load(std::memory_order_acquire) != tail + 1)
					break;
				a_visit(std::move(cell.value));
				cell.value = T{};
				cell.sequence.store(tail + Capacity, std::memory_order_release);
				++tail;
				++count;
			}
			// The spilled events, reversed into push order.
			Spill* reversed = nullptr;
			for (auto* node = spill.exchange(nullptr, std::memory_order_acquire); node;) {
				auto* next = node->next;
				node->next = reversed;
				reversed = node;
				node = next;
			}
			for (auto* node = reversed; node;) {
				a_visit(std::move(node->value));
				auto* next = node->next;
				delete node;
				node = next;
				++count;
			}
			return count;
		}
		/** @brief Drops every event pushed before the call. */
		void Discard() { Drain([](T&&) {}); }
		/** @brief The pushes that found the ring full, since the start. */
		std::uint64_t Spilled() const { return spilled.load(std::memory_order_relaxed); }

	private:
		struct Cell
		{
			std::atomic<std::uint64_t> sequence{ 0 };
			T value{};
		};
		struct Spill
		{
			T value;
			Spill* next;
		};
		Cell* cells = new Cell[Capacity];  // never freed (above)
		std::atomic<std::uint64_t> head{ 0 };
		std::uint64_t tail = 0;  // the consumer's
		std::atomic<Spill*> spill{ nullptr };
		std::atomic<std::uint64_t> spilled{ 0 };
	};
}
