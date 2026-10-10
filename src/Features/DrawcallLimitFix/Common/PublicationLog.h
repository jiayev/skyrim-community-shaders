#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace DCLF
{
	/**
	 * @brief A forward-linked log from one producer to one consumer (T6b3a): the producer appends a node and sets the last one's next
	 * to it; the consumer walks from where it stopped, as far as next is set, and frees what it has walked past. Lock-free, never
	 * blocks, never loses a node: what a latest-wins mailbox skips (a publication the frame's start never took) still reaches the
	 * consumer here. The producer never reads a node after setting its next; the consumer never frees the node whose next is unset
	 * (the producer's last), so neither touches a node the other may be writing.
	 *
	 * The consumer holds the log's first node (a stub, consumed) and any number of cursors into it, each the last node it has taken;
	 * Release frees the nodes before the cursor that lags (the caller names it).
	 */
	template <class T>
	class PublicationLog
	{
	public:
		struct Node
		{
			T value{};
			std::uint64_t sequence = 0;  // 1 for the first appended; the stub's 0
			std::atomic<Node*> next{ nullptr };
		};

		PublicationLog() :
			stub(new Node{}), tail(stub) {}
		PublicationLog(const PublicationLog&) = delete;
		PublicationLog& operator=(const PublicationLog&) = delete;
		~PublicationLog()
		{
			for (Node* node = stub; node;) {
				Node* next = node->next.load(std::memory_order_acquire);
				delete node;
				node = next;
			}
		}

		/** @brief The producer: a_value appended after the last node. Returns its sequence. */
		std::uint64_t Append(T&& a_value)
		{
			auto* node = new Node{};
			node->value = std::move(a_value);
			node->sequence = ++appended;
			tail->next.store(node, std::memory_order_release);
			tail = node;
			return node->sequence;
		}
		/** @brief The producer: the sequence of the last node appended (0: none yet). */
		std::uint64_t Appended() const { return appended; }

		/** @brief The consumer: the first node held (consumed: a cursor starts here). */
		Node* First() const { return stub; }
		/** @brief The consumer: the node after a_node, null while the producer has appended none after it. */
		static Node* Next(Node* a_node) { return a_node->next.load(std::memory_order_acquire); }
		/**
		 * @brief The consumer: frees every node before a_lagging (the cursor furthest behind: every other cursor is at it or past it),
		 * which becomes the first node held.
		 */
		void Release(Node* a_lagging)
		{
			while (stub != a_lagging) {
				Node* next = stub->next.load(std::memory_order_acquire);
				if (!next)
					return;  // a_lagging is not in the log (a defect): nothing freed past the producer's last
				delete stub;
				stub = next;
			}
		}

	private:
		Node* stub;               // the consumer's
		Node* tail;               // the producer's
		std::uint64_t appended = 0;  // the producer's
	};
}
