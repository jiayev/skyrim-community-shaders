// Tree wind's table family and coordinator component (scene lane): the tree slots listed for the residents that draw under a tree node,
// with their owned engine nodes and seed requests, and the members TreeWindCS writes the wind of.

// TreeWindScene.h first: volk must precede every Vulkan header of the translation unit (the tables, the seeds' items).
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindScene.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindTables.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Publication.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace DCLF::Features::TreeWind
{
	namespace
	{
		Api::TableFamilyId treeFamily;         // install
		Api::CoordinatorComponentId component;  // install

		/** @brief Resident slots, each listed at most once: swap-removed, the first kept. */
		struct TreeMembers
		{
			std::vector<std::uint32_t> list;
			std::vector<std::uint32_t> position;
			void Add(std::uint32_t a_slot)
			{
				if (position.size() <= a_slot)
					position.resize(std::size_t(a_slot) + 1, ~0u);
				position[a_slot] = static_cast<std::uint32_t>(list.size());
				list.push_back(a_slot);
			}
			void Remove(std::uint32_t a_slot)
			{
				const std::uint32_t at = position[a_slot];
				const std::uint32_t moved = list.back();
				list[at] = moved;
				position[moved] = at;
				list.pop_back();
				position[a_slot] = ~0u;
			}
			bool Has(std::uint32_t a_slot) const { return a_slot < position.size() && position[a_slot] < list.size() && list[position[a_slot]] == a_slot; }
			void Clear()
			{
				list.clear();
				position.clear();
			}
		};

		/** @brief The component's state: the resident trees (KeepResidentsAlive's recount), and the listed nodes' references by tree slot. */
		struct TreeComponent
		{
			TreeMembers members;
			// Owned while listed (a published version that names it keeps it, through the retirement chain).
			std::vector<RE::NiPointer<RE::NiAVObject>> owners;
		};

		// The family's callbacks (Api::TableFamily).
		std::shared_ptr<void> MakeTrees() { return std::make_shared<TreeTables>(); }

		void AssignTrees(void* a_to, const void* a_from)
		{
			*static_cast<TreeTables*>(a_to) = *static_cast<const TreeTables*>(a_from);
		}

		// Vectors by their bytes, the index by its entries.
		template <class T>
		bool SameRows(const std::vector<T>& a_left, const std::vector<T>& a_right)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			return a_left.size() == a_right.size() && (a_left.empty() || std::memcmp(a_left.data(), a_right.data(), a_left.size() * sizeof(T)) == 0);
		}

		bool SameTrees(const void* a_left, const void* a_right)
		{
			const auto& x = *static_cast<const TreeTables*>(a_left);
			const auto& y = *static_cast<const TreeTables*>(a_right);
			if (!SameRows(x.trees, y.trees) || !SameRows(x.treeRefs, y.treeRefs) || !SameRows(x.treeFree, y.treeFree) || !SameRows(x.treeNode, y.treeNode) ||
				!SameRows(x.treeObjects, y.treeObjects) || x.treeIndex.size() != y.treeIndex.size())
				return false;
			for (const auto& [key, value] : x.treeIndex)
				if (const auto it = y.treeIndex.find(key); it == y.treeIndex.end() || !(it->second == value))
					return false;
			return true;
		}

		void ClearTrees(void* a_columns)
		{
			auto& trees = *static_cast<TreeTables*>(a_columns);
			trees.trees.clear();
			trees.treeRefs.clear();
			trees.treeFree.clear();
			trees.treeNode.clear();
			trees.treeIndex.clear();
			trees.treeObjects.clear();
			++trees.treesContentEpoch;
			++trees.treeObjectsVersion;
		}

		void RecycleTree(void* a_columns, std::uint32_t a_row)
		{
			static_cast<TreeTables*>(a_columns)->treeFree.push_back(a_row);
		}

		// The rows the scene resources' tree buffers hold (their requirement's: TreeWindScene.cpp), so a revision's reuse witnesses them
		// rather than every write of the family (TableFamilies::RequirementsWitness).
		std::uint32_t TreeRows(const void* a_columns)
		{
			return static_cast<std::uint32_t>(static_cast<const TreeTables*>(a_columns)->trees.size());
		}

		// The component's hooks (Api::CoordinatorComponent).
		std::shared_ptr<void> MakeComponent() { return std::make_shared<TreeComponent>(); }

		/** @brief A member's tree slot given back (the family's reference): its tree's last member retires the slot. */
		void UnlistTree(const Api::CoordinatorComponentContext& a_context, std::uint32_t a_slot)
		{
			auto& tables = a_context.tables;
			auto& family = tables.families.Mutable(treeFamily);
			if (a_slot >= family.objectRow.size())
				return;
			family.NoteWrite();
			auto& trees = *static_cast<TreeTables*>(family.columns.get());
			auto& owners = a_context.State<TreeComponent>().owners;
			auto& current = family.objectRow[a_slot];
			if (current < trees.treeRefs.size() && --trees.treeRefs[current] == 0) {
				trees.treeIndex.erase(trees.treeNode[current]);
				trees.treeNode[current] = nullptr;
				if (current < owners.size())
					a_context.HandBack(std::move(owners[current]));
				tables.Retire(SceneTables::kRetiredFamilyRow, current, treeFamily.index);
			}
			if (current != kNoTree)
				a_context.NoteTouched(a_slot);
			current = kNoTree;
		}

		/** @brief A resident's tree slot: taken for a tree member, given back otherwise (as it leaves: UnlistTree). */
		void ListTree(const Api::CoordinatorComponentContext& a_context, std::uint32_t a_slot)
		{
			// The walk parity's dense walk restores the tables, not the owners kept beside them (as ListFadeRoot).
			if (a_context.DenseWalk())
				return;
			auto& tables = a_context.tables;
			auto& family = tables.families.Mutable(treeFamily);
			auto& trees = *static_cast<TreeTables*>(family.columns.get());
			auto& owners = a_context.State<TreeComponent>().owners;
			const bool tree = a_slot < tables.objects.size() && (tables.objects[a_slot].flags & kObjectTreeAnim);
			const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
			// The tree node from the mirror (its property's fade node, a BSTreeNode); its values are the seed's, which the render thread takes
			// at the frame's start (the scene work reads no engine memory, and runs no engine code).
			TreeStatic row;
			const void* node = nullptr;
			if (tree && geometry) {
				std::uint32_t kind = 0;
				if (const void* fadeNode = a_context.MirroredFadeNode(a_slot, kind); fadeNode && (kind & Api::kMirroredKindTree))
					node = fadeNode;
			}
			// A node not listed yet is owned from the entry's reference (pins), never from the key; without one, nodeless.
			RE::NiPointer<RE::NiAVObject> owner;
			if (node && !trees.treeIndex.contains(node) && !(owner = a_context.HoldFadeNode(a_slot, node)))
				node = nullptr;
			if (family.objectRow.size() <= a_slot) {
				family.NoteWrite();
				family.objectRow.resize(std::size_t(a_slot) + 1, kNoTree);
			}
			auto& current = family.objectRow[a_slot];
			const std::uint32_t wanted = !tree ? kNoTree : !node ? kNodelessTree : kNoTree;
			// Already under this node (a patch that changed something else), or nodeless and staying so.
			if (node && current < trees.treeNode.size() && trees.treeNode[current] == node)
				return;
			if (!node && current == wanted)
				return;
			family.NoteWrite();
			UnlistTree(a_context, a_slot);
			if (!node) {
				current = wanted;
				a_context.NoteTouched(a_slot);
				return;
			}
			auto [it, inserted] = trees.treeIndex.try_emplace(node, 0u);
			if (inserted) {
				std::uint32_t t;
				if (!trees.treeFree.empty()) {
					t = trees.treeFree.back();
					trees.treeFree.pop_back();
				} else {
					t = static_cast<std::uint32_t>(trees.trees.size());
					trees.trees.emplace_back();
					trees.treeRefs.push_back(0);
					trees.treeNode.push_back(nullptr);
					family.rowTag.push_back(0);
				}
				row.generation = ++trees.treeGenerations;
				row.seedOdd = (trees.trees[t].seedOdd & 1u) ^ 1u;
				trees.trees[t] = row;
				family.rowTag[t] = row.generation;
				trees.treeNode[t] = node;
				// Owned while listed (a published version that names it keeps it, through the retirement chain).
				if (owners.size() <= t)
					owners.resize(std::size_t(t) + 1);
				owners[t] = std::move(owner);
				a_context.RequestSeed({ owners[t], t, row.generation, 2 * t + row.seedOdd });
				it->second = t;
				++trees.treesContentEpoch;
			}
			++trees.treeRefs[it->second];
			current = it->second;
			a_context.NoteTouched(a_slot);
		}

		/** @brief The resident trees recounted as the coordinator recounted its residents, and the members TreeWindCS writes the wind of. */
		void Recount(const Api::CoordinatorComponentContext& a_context, const Api::ResidentRecount& a_recount)
		{
			auto& tables = a_context.tables;
			auto& members = a_context.State<TreeComponent>().members;
			bool treesTouched = false;
			// A counted slot is a resident within the objects (KeepResidentsAlive's keys).
			const auto counted = [&](std::uint32_t a_slot) { return a_context.Counted(a_slot) && (tables.objects[a_slot].flags & kObjectTreeAnim); };
			if (a_recount.rebuild) {
				members.Clear();
				for (const std::uint32_t slot : a_recount.residents)
					if (counted(slot))
						members.Add(slot);
				treesTouched = true;
			} else {
				// Their old keys out (all of them first), then their keys now.
				for (const std::uint32_t slot : a_recount.touched)
					if (members.Has(slot)) {
						members.Remove(slot);
						treesTouched = true;
					}
				for (const std::uint32_t slot : a_recount.touched)
					if (counted(slot)) {
						members.Add(slot);
						treesTouched = true;
					}
			}
			// The members TreeWindCS writes the wind of, by their tree slots (in slot order).
			if (!treesTouched)
				return;
			auto& family = tables.families.Mutable(treeFamily);
			auto& trees = *static_cast<TreeTables*>(family.columns.get());
			std::vector<std::uint32_t> slots = members.list;
			std::sort(slots.begin(), slots.end());
			std::vector<TreeObject> treeObjects;
			treeObjects.reserve(slots.size());
			for (const std::uint32_t slot : slots)
				if (slot < family.objectRow.size() && family.objectRow[slot] != kNoTree)
					treeObjects.push_back({ slot, family.objectRow[slot] });
			if (treeObjects.size() != trees.treeObjects.size() ||
				std::memcmp(treeObjects.data(), trees.treeObjects.data(), treeObjects.size() * sizeof(TreeObject)) != 0) {
				trees.treeObjects = std::move(treeObjects);
				++trees.treeObjectsVersion;
				family.NoteWrite();
			}
		}

		// The listed nodes' references through the retirement chain with everything else the cleared tables named.
		void Reset(const Api::CoordinatorComponentContext& a_context)
		{
			a_context.HandBack(a_context.State<TreeComponent>().owners);
		}
	}

	Api::TableFamilyId TreeFamilyId()
	{
		return treeFamily;
	}

	const Extensions::TableFamilyColumns* TreeFamilyOf(const SceneTables& a_tables)
	{
		return a_tables.families.Find(treeFamily);
	}

	const TreeTables& TreesOf(const SceneTables& a_tables)
	{
		static const TreeTables kNone;
		const auto* trees = a_tables.families.Columns<TreeTables>(treeFamily);
		return trees ? *trees : kNone;
	}

	std::uint32_t TreeOf(const SceneTables& a_tables, std::uint32_t a_object)
	{
		const auto* family = TreeFamilyOf(a_tables);
		return family ? family->RowOf(a_object) : kNoTree;
	}

	Api::CoordinatorComponentId ComponentId()
	{
		return component;
	}

	void RegisterTables(Api::InstallContext& a_context)
	{
		treeFamily = a_context.RegisterTableFamily({ .name = "trees", .make = &MakeTrees, .assign = &AssignTrees, .same = &SameTrees, .clear = &ClearTrees,
			.recycle = &RecycleTree, .rowsEnd = kNodelessTree, .recordWord = kTreeRecordWord, .rows = &TreeRows });
		component = a_context.RegisterCoordinatorComponent(
			{ .name = "tree-wind",
				.make = &MakeComponent,
				.residency = { .resident = &ListTree, .left = &UnlistTree, .recount = &Recount },
				.retirement = { .reset = &Reset } });
	}
}
