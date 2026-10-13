// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: the local shadow lights' volumes (LightViews' cull planes, which LocalShadowLights samples)
// against their culling processes' after the cull, reported every 3000 volumes.

#include "SceneObservers.h"

#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Engine/Views/SunViews.h"

namespace DCLF::Diagnostics
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
		 * @brief A descriptor's volume as LightViews has it against its culling process after the cull: its planes, and its custom planes
		 * when it has them.
		 */
		void OnShadowVolume(const ShadowVolumeSampled& a_event)
		{
			const auto& a_view = a_event.view;
			const auto& a_process = a_event.process;
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

	void InstallShadowVolumeParity()
	{
		Subscribe<ShadowVolumeSampled>(OnShadowVolume);
	}
}
