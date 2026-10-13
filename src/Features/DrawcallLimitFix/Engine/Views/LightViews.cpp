// LightViews: the kept local lights' views, computed on the render thread from LightSelection's detour.

#include "Features/DrawcallLimitFix/Engine/Views/LightViews.h"

#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF
{
	LightViews& LightViews::Get()
	{
		static LightViews instance;
		return instance;
	}

	const SunViews::Cascade* LightViews::CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
	{
		if (a_sceneFrame != sceneFrame)
			return nullptr;
		for (const auto& light : lights)
			if (light.light == a_light)
				return a_descriptor < light.views.size() && light.views[a_descriptor].valid ? &light.views[a_descriptor] : nullptr;
		return nullptr;
	}

	void LightViews::Update(const RE::BSShadowLight* a_light)
	{
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		if (frame != sceneFrame) {
			// A new frame: last frame's views go; the observers count frames that had local lights.
			if (!lights.empty())
				Diagnostics::Notify<Diagnostics::EnginePoint::LightViewsFrame>();
			lights.clear();
			sceneFrame = frame;
		}
		auto& light = lights.emplace_back();
		light.light = a_light;
		auto& data = const_cast<RE::BSShadowLight*>(a_light)->GetRuntimeData();
		if (const auto* niLight = a_light->light.get())
			light.sphere = { niLight->world.translate.x, niLight->world.translate.y, niLight->world.translate.z,
				const_cast<RE::NiLight*>(niLight)->GetLightRuntimeData().radius.x };
		light.views.resize(data.shadowmapDescriptors.size());
		for (std::uint32_t d = 0; d < data.shadowmapDescriptors.size(); ++d) {
			const auto& descriptor = data.shadowmapDescriptors[d];
			const auto* camera = descriptor.camera.get();
			if (!camera)
				continue;
			auto& view = light.views[d];
			SunViews::CameraMatrices(*camera, view);
			SunViews::FrustumPlanes(const_cast<RE::NiCamera*>(camera)->GetRuntimeData2().viewFrustum, camera->world, view.frustum);
			// The caster volume UpdateCamera left on the light's culling process (none: mask 0), and what its cull tests.
			const auto* process = static_cast<const RE::NiCullingProcess*>(descriptor.cullingProcess);
			if (process && process->doCustomCullPlanes)
				SunViews::CullPlanes(process, *camera, view.caster);
			// A point light's process (BSParabolicCullingProcess) culls without planes: they stay zero, and nothing is outside them.
			if (!const_cast<RE::BSShadowLight*>(a_light)->GetIsParabolicLight() || (process && process->doCustomCullPlanes))
				SunViews::CullPlanes(process, *camera, view.cull);
			view.valid = true;
		}
	}
}
