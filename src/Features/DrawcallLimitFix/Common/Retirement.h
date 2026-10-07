#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace DCLF
{
	/**
	 * @brief Deferred reclamation by ownership (BasicRenderer's rule: nothing is logically freed while a published version that may
	 * name it lives). One owner thread retires things into the open node; each publication it makes holds the node opened with it,
	 * which collects what is retired after it (named by that publication and the ones before). A node holds the next newer one, so
	 * a node lives while any publication up to it does. When the last of them goes - on whatever thread drops it - the node's batch
	 * is returned, and the owner takes it back (Drain) to put the slots on its free lists and hand the references on.
	 *
	 * Batch: movable, with Empty().
	 */
	template <class Batch>
	class RetirementChain
	{
		struct Returned
		{
			Batch batch;
			Returned* next = nullptr;
		};
		struct Node
		{
			explicit Node(RetirementChain* a_chain) :
				chain(a_chain) {}
			Node(const Node&) = delete;
			Node& operator=(const Node&) = delete;
			~Node()
			{
				if (!batch.Empty())
					chain->Return(std::move(batch));
				// The newer nodes go with this one when nothing else holds them; iteratively, not by recursion through the chain.
				auto newer = std::move(next);
				while (newer && newer.use_count() == 1) {
					auto after = std::move(newer->next);
					newer.reset();
					newer = std::move(after);
				}
			}
			RetirementChain* chain;
			Batch batch;
			std::shared_ptr<Node> next;
		};

	public:
		RetirementChain() :
			open(std::make_shared<Node>(this)) {}
		RetirementChain(const RetirementChain&) = delete;
		RetirementChain& operator=(const RetirementChain&) = delete;
		~RetirementChain()
		{
			open.reset();
			Drain([](Batch&&) {});
		}

		/** @brief Owner thread: where what is retired now goes (named by the publications made so far). */
		Batch& Open() { return open->batch; }
		/** @brief Owner thread, with a publication made now: what it holds (the node collecting what is retired after it). */
		std::shared_ptr<const void> Publish()
		{
			auto node = std::make_shared<Node>(this);
			open->next = node;
			open = std::move(node);
			return open;
		}
		/** @brief Owner thread: the batches returned since, oldest first: a_take(batch). Returns how many. */
		template <class F>
		std::size_t Drain(F&& a_take)
		{
			Returned* list = returned.exchange(nullptr, std::memory_order_acquire);
			Returned* ordered = nullptr;
			while (list) {
				auto* next = list->next;
				list->next = ordered;
				ordered = list;
				list = next;
			}
			std::size_t count = 0;
			while (ordered) {
				auto* next = ordered->next;
				a_take(std::move(ordered->batch));
				delete ordered;
				ordered = next;
				++count;
			}
			return count;
		}

	private:
		void Return(Batch&& a_batch)
		{
			auto* node = new Returned{ std::move(a_batch) };
			node->next = returned.load(std::memory_order_relaxed);
			while (!returned.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {}
		}

		std::shared_ptr<Node> open;
		std::atomic<Returned*> returned{ nullptr };
	};
}
