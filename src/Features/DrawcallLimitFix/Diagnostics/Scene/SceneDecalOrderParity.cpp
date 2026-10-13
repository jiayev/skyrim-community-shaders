#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

// CS_DCLF_PERSISTENT_PARITY: every 60 passes the kept decal order against one made whole (every key taken again: Coordinator's
// CompareDecalOrder), after the order's upkeep.

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		void OnDecalOrderDue(const DecalOrderDue& a_event)
		{
			a_event.checked = a_event.checked || ParityDue(V::passSerial(a_event.store));
		}

		void OnDecalsOrdered(const DecalsOrdered& a_event)
		{
			auto& dp = SceneDiagnostics().decalOrder;
			const auto result = V::CompareDecalOrder(a_event.store);
			++dp.checks;
			dp.decals += result.decals;
			if (result.staleKeys && dp.staleKeys == 0)
				dp.staleFirst = result.staleFirst;
			dp.staleKeys += result.staleKeys;
			dp.unordered += result.unordered;
			if (result.differ && dp.differ++ == 0)
				dp.first = result.first;
		}
	}

	void InstallDecalOrderParity()
	{
		Subscribe<DecalOrderDue>(OnDecalOrderDue);
		Subscribe<DecalsOrdered>(OnDecalsOrdered);
	}
}
