// Shadows/Engine: the shadow epoch's passes and frame points, the occlusion maps drawn in it (see Engine.h).

#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// The passes the schedule names (no setup: a Shadow epoch's pass may have none, so their place among the registrations changes no
			// order), then the epoch's frame points and the shadow-light selection's hooks that reach them.
			RegisterPasses(a_context);
			RegisterShadowEpochs(a_context);
			InstallCasterHooks();
		}
	}

	Api::Feature Feature()
	{
		return { "shadow-epochs", &Install };
	}
}
