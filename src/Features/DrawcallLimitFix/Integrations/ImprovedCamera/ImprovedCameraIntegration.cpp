// Improved Camera, an SKSE plugin (not a CS feature). In first person it hides the player's head and arms by posing the skins again
// right after the shadow maps are drawn, so the shadows keep the whole body. DCLF samples the skins once, at the frame's start, so it
// registers the player as a late pose (Api/LatePoses.h): the views drawn after the shadows then show the hidden pose too.

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

		// Render thread, at the depth commit: the player while the camera is in first person (Improved Camera's pose is a first-person one).
		const void* Reference()
		{
			const auto& value = SwitchValue(latePoseSwitch);
			if (value == "0" || (value != "1" && value != "rewind" && !PluginLoaded()))
				return nullptr;
			const auto* camera = RE::PlayerCamera::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!camera || !camera->IsInFirstPerson() || !player)
				return nullptr;
			// Test: the player's skins marked as posed again with their bones unchanged, as the plugin marks them. The late producer samples
			// them again, and the frame must look the same.
			if (value == "rewind")
				if (auto* root = player->Get3D(false))
					RE::BSVisit::TraverseScenegraphGeometries(root, [](RE::BSGeometry* a_geometry) {
						if (auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get())
							skin->frameID = 0;
						return RE::BSVisit::BSVisitControl::kContinue;
					});
			return static_cast<const RE::TESObjectREFR*>(player);
		}

		void Install(Api::InstallContext& a_context)
		{
			latePoseSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LATE_POSE", .kind = Api::SwitchKind::Test, .reduced = Api::SwitchReduced::Never,
				.description = "0: the player's skins are not sampled again after the shadow maps (Improved Camera's first-person pose); 1: they are checked in first person without the plugin too; rewind: the test, which marks them posed again with their bones unchanged" });
			a_context.RegisterLatePose({ .name = "improved camera first person", .reference = &Reference });
		}
	}

	Api::Feature Integration()
	{
		return { "improved camera", &Install };
	}
}
