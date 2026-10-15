// Hidden nodes: the hidden events' source (the patched stores, its early hooks) and the watch that names a store it misses (its switch,
// observer and report lines).

#include "Features/DrawcallLimitFix/Features/HiddenNodes/HiddenNodes.h"

namespace DCLF::Features::HiddenNodes
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// CS_DCLF_HIDDEN_WATCH.
			RegisterWatch(a_context);
		}
	}

	Api::Feature Feature()
	{
		// The stores go in early, where Core's scene events left them: before the diagnostics and Core's engine side (the primary cull's
		// kept scene lists, the LOD gates) install, which follow the events once Scene::HiddenStoresLive answers.
		return { "hidden-nodes", &Install, &InstallStores };
	}
}
