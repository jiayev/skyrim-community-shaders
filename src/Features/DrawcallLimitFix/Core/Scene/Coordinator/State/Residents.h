#pragma once

// The residents (CoordinatorResidents): the resident slots and their patches, and what they keep alive (pipeline and material
// members; the components keep their own: State/Components.h, the fade roots' FadeRoots.cpp). Residents.cpp, with the joins (AccumulatePhase.cpp).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorResidents
	{
		// Scene members' records.
		struct ResidentPatch
		{
			AccumulatedPass pass;
			std::uint32_t pipeline = 0, material = 0;
		};
		static constexpr std::uint32_t kNotResident = ~0u;
		// Resident slots, each one's list position, and its patch.
		std::vector<std::uint32_t> residents;
		std::vector<std::uint32_t> residentPos;
		std::vector<ResidentPatch> residentPatches;
		// What the residents keep (KeepResidentsAlive), kept by changes: only slots NoteResidentTouched names are recounted; rebuilt whole
		// when every residency ended or the tables reset.
		bool residentMaintenanceDirty = true;
		/** @brief Slots by key (a pipeline), each in one list at a time: swap-removed, the first kept. */
		struct SlotMembers
		{
			std::vector<std::vector<std::uint32_t>> lists;
			std::vector<std::uint32_t> position;
			void Add(std::uint32_t a_key, std::uint32_t a_slot)
			{
				if (lists.size() <= a_key)
					lists.resize(std::size_t(a_key) + 1);
				if (position.size() <= a_slot)
					position.resize(std::size_t(a_slot) + 1, ~0u);
				position[a_slot] = static_cast<std::uint32_t>(lists[a_key].size());
				lists[a_key].push_back(a_slot);
			}
			void Remove(std::uint32_t a_key, std::uint32_t a_slot)
			{
				auto& list = lists[a_key];
				const std::uint32_t at = position[a_slot];
				const std::uint32_t moved = list.back();
				list[at] = moved;
				position[moved] = at;
				list.pop_back();
				position[a_slot] = ~0u;
			}
			std::uint32_t First(std::uint32_t a_key) const { return a_key < lists.size() && !lists[a_key].empty() ? lists[a_key].front() : ~0u; }
			std::size_t Count(std::uint32_t a_key) const { return a_key < lists.size() ? lists[a_key].size() : 0; }
			void Clear()
			{
				lists.clear();
				position.clear();
			}
			/** @brief Whether a_slot is listed under a_key (Remove's precondition). */
			bool Has(std::uint32_t a_key, std::uint32_t a_slot) const
			{
				return a_key < lists.size() && a_slot < position.size() && position[a_slot] < lists[a_key].size() && lists[a_key][position[a_slot]] == a_slot;
			}
		};
		// What each resident slot was counted under (kNone: not counted).
		struct ResidentCounted
		{
			static constexpr std::uint32_t kNone = ~0u;
			std::uint32_t pipeline = kNone, material = kNone;
		};
		std::vector<ResidentCounted> residentCounted;
		std::vector<std::uint32_t> residentsTouched;   // since the last KeepResidentsAlive
		SlotMembers pipelineMembers;
		std::vector<std::uint32_t> materialMembers;    // by material slot: resident count
		// Pipelines and materials the joins marked used this pass, cleared again where no resident holds them.
		std::vector<std::uint32_t> joinMarkedPipelines, joinMarkedMaterials;
		ankerl::unordered_dense::set<const RE::BSGeometry*> residentJoining;  // this pass's resident passes, until patched
		ankerl::unordered_dense::set<const RE::BSGeometry*> residentLayerJoining;  // the layers'
	};
}
