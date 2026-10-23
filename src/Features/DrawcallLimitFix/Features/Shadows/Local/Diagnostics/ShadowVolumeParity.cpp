// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: the local shadow lights' volumes (the Lights board's cull planes, which LocalShadowLights samples)
// against their culling processes' after the cull, reported every 3000 volumes.

#include "Features/DrawcallLimitFix/Features/Shadows/Local/Local.h"

#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

namespace DCLF::Features::Shadows::Local
{
	namespace
	{
		ShadowCameras::Planes ToPlanes(const RE::NiFrustumPlanes& a_planes)
		{
			ShadowCameras::Planes out;
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
			// The point lights' paraboloid culls (kCullParabolic): compared, differing, and skipped (the process last set up for another light).
			std::uint64_t parabolic = 0, parabolicDiffer = 0, parabolicOther = 0;
			std::string parabolicFirst;
		};
		VolumeParity volumeParity;

		/**
		 * @brief A point light's paraboloid cull (ShadowCameras::Camera::parabolic*) against the process FUN_141519d20 set up for it
		 * (Accumulate's static BSParabolicCullingProcess, AE 0x14335bfa0, SE 0x143202ae0), once per light (descriptor 0's sample): its sphere, plane and back
		 * accumulator. Only while the process's camera (+0x18) is at the light's sphere centre: the process holds the last light culled.
		 */
		void CheckParabolic(const ShadowCameras::Camera& a_view, VolumeParity& a_parity)
		{
			constexpr DCLF::Engine::Offset kProcess{ 0x335bfa0, 0x3202ae0 };
			constexpr std::size_t kCamera = 0x18, kBackAccumulator = 0x30200, kPlane = 0x30208, kCentre = 0x30218;
			const auto* process = reinterpret_cast<const std::byte*>(REL::Module::get().base() + kProcess);
			const auto* camera = DCLF::Engine::At<const RE::NiAVObject*>(process, kCamera);
			const auto& centre = a_view.parabolicSphere;
			if (!camera || camera->world.translate.x != centre[0] || camera->world.translate.y != centre[1] || camera->world.translate.z != centre[2]) {
				++a_parity.parabolicOther;
				return;
			}
			++a_parity.parabolic;
			std::array<float, 4> sphere{}, plane{};
			std::memcpy(sphere.data(), &DCLF::Engine::At<float>(process, kCentre), sizeof(sphere));
			std::memcpy(plane.data(), &DCLF::Engine::At<float>(process, kPlane), sizeof(plane));
			const bool back = DCLF::Engine::At<const void*>(process, kBackAccumulator) != nullptr;
			if ((sphere != a_view.parabolicSphere || plane != a_view.parabolicPlane || (a_view.parabolicBothSides && !back)) && a_parity.parabolicDiffer++ == 0)
				a_parity.parabolicFirst = fmt::format("sphere ({}, {}, {}, {}) plane ({}, {}, {}, {}) both sides {}, the process's ({}, {}, {}, {}) ({}, {}, {}, {}) back {}", centre[0],
					centre[1], centre[2], centre[3], a_view.parabolicPlane[0], a_view.parabolicPlane[1], a_view.parabolicPlane[2], a_view.parabolicPlane[3], a_view.parabolicBothSides,
					sphere[0], sphere[1], sphere[2], sphere[3], plane[0], plane[1], plane[2], plane[3], back);
		}

		/**
		 * @brief A descriptor's volume as the Lights board has it against its culling process after the cull: its planes, and its custom planes
		 * when it has them.
		 */
		void OnShadowVolume(const Diagnostics::ShadowVolumeSampled& a_event)
		{
			const auto& a_view = a_event.view;
			const auto& a_process = a_event.process;
			auto& p = volumeParity;
			++p.volumes;
			const auto& process = reinterpret_cast<const RE::NiCullingProcess&>(a_process);
			// A zero plane leaves nothing outside: inactive.
			const auto active = [](ShadowCameras::Planes a_planes) {
				for (std::uint32_t q = 0; q < 6; ++q)
					if (a_planes.plane[q] == std::array<float, 4>{})
						a_planes.mask &= ~(1u << q);
				if (!a_planes.mask)
					a_planes = {};
				return a_planes;
			};
			const auto mine = active(a_view.cull);
			const auto planes = active(ToPlanes(process.planes));
			const bool same = ShadowCameras::SamePlanes(mine, planes) &&
			                  (!process.doCustomCullPlanes || ShadowCameras::SamePlanes(mine, active(ToPlanes(process.customCullPlanes))));
			if (!same && p.differ++ == 0)
				p.first = ShadowCameras::DescribePlanes(mine, planes);
			if (a_view.parabolic && !a_view.parabolicBack)
				CheckParabolic(a_view, p);
			if ((p.volumes % 3000) == 0) {
				logger::info("[DCLF] local shadow volumes (T3b: LightViews' cull planes against the culling processes' after the cull): {} volumes, {} differ{}{}; point lights' "
							 "paraboloid culls (sphere, plane) against the process: {} compared, {} differ, {} skipped (another light's){}{}",
					p.volumes, p.differ, p.differ ? " <- LOCAL SHADOW VOLUME" : " <- OK", p.first.empty() ? "" : "; first: " + p.first, p.parabolic, p.parabolicDiffer,
					p.parabolicOther, p.parabolicDiffer ? " <- PARABOLIC CULL" : " <- OK", p.parabolicFirst.empty() ? "" : "; first: " + p.parabolicFirst);
				p = {};
			}
		}
	}

	void SubscribeShadowVolumeParity()
	{
		Diagnostics::Subscribe<Diagnostics::ShadowVolumeSampled>(OnShadowVolume);
	}
}
