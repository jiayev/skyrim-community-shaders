// CS_DCLF_PERSISTENT_PARITY: every 60 passes the kept decal order against one made whole (every key taken again: CompareDecalOrder),
// after the order's upkeep. Reported with the scene's lines (Diagnostics/Reports/SceneCoordinator.cpp, SceneState's decalOrder).

#include "Features/DrawcallLimitFix/Features/Decals/DecalOrder.h"
#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"

#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneState.h"

namespace DCLF::Features::Decals
{
	namespace
	{
		void OnOrderDue(const OrderDue& a_event)
		{
			a_event.checked = a_event.checked || Diagnostics::ParityDue(a_event.passSerial);
		}

		void OnOrdered(const Ordered& a_event)
		{
			auto& dp = Diagnostics::SceneDiagnostics().decalOrder;
			const auto result = CompareDecalOrder(a_event.order, a_event.tables, a_event.mirror);
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

	void SubscribeOrderParity()
	{
		Diagnostics::Subscribe<OrderDue>(OnOrderDue);
		Diagnostics::Subscribe<Ordered>(OnOrdered);
	}
}
