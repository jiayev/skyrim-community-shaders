#include "LocalShadows.h"

#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		void StorePlanes(const RE::NiFrustumPlanes& a_planes, std::array<std::array<float, 4>, 12>& a_out, std::uint32_t a_first)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				const auto& plane = a_planes.cullingPlanes[p];
				a_out[a_first + p] = { plane.normal.x, plane.normal.y, plane.normal.z, plane.constant };
			}
		}
	}

	LocalShadowLights LocalShadowLights::Sample()
	{
		LocalShadowLights out;
		auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		if (!node)
			return out;
		const auto& runtime = node->GetRuntimeData();
		// shadowLightsAccum is the shadow-caster array the selection indexes by the mask's bits: the lights that accumulated
		// this frame, a light once per bit it took (the sun once per cascade). activeShadowLights also holds the ones in
		// range that did not, with a stale maskIndex.
		const RE::BSShadowLight* previous = nullptr;
		for (auto* shadowLight : runtime.shadowLightsAccum) {
			if (!shadowLight || shadowLight == previous || shadowLight == runtime.sunShadowDirLight)
				continue;
			previous = shadowLight;
			auto& data = shadowLight->GetRuntimeData();
			Light sampled;
			sampled.maskBit = 1u << (data.maskIndex & 31);
			sampled.affectsLand = Engine::At<std::uint8_t>(shadowLight, 0x61) != 0;
			if (const auto* niLight = shadowLight->light.get()) {
				sampled.center[0] = niLight->world.translate.x;
				sampled.center[1] = niLight->world.translate.y;
				sampled.center[2] = niLight->world.translate.z;
				sampled.radius = niLight->GetLightRuntimeData().radius.x;
			}
			for (const auto& descriptor : data.shadowmapDescriptors) {
				const auto* process = descriptor.cullingProcess;
				if (!process || !descriptor.isEnabled)
					continue;
				Light::Volume volume;
				StorePlanes(process->planes, volume.planes, 0);
				volume.masks[0] = process->planes.activePlanes.underlying() & 0x3Fu;
				if (process->doCustomCullPlanes) {
					StorePlanes(process->customCullPlanes, volume.planes, 6);
					volume.masks[1] = process->customCullPlanes.activePlanes.underlying() & 0x3Fu;
				}
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
