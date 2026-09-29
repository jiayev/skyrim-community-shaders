#include "NativeMenu/NativeMenu.h"

#include "Features/ScreenSpaceGI.h"
#include "Globals.h"
#include "I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.screen_space_gi."

namespace
{
	using Settings = ScreenSpaceGI::Settings;

	struct SSGIRoot
	{
		static Settings& Live() { return globals::features::screenSpaceGI.settings; }
		static Settings Defaults() { return Settings{}; }
	};

	void __stdcall SetSSGIEnabled(float v)
	{
		auto& ssgi = globals::features::screenSpaceGI;
		ssgi.settings.Enabled = v != 0.0f;
		ssgi.queuedResetHistory = true;
	}

	bool __stdcall IsSSGIEditable()
	{
		return NativeMenu::IsFeatureEditable(globals::features::screenSpaceGI.GetShortName());
	}
}

namespace NativeMenu
{
	std::vector<Row> SSGIRows()
	{
		if (!globals::features::screenSpaceGI.loaded)
			return {};

		using Enabled = Bind<SSGIRoot, &Settings::Enabled>;

		return {
			Checkbox(T(TKEY("enable"), "Enable Screen Space GI"), &Enabled::GetFlag, &SetSSGIEnabled, Enabled::Default(),
				T(TKEY("enabled_tooltip"),
					"Enable Screen Space Global Illumination. When disabled, all other settings are ignored."),
				&IsSSGIEditable),
		};
	}
}

#undef I18N_KEY_PREFIX
