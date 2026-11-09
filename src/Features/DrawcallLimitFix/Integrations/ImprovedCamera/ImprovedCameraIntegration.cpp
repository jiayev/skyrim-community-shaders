// Improved Camera, an SKSE plugin (not a CS feature). In first person it hides the player's head and arms, and the weapons on the back,
// right after the shadow maps are drawn, so the shadows keep the whole body. It does that by shrinking those nodes to almost nothing.
// DCLF samples the frame's rows once, at the frame's start, so it names the player's third-person root as a late pose (Api/LatePoses.h):
// the views drawn after the shadows then leave the hidden parts out too.

#include "Features/DrawcallLimitFix/Integrations/ImprovedCamera/ImprovedCamera.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Integrations::ImprovedCamera
{
	namespace
	{
		Api::SwitchId latePoseSwitch;

		bool PluginLoaded()
		{
			// Read at the first frame, long after the plugins loaded.
			static const bool loaded = ::GetModuleHandleW(L"ImprovedCameraSE.dll") != nullptr;
			return loaded;
		}

		// Render thread: the player's third-person root while the camera is in first person (Improved Camera hides parts of it then).
		const void* Root()
		{
			const auto& value = SwitchValue(latePoseSwitch);
			if (value == "0" || (value != "1" && !PluginLoaded()))
				return nullptr;
			const auto* camera = RE::PlayerCamera::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!camera || !camera->IsInFirstPerson() || !player)
				return nullptr;
			return player->Get3D(false);
		}

		// Any thread: whether the plugin is loaded and the late pose is looked for.
		std::string Status()
		{
			const auto& value = SwitchValue(latePoseSwitch);
			const bool on = value != "0" && (value == "1" || PluginLoaded());
			return fmt::format("plugin {}, late pose {}{}", PluginLoaded() ? "loaded" : "not loaded", on ? "on" : "off",
				value.empty() ? "" : fmt::format(" (CS_DCLF_LATE_POSE={})", value));
		}

		void Install(Api::InstallContext& a_context)
		{
			latePoseSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LATE_POSE", .kind = Api::SwitchKind::Test, .reduced = Api::SwitchReduced::Never,
				.description = "0: the player's parts Improved Camera hides after the shadow maps are not looked for; 1: they are looked for in first person without the plugin too" });
			a_context.RegisterLatePose({ .name = "improved camera first person", .root = &Root });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "improved camera", .install = &Install, .status = &Status };
	}
}
