#pragma once

// The fade roots: the fade-root table as a journaled table family (FadeRootTables, its columns) and the coordinator component that lists
// and steps its rows (FadeRoots.cpp: RegisterScene, which holds their ids; the channels' are FadesFeature.cpp's). The coordinator
// counts an unmirrored root (NoteFadeRootUnmirrored: Scene/Coordinator/Coordinator.h, Residents.cpp) and reports the stood-in count
// through it.

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Api/TableFamilies.h"
#include "Features/DrawcallLimitFix/Scene/Model/Records.h"

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Api
{
	class RootListView;
}

namespace DCLF::Scene
{
	/**
	 * @brief The fade-root family's columns (Records.h, FadeRootStatic): a slot per fade node a member draws under, refcounted by
	 * members, its row rewritten when an input event arrives (a tree's LOD switch: fadeRootSwitch). A row's object is one of its members
	 * (whose fade node row is the centre), rechosen when membership changes. Each object's root is the family's objectRow (kNoFadeRoot:
	 * none); the rows written since what the depth commit holds are the family's journal (Extensions::TableFamilyColumns::Mark), which it
	 * sends as runs, not the whole table.
	 */
	struct FadeRootTables
	{
		std::vector<FadeRootStatic> fadeRoots;
		std::vector<std::uint32_t> fadeRootRefs, fadeRootFree;
		std::vector<const void*> fadeRootNode;
		ankerl::unordered_dense::map<const void*, std::uint32_t> fadeRootIndex;
		ankerl::unordered_dense::map<const void*, std::uint32_t> fadeRootSwitch;  // a tree root's LOD switch -> its root slot
		std::uint32_t fadeRootGenerations = 0;
	};

	// The ids Fades' install took (FadeRoots.cpp: the family and the component; FadesFeature.cpp: the channels; none before InstallFeatures).
	Api::TableFamilyId FadeRootFamilyId();
	Api::CoordinatorComponentId FadeRootComponentId();
	Api::EngineChannelId FadeSnapsChannelId();
	Api::EngineChannelId FadeAmountsChannelId();

	// The family's callbacks (Api::TableFamily; FadeRoots.cpp).
	std::shared_ptr<void> MakeFadeRoots();
	void AssignFadeRoots(void* a_to, const void* a_from);
	bool SameFadeRoots(const void* a_left, const void* a_right);
	void ClearFadeRoots(void* a_columns);
	void RecycleFadeRoot(void* a_columns, std::uint32_t a_row);
	std::uint32_t FadeRootRows(const void* a_columns);
	/** @brief Api::RootStatesContribution::view: the roots' rows, nodes and index of a_columns. */
	void ViewFadeRoots(const void* a_columns, Api::RootListView& a_out);

	/**
	 * @brief The component's registration (Api::CoordinatorComponent "fade roots"): listed at a slot's record writes and its entry's
	 * fade node answer (slots.written), unlisted at its release (slots.released), rows stepped by the snaps and amounts (events.keys),
	 * the tree LOD switches (events.switched) and the primary cull's admitted roots (events.admitted); its owners handed back with the
	 * slot tables (retirement.reset). Registered last, so every shared point keeps today's order.
	 */
	Api::CoordinatorComponent FadeRootComponent();

	/** @brief The report (EnginePrimaryCull): the admitted roots the primary cull stands in for (kFadeRootStoodIn). */
	std::size_t FadeRootsStoodIn();
}
