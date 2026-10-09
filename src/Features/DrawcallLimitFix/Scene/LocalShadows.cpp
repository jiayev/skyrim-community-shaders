#include "LocalShadows.h"

#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/LightSelection.h"
#include "Features/DrawcallLimitFix/Engine/LightViews.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		SunViews::Planes ToPlanes(const RE::NiFrustumPlanes& a_planes)
		{
			SunViews::Planes out;
			for (std::uint32_t p = 0; p < 6; ++p) {
				const auto& plane = a_planes.cullingPlanes[p];
				out.plane[p] = { plane.normal.x, plane.normal.y, plane.normal.z, plane.constant };
			}
			out.mask = a_planes.activePlanes.underlying() & 0x3Fu;
			return out;
		}

		struct VolumeParity
		{
			std::uint64_t volumes = 0, differ = 0;
			std::string first;
		};
		VolumeParity volumeParity;

		/**
		 * @brief The parity's: a descriptor's volume as LightViews has it against its culling process after the cull - its planes, and
		 * its custom planes when it has them, both of which the volumes tested before T3b.
		 */
		void CheckVolume(const SunViews::Cascade& a_view, const RE::BSCullingProcess& a_process)
		{
			auto& p = volumeParity;
			++p.volumes;
			const auto& process = reinterpret_cast<const RE::NiCullingProcess&>(a_process);
			// A zero plane leaves nothing outside: inactive.
			const auto active = [](SunViews::Planes a_planes) {
				for (std::uint32_t q = 0; q < 6; ++q)
					if (a_planes.plane[q] == std::array<float, 4>{})
						a_planes.mask &= ~(1u << q);
				if (!a_planes.mask)
					a_planes = {};
				return a_planes;
			};
			const auto mine = active(a_view.cull);
			const auto planes = active(ToPlanes(process.planes));
			const bool same = SunViews::SamePlanes(mine, planes) &&
			                  (!process.doCustomCullPlanes || SunViews::SamePlanes(mine, active(ToPlanes(process.customCullPlanes))));
			if (!same && p.differ++ == 0)
				p.first = SunViews::DescribePlanes(mine, planes);
			if ((p.volumes % 3000) == 0) {
				logger::info("[DCLF] local shadow volumes (T3b: LightViews' cull planes against the culling processes' after the cull): {} volumes, {} differ{}{}", p.volumes,
					p.differ, p.differ ? " <- LOCAL SHADOW VOLUME" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
				p = {};
			}
		}
	}

	LocalShadowLights LocalShadowLights::Sample()
	{
		LocalShadowLights out;
		// DCLF's selection (T3b): the lights CalculateActiveShadowCasterLights keeps, a light once (the sun's slots are the sun's),
		// their mask indices, and per shadowmap descriptor what its cull tests (LightViews, T3a: Process2's planes).
		const std::uint32_t sceneFrame = SceneStore::Get().GetFrame();
		const auto* selection = LightSelection::Get().Current(sceneFrame);
		if (!selection)
			return out;
		const bool parity = SunViews::ParityEnabled();
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
				if (parity)
					CheckVolume(*view, *process);
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
