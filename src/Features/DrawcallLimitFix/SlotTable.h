#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace DCLF
{
	/**
	 * @brief The slots of one of SceneStore's shared tables (geometries, pipelines, materials), whose columns are parallel
	 * vectors the table grows together (Tables::GeometryColumns, PipelineColumns, MaterialColumns).
	 *
	 * A slot lives while something references it - the objects' records, and for a geometry slot the partition before it
	 * in a skin's chain - counted from the tables' logs (SceneStore::UpdateSlotReferences), and for kIdleFrames after its
	 * last reference goes, so an object that comes back soon finds its slot. A slot can also be freed while referenced
	 * (its record no longer describes anything); its generation then moves on, and every count taken against the old
	 * generation lapses instead of landing on the slot's next use.
	 */
	class SlotTable
	{
	public:
		static constexpr std::uint32_t kIdleFrames = 64;

		struct Allocation
		{
			std::uint32_t slot = 0;
			bool grown = false;  // a new slot at the end: the caller grows its columns
		};

		/** @brief A free slot, or a new one; unreferenced, so it is freed once idle unless a reference comes. */
		Allocation Allocate(std::uint32_t a_frame)
		{
			Allocation allocation;
			if (!freeList.empty()) {
				allocation.slot = freeList.back();
				freeList.pop_back();
			} else {
				allocation.slot = static_cast<std::uint32_t>(alive.size());
				allocation.grown = true;
				alive.push_back(0);
				refs.push_back(0);
				generation.push_back(0);
				idleSince.push_back(0);
			}
			alive[allocation.slot] = 1;
			refs[allocation.slot] = 0;
			++aliveCount;
			BecomeIdle(allocation.slot, a_frame);
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
			freeList.push_back(a_slot);
		}
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
		void Release(std::uint32_t a_slot, std::uint32_t a_generation, std::uint32_t a_frame)
		{
			if (!Alive(a_slot) || generation[a_slot] != a_generation || refs[a_slot] == 0)
				return;
			if (--refs[a_slot] == 0) {
				--referencedCount;
				BecomeIdle(a_slot, a_frame);
			}
		}
		/** @brief Every count to zero, before a recount; each live slot's idle time starts now. */
		void ResetReferences(std::uint32_t a_frame)
		{
			idle.clear();
			referencedCount = 0;
			for (std::uint32_t slot = 0; slot < alive.size(); ++slot) {
				refs[slot] = 0;
				if (alive[slot])
					BecomeIdle(slot, a_frame);
			}
		}
		/** @brief Frees the slots unreferenced for more than kIdleFrames, calling a_onFree(slot) first. */
		template <class F>
		std::uint32_t Expire(std::uint32_t a_frame, F&& a_onFree)
		{
			std::uint32_t freed = 0;
			while (!idle.empty() && a_frame - idle.front().frame > kIdleFrames) {
				const auto entry = idle.front();
				idle.pop_front();
				// An entry stands only for the slot's latest idle spell, at the generation it was taken.
				if (!Alive(entry.slot) || generation[entry.slot] != entry.generation || refs[entry.slot] || idleSince[entry.slot] != entry.frame)
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
		void BecomeIdle(std::uint32_t a_slot, std::uint32_t a_frame)
		{
			idleSince[a_slot] = a_frame;
			idle.push_back({ a_slot, generation[a_slot], a_frame });
		}

		struct Idle
		{
			std::uint32_t slot = 0, generation = 0, frame = 0;
		};
		std::vector<std::uint8_t> alive;
		std::vector<std::uint32_t> refs, generation, idleSince, freeList;
		std::deque<Idle> idle;  // in frame order
		std::size_t aliveCount = 0, referencedCount = 0;
	};
}
