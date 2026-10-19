// LightViews: the kept local lights' views (LightsBoard), computed on the render thread from LightSelection's detour (the Spot and Point
// sources' lightKept), and the sources' camera over them.

#include "Features/DrawcallLimitFix/Features/Shadows/Local/LightViews.h"

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"

#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Features::Shadows::Local
{
	LightFrame& LightsBoard()
	{
		static LightFrame frame;
		return frame;
	}

	void LightCamera(Api::ShadowSourceCamera& a_camera)
	{
		a_camera.camera = LightsBoard().CascadeOf(a_camera.light, a_camera.descriptor, a_camera.sceneFrame);
	}

	LightViews& LightViews::Get()
	{
		static LightViews instance;
		return instance;
	}

	void LightViews::Update(const RE::BSShadowLight* a_light)
	{
		auto& board = LightsBoard();
		auto& lights = board.lights;
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		if (frame != board.sceneFrame) {
			// A new frame: last frame's views go; the observers count frames that had local lights.
			if (!lights.empty())
				Diagnostics::Notify<Diagnostics::EnginePoint::LightViewsFrame>();
			lights.clear();
			board.sceneFrame = frame;
		}
		auto& light = lights.emplace_back();
		light.light = a_light;
		auto& data = const_cast<RE::BSShadowLight*>(a_light)->GetRuntimeData();
		if (const auto* niLight = a_light->light.get())
			light.sphere = { niLight->world.translate.x, niLight->world.translate.y, niLight->world.translate.z,
				const_cast<RE::NiLight*>(niLight)->GetLightRuntimeData().radius.x };
		light.views.resize(data.shadowmapDescriptors.size());
		// A portal-strict point light's casters pass its strict gates: its entry in the selection's atlas (encoded: the commits publish it).
		std::uint32_t portalContext = 0;
		if (const auto* selection = LightSelection::Get().Current(frame); selection && !selection->portalContexts.empty())
			if (const auto* entry = selection->LightOf(a_light); entry && entry->portalContext && entry->portalContextIndex != ~0u)
				portalContext = entry->portalContextIndex + 1;
		for (std::uint32_t d = 0; d < data.shadowmapDescriptors.size(); ++d) {
			const auto& descriptor = data.shadowmapDescriptors[d];
			const auto* camera = descriptor.camera.get();
			if (!camera)
				continue;
			auto& view = light.views[d];
			ShadowCameras::CameraMatrices(*camera, view);
			ShadowCameras::FrustumPlanes(const_cast<RE::NiCamera*>(camera)->GetRuntimeData2().viewFrustum, camera->world, view.frustum);
			// The caster volume UpdateCamera left on the light's culling process (none: mask 0), and what its cull tests.
			const auto* process = static_cast<const RE::NiCullingProcess*>(descriptor.cullingProcess);
			if (process && process->doCustomCullPlanes)
				ShadowCameras::CullPlanes(process, *camera, view.caster);
			// A point light's process (BSParabolicCullingProcess) culls without planes: they stay zero, and nothing is outside them.
			if (!const_cast<RE::BSShadowLight*>(a_light)->GetIsParabolicLight() || (process && process->doCustomCullPlanes))
				ShadowCameras::CullPlanes(process, *camera, view.cull);
			view.portalContext = portalContext;
			view.valid = true;
		}
	}
}
