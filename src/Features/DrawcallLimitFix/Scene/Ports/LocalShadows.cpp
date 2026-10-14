// LocalShadowLights' render-thread sample from DCLF's light selection and light views, and the per-object mask test.

#include "Features/DrawcallLimitFix/Scene/Ports/LocalShadows.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/Views/LightSelection.h"
#include "Features/DrawcallLimitFix/Engine/Views/LightViews.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"

namespace DCLF
{
	LocalShadowLights LocalShadowLights::Sample()
	{
		LocalShadowLights out;
		// DCLF's selection: the lights CalculateActiveShadowCasterLights keeps, a light once (the sun's slots are the sun's),
		// their mask indices, and per shadowmap descriptor what its cull tests (LightViews: Process2's planes).
		const std::uint32_t sceneFrame = Scene::FrameView::Get().GetFrame();
		const auto* selection = LightSelection::Get().Current(sceneFrame);
		if (!selection)
			return out;
		for (const auto& selected : selection->locals) {
			if (selected.light == selection->sun)
				continue;
			auto* shadowLight = const_cast<RE::BSShadowLight*>(selected.light);
			auto& data = shadowLight->GetRuntimeData();
			Light sampled;
			sampled.maskBit = 1u << (selected.maskIndex & 31);
			sampled.affectsLand = Engine::At<std::uint8_t>(shadowLight, 0x61) != 0;
			if (const auto* niLight = shadowLight->light.get()) {
				sampled.center[0] = niLight->world.translate.x;
				sampled.center[1] = niLight->world.translate.y;
				sampled.center[2] = niLight->world.translate.z;
				sampled.radius = niLight->GetLightRuntimeData().radius.x;
			}
			for (std::uint32_t d = 0; d < data.shadowmapDescriptors.size(); ++d) {
				const auto& descriptor = data.shadowmapDescriptors[d];
				const auto* process = descriptor.cullingProcess;
				if (!process || !descriptor.isEnabled)
					continue;
				const auto* view = LightViews::Get().CascadeOf(selected.light, d, sceneFrame);
				if (!view)
					continue;
				Light::Volume volume;
				for (std::uint32_t q = 0; q < 6; ++q)
					volume.planes[q] = view->cull.plane[q];
				volume.masks[0] = view->cull.mask & 0x3Fu;
				Diagnostics::Notify(Diagnostics::ShadowVolumeSampled{ *view, *process });
				sampled.volumes.push_back(volume);
			}
			out.lights.push_back(std::move(sampled));
		}
		return out;
	}

	std::uint32_t LocalShadowLights::MaskOf(const RE::BSShaderProperty* a_property, const float a_center[3], float a_radius) const
	{
		constexpr std::uint64_t kLandscapeFlags = (1ull << 14) | (1ull << 46);
		const bool landscape = a_property && (a_property->flags.underlying() & kLandscapeFlags);
		std::uint32_t mask = 0;
		for (const auto& light : lights) {
			if (landscape && !light.affectsLand)
				continue;
			const float dx = a_center[0] - light.center[0], dy = a_center[1] - light.center[1], dz = a_center[2] - light.center[2];
			const float reach = light.radius + a_radius;
			if (dx * dx + dy * dy + dz * dz > reach * reach)
				continue;
			for (const auto& volume : light.volumes) {
				bool inside = true;
				for (std::uint32_t p = 0; p < 12 && inside; ++p) {
					if (!((volume.masks[p / 6] >> (p % 6)) & 1))
						continue;
					const auto& plane = volume.planes[p];
					inside = plane[0] * a_center[0] + plane[1] * a_center[1] + plane[2] * a_center[2] - plane[3] >= -a_radius;
				}
				if (inside) {
					mask |= light.maskBit;
					break;
				}
			}
		}
		return mask;
	}
}
