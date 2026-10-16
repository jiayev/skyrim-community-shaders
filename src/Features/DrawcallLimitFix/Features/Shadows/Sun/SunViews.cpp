// SunViews: the UpdateCamera hook (render thread), which writes the sun's board (ShadowCameras::Sun) from the cameras it set up.

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <algorithm>

#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Features::Shadows::Sun
{
	SunViews& SunViews::Get()
	{
		static SunViews instance;
		return instance;
	}

	void SunViews::Update(const void* a_light)
	{
		auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		const auto* sun = node ? node->GetRuntimeData().sunShadowDirLight : nullptr;
		// The sun alone: the shadow scene node's caster list holds a second directional light whose descriptors are never drawn.
		if (!sun || a_light != sun)
			return;
		auto& light = *const_cast<RE::BSShadowDirectionalLight*>(sun);
		// In place (the full-frusta's storage is reused): written and read on this thread; the registration threads read the cascades
		// only after SunAccumulation's bitsReady release, later in the frame.
		ShadowCameras::SunFrame& next = ShadowCameras::Sun();
		next.sceneFrame = Scene::FrameView::Get().GetFrame();
		next.valid = false;
		next.cascades = {};
		const auto& descriptors = light.GetRuntimeData().shadowmapDescriptors;
		next.count = std::min<std::uint32_t>(descriptors.size(), ShadowCameras::SunFrame::kMaxCascades);
		for (std::uint32_t d = 0; d < next.count; ++d) {
			const auto& descriptor = descriptors[d];
			const auto* camera = descriptor.camera.get();
			if (!camera)
				continue;
			auto& cascade = next.cascades[d];
			ShadowCameras::CameraMatrices(*camera, cascade);
			ShadowCameras::FrustumPlanes(const_cast<RE::NiCamera*>(camera)->GetRuntimeData2().viewFrustum, camera->world, cascade.frustum);
			// The caster volume UpdateCamera has just written to the cascade's culling process (FUN_141514c10): its output, not a cull's.
			const auto* process = static_cast<const RE::NiCullingProcess*>(descriptor.cullingProcess);
			if (process && process->doCustomCullPlanes)
				ShadowCameras::CullPlanes(process, *camera, cascade.caster);
			ShadowCameras::CullPlanes(process, *camera, cascade.cull);
			cascade.valid = true;
		}
		// Each process of the full-frustum cull culls through the full-frustum camera (FUN_141511f30 sets it as theirs).
		auto& runtime = light.GetShadowDirectionalLightRuntimeData();
		next.fullFrusta.clear();
		if (const auto* camera = runtime.fullFrustumCamera.get())
			for (const auto& process : runtime.fullFrustumCullingProcessArray)
				if (process)
					ShadowCameras::CullPlanes(process.get(), *camera, next.fullFrusta.emplace_back());
		next.valid = true;
		Diagnostics::Notify<Diagnostics::EnginePoint::SunViewsUpdated>();
	}

	struct SunViews::Hooks
	{
		/** @brief BSShadowDirectionalLight::UpdateCamera (vtable slot 0x10, 0x141512230): DCLF's views from what it set up. */
		struct UpdateCamera
		{
			static bool thunk(RE::BSShadowDirectionalLight* a_light, RE::NiCamera* a_camera)
			{
				const bool result = func(a_light, a_camera);
				SunViews::Get().Update(a_light);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void SunViews::Install()
	{
		if (installed)
			return;
		stl::write_vfunc<0x10, Hooks::UpdateCamera>(RE::VTABLE_BSShadowDirectionalLight[0]);
		installed = true;
		logger::info("[DCLF] sun views: BSShadowDirectionalLight::UpdateCamera hooked (T2a)");
	}
}
