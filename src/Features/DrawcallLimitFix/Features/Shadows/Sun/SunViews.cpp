// SunViews: the UpdateCamera hook (render thread), which writes the sun's board (SunBoard) from the cameras it set up, and what Core
// reads of the board: the Sun source's camera and the sun volume (the cascades, the full-frustum planes).

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SunVolume.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <algorithm>

#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Features::Shadows::Sun
{
	SunFrame& SunBoard()
	{
		static SunFrame frame;
		return frame;
	}

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
		SunFrame& next = SunBoard();
		next.sceneFrame = Scene::FrameView::Get().GetFrame();
		next.valid = false;
		next.cascades = {};
		const auto& descriptors = light.GetRuntimeData().shadowmapDescriptors;
		next.count = std::min<std::uint32_t>(descriptors.size(), SunFrame::kMaxCascades);
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

	void SunCamera(Api::ShadowSourceCamera& a_camera)
	{
		a_camera.camera = SunBoard().CascadeOf(a_camera.descriptor, a_camera.sceneFrame);
	}

	void SampleSunCascades(Api::SunCascadeSink& a_sink)
	{
		// This frame's cascades, whether or not the entry exclusion ran: the colour pass's sun test needs them every frame (without them
		// every pass with the sun's bits would miss, and draw unshadowed). From DCLF's own views (the Sun board, from the cameras UpdateCamera
		// set up for the sun this frame), so the test does not depend on the engine's Accumulate running. None when UpdateCamera did not
		// run for the sun this frame (the sun does not draw: an interior), when the engine gives no pass the sun's bits either. Each
		// cascade's planes are those Accumulate's hook copies from the same views (the bits' cascades), in descriptor order.
		const auto& views = SunBoard();
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		if (!views.valid || views.sceneFrame != frame)
			return;
		for (std::uint32_t d = 0; d < views.count && d < views.cascades.size(); ++d)
			if (const auto* cascade = views.CascadeOf(d, frame))
				a_sink.Put(*cascade);
	}

	bool SampleSunEntryPlanes(Api::SunEntryPlanesSink& a_sink, std::uint32_t a_frame)
	{
		// DCLF's full-frustum planes as UpdateCamera set up each process: a_frame's at the shadow epoch, the frame before's for the build
		// ahead (posted before this frame's UpdateCamera), as the engine held them there.
		const auto& views = SunBoard();
		if (!views.valid || (views.sceneFrame != a_frame && views.sceneFrame + 1 != a_frame) || views.fullFrusta.empty())
			return false;
		for (const auto& planes : views.fullFrusta)
			a_sink.Put(planes);
		return true;
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
