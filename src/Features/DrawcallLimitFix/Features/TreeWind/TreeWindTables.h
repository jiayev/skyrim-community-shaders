#pragma once

// Tree wind's table family (Api::TableFamily): the tree slots its members draw under, held in the scene's tables and published with
// them. Written by the feature's coordinator component (TreeWindTables.cpp) on the scene lane; read by the seeds, the scene resources'
// commit and requirement, and the parity, from the tables they are handed.

#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <vector>

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Features::TreeWind
{
	/**
	 * @brief The trees' columns: a slot per tree node a member draws under, refcounted by members (the objects' tree slots are the family's
	 * references: Extensions::TableFamilyColumns::objectRow, kNoTree or kNodelessTree; each slot's generation its row tag); treeObjects
	 * lists the members for TreeWindCS. The commits upload against the versions.
	 */
	struct TreeTables
	{
		std::vector<TreeStatic> trees;
		std::vector<std::uint32_t> treeRefs, treeFree;
		std::vector<const void*> treeNode;
		ankerl::unordered_dense::map<const void*, std::uint32_t> treeIndex;
		std::vector<TreeObject> treeObjects;
		std::uint64_t treesContentEpoch = 0, treeObjectsVersion = 0;
		std::uint32_t treeGenerations = 0;
	};

	/** @brief The trees' family's registration (its fit's slot: Draws::SceneFit::familyRows). */
	Api::TableFamilyId TreeFamilyId();
	/** @brief The trees' family in a_tables (null before the coordinator first wrote one: no tree listed). */
	const Extensions::TableFamilyColumns* TreeFamilyOf(const SceneTables& a_tables);
	/** @brief The trees' columns in a_tables (empty ones before the coordinator first wrote them). */
	const TreeTables& TreesOf(const SceneTables& a_tables);
	/** @brief a_object's tree slot in a_tables (kNoTree, kNodelessTree, or a slot). */
	std::uint32_t TreeOf(const SceneTables& a_tables, std::uint32_t a_object);
}
