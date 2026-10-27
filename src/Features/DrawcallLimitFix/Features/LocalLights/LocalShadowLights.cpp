// LocalShadowLights' render-thread sample from DCLF's light selection and light views (the Spot and Point sources' cameras), and the
// per-object mask test.

#include "Features/DrawcallLimitFix/Features/LocalLights/LocalShadowLights.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LocalLights.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"

namespace DCLF
{
	LocalShadowLights LocalShadowLights::Sample()
	{
		LocalShadowLights out;
		// DCLF's selection: the lights CalculateActiveShadowCasterLights keeps, a light once (the sun's slots are the sun's),
		// their mask indices, and per shadowmap descriptor what its cull tests (its source's camera, Shadows/Local's Lights board:
		// Process2's planes).
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
			// Gated only while the atlas holding its context was encoded (the commits publish that atlas).
			if (selected.portalContext && selected.portalContextIndex != ~0u && !selection->portalContexts.empty())
				sampled.portalContext = selected.portalContextIndex + 1;
			if (const auto* niLight = shadowLight->light.get()) {
				sampled.center[0] = niLight->world.translate.x;
				sampled.center[1] = niLight->world.translate.y;
				sampled.center[2] = niLight->world.translate.z;
				sampled.radius = niLight->GetLightRuntimeData().radius.x;
			}
			const auto source = shadowLight->GetIsParabolicLight() ? Api::ShadowSource::Point : Api::ShadowSource::Spot;
			for (std::uint32_t d = 0; d < data.shadowmapDescriptors.size(); ++d) {
				const auto& descriptor = data.shadowmapDescriptors[d];
				const auto* process = descriptor.cullingProcess;
				if (!process || !descriptor.isEnabled)
					continue;
				Api::ShadowSourceCamera request{ selected.light, d, sceneFrame };
				const auto* view = Extensions::SourceCamera(source, request);
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
}
