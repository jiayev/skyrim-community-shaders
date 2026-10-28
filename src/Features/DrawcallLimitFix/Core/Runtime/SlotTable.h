#pragma once

#include <cstdint>
#include <deque>
#include <set>
#include <vector>

namespace DCLF
{
	/**
	 * @brief The slots of one of SceneTables' shared tables (geometrySlots, pipelineSlots, materialSlots), whose columns are
	 * parallel vectors the table grows together. Coordinator thread.
	 *
	 * A slot lives while something references it - the objects' records, and for a geometry slot the partition before it
	 * in a skin's chain - counted from the tables' logs (Coordinator::UpdateSlotReferences), and is freed after the batch
	 * in which its last reference went (DrainUnreferenced). A slot can also be freed while referenced (its record
	 * describes nothing); its generation then moves on, and every count taken against the old generation lapses rather than
	 * landing on the slot's next use.
	 *
	 * A freed slot is retiring, not free: a published version of the tables may still name it. The owner takes the
	 * retiring slots (TakeRetiring) into its retirement chain and gives each back (Recycle) once no publication that names it lives.
	 *
	 * Free slots are reused lowest first, so the slots in use stay packed at the bottom and the table's top falls as a scene unloads.
	 * An owner that sizes GPU tables from Size() calls TrimTop after recycling, and cuts its columns to the size it returns: the free
	 * slots past the last one in use (alive or retiring) are dropped. Generations are kept across a trim, so a count taken against a
	 * dropped slot still lapses once the slot is made again.
	 */
	class SlotTable
	{
	public:
		struct Allocation
		{
			std::uint32_t slot = 0;
			bool grown = false;  // a new slot at the end: the caller grows its columns
		};

		/** @brief A free slot, or a new one; unreferenced, so the next drain frees it unless a reference comes. */
		Allocation Allocate()
		{
			Allocation allocation;
			if (!freeList.empty()) {
				allocation.slot = *freeList.begin();
				freeList.erase(freeList.begin());
			} else {
				allocation.slot = static_cast<std::uint32_t>(alive.size());
				allocation.grown = true;
				alive.push_back(0);
				refs.push_back(0);
				if (generation.size() < alive.size())
					generation.push_back(0);
			}
			alive[allocation.slot] = 1;
			refs[allocation.slot] = 0;
			++aliveCount;
			BecomeIdle(allocation.slot);
			return allocation;
		}
		/** @brief Frees a slot whatever references it has: the counts taken against it lapse. */
		void Free(std::uint32_t a_slot)
		{
			if (!Alive(a_slot))
				return;
			alive[a_slot] = 0;
			if (refs[a_slot])
				--referencedCount;
			refs[a_slot] = 0;
			++generation[a_slot];
			--aliveCount;
			retiring.push_back({ a_slot, generation[a_slot] });
		}
		/** @brief The slots freed since the last call, with their generations then (for Recycle). */
		template <class F>
		void TakeRetiring(F&& a_take)
		{
			for (const auto& entry : retiring)
				a_take(entry.slot, entry.generation);
			retiring.clear();
		}
		/** @brief A retired slot back on the free list: no publication names it any more (unless it was reused or cleared since). */
		void Recycle(std::uint32_t a_slot, std::uint32_t a_generation)
		{
			if (a_slot < alive.size() && !alive[a_slot] && generation[a_slot] == a_generation)
				freeList.insert(a_slot);
		}
		/**
		 * @brief Drops the free slots past the last one in use (alive or retiring: a publication may name it): the new Size(), to
		 * which the owner cuts its columns. Retiring slots are never past it.
		 */
		std::size_t TrimTop()
		{
			while (!alive.empty() && !freeList.empty() && *freeList.rbegin() == alive.size() - 1) {
				freeList.erase(std::prev(freeList.end()));
				alive.pop_back();
				refs.pop_back();
			}
			return alive.size();
		}
		std::size_t FreeCount() const { return freeList.size(); }
		std::size_t Retiring() const { return retiring.size(); }
		bool Alive(std::size_t a_slot) const { return a_slot < alive.size() && alive[a_slot]; }
		std::uint32_t Generation(std::size_t a_slot) const { return a_slot < generation.size() ? generation[a_slot] : 0; }
		/** @brief A reference to the slot as of a_generation (none when the slot has moved on since). */
		void AddRef(std::uint32_t a_slot, std::uint32_t a_generation)
		{
			if (!Alive(a_slot) || generation[a_slot] != a_generation)
				return;
			if (refs[a_slot]++ == 0)
				++referencedCount;
		}
		void Release(std::uint32_t a_slot, std::uint32_t a_generation)
		{
			if (!Alive(a_slot) || generation[a_slot] != a_generation || refs[a_slot] == 0)
				return;
			if (--refs[a_slot] == 0) {
				--referencedCount;
				BecomeIdle(a_slot);
			}
		}
		/** @brief Every count to zero, before a recount: each live slot is unreferenced until the recount says otherwise. */
		void ResetReferences()
		{
			idle.clear();
			referencedCount = 0;
			for (std::uint32_t slot = 0; slot < alive.size(); ++slot) {
				refs[slot] = 0;
				if (alive[slot])
					BecomeIdle(slot);
			}
		}
		/**
		 * @brief After a complete batch of reference changes: frees each slot whose last reference went (a_onFree first). A
		 * reference reacquired in the batch cancels the free; a reused slot's stale events are skipped by generation.
		 */
		template <class F>
		std::uint32_t DrainUnreferenced(F&& a_onFree)
		{
			std::uint32_t freed = 0;
			while (!idle.empty()) {
				const auto entry = idle.front();
				idle.pop_front();
				if (!Alive(entry.slot) || generation[entry.slot] != entry.generation || refs[entry.slot])
					continue;
				a_onFree(entry.slot);
				Free(entry.slot);
				++freed;
			}
			return freed;
		}
		std::size_t Size() const { return alive.size(); }
		std::size_t AliveCount() const { return aliveCount; }
		std::size_t ReferencedCount() const { return referencedCount; }
		std::uint32_t References(std::size_t a_slot) const { return a_slot < refs.size() ? refs[a_slot] : 0; }
		void Clear() { *this = {}; }

	private:
		void BecomeIdle(std::uint32_t a_slot) { idle.push_back({ a_slot, generation[a_slot] }); }

		struct Idle
		{
			std::uint32_t slot = 0, generation = 0;
		};
		std::vector<std::uint8_t> alive;
		std::vector<std::uint32_t> refs, generation;  // generation: at least alive's size (kept across TrimTop)
		std::set<std::uint32_t> freeList;            // lowest first
		std::vector<Idle> retiring;  // freed, not on the free list yet (TakeRetiring)
		std::deque<Idle> idle;  // last-reference events, in order
		std::size_t aliveCount = 0, referencedCount = 0;
	};
}
