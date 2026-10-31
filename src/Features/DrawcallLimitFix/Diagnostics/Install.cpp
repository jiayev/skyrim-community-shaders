// Diagnostics::Install: the areas' installs, once, at PostPostLoad (before DCLF's threads start).
#include "Install.h"

#include "Hooks.h"
#include "PlayerParity.h"

namespace DCLF::Diagnostics
{
	void Install()
	{
		static bool installed = false;
		if (installed)
			return;
		installed = true;
		InstallFeature();
		InstallEngine();
		InstallScene();
		InstallDraws();
		PlayerParity::Install();
	}
}
