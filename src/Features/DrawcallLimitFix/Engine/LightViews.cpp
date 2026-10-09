#include "LightViews.h"

#include "Features/DrawcallLimitFix/Scene/SceneStore.h"

#include "State.h"

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

	void LightViews::Report()
	{
		auto& p = parity;
		const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.missing;
		logger::info("[DCLF] light views (T3a: DCLF's, from the cameras the local lights' UpdateCamera set up, against the engine's): {} views ({} matrices, {} eye, "
					 "{} viewport, {} caster volume differ), {} without DCLF's{}{}",
			p.views, p.viewProj, p.eye, p.viewport, p.casters, p.missing, differ ? " <- LIGHT VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		p = {};
	}

	void LightViews::Update(const RE::BSShadowLight* a_light)
	{
		const std::uint32_t frame = SceneStore::Get().GetFrame();
		if (frame != sceneFrame) {
			// A new frame: last frame's views go; the report counts frames that had local lights.
			if (!lights.empty() && SunViews::ParityEnabled() && ++reportFrames >= 300) {
				reportFrames = 0;
				Report();
			}
			lights.clear();
			sceneFrame = frame;
		}
		auto& light = lights.emplace_back();
		light.light = a_light;
		auto& data = const_cast<RE::BSShadowLight*>(a_light)->GetRuntimeData();
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
