// Core's built-in gates (Scene::RegisterCoreGates): registered as Core at the start of InstallFeatures, before every feature, on the
// generic structures a feature's gates would use (Api/EngineEvents.h: SceneStack channel, show veto, adoption guard, reserved hidden
// site; Api/CoordinatorComponents.h: the component's marks, vetoes, commit and adoption hooks).

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Scene
{
	void RegisterCoreGates([[maybe_unused]] Api::InstallContext& a_context)
	{
		// Nothing yet. The LOD gates register here, while LodGates::Enabled(), as they move onto the generic structures: their SceneStack
		// channel, show veto and reserved hidden site (b1), adoption guard (b2), component with its marks and events (b3), commit hooks (b4).
	}
}
