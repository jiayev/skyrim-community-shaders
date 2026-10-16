// Shadows/Engine: the shadow and occlusion epochs' passes and frame points (see Engine.h).

#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// The passes the schedule names (no setup: a Shadow epoch's pass may have none, so their place among the registrations changes no
			// order), then the epochs' frame points.
			RegisterPasses(a_context);
			RegisterShadowEpochs(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "shadow-epochs", &Install };
	}
}
