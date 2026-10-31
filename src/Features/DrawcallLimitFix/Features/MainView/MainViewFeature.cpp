// MainView: the main view's draw passes and the main pass's points (see MainView.h).

#include "Features/DrawcallLimitFix/Features/MainView/MainView.h"

namespace DCLF::Features::MainView
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// The draws the schedule names (no setup, so their place among the registrations changes no order), then the main pass's points.
			RegisterPasses(a_context);
			RegisterPrimaryView(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "main-view", &Install };
	}
}
