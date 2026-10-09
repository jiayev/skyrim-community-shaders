#include "SunViews.h"

#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "State.h"

namespace DCLF
{
	SunViews& SunViews::Get()
	{
		static SunViews instance;
		return instance;
	}

	const SunViews::Cascade* SunViews::CascadeOf(std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
	{
		if (!frame.valid || frame.sceneFrame != a_sceneFrame || a_descriptor >= frame.count || !frame.cascades[a_descriptor].valid)
			return nullptr;
		return &frame.cascades[a_descriptor];
	}

	void SunViews::FrustumPlanes(const RE::NiFrustum& a_frustum, const RE::NiTransform& a_world, Planes& a_out)
	{
		// FUN_140d32cc0 (AE): the camera looks down its rotation's first column, up its second, right its third.
		const auto& r = a_world.rotate.entry;
		const float dir[3] = { r[0][0], r[1][0], r[2][0] };
		const float up[3] = { r[0][1], r[1][1], r[2][1] };
		const float right[3] = { r[0][2], r[1][2], r[2][2] };
		const float eye[3] = { a_world.translate.x, a_world.translate.y, a_world.translate.z };
		// FUN_140d32a60: the plane through a point, d = (p.y * n.y + n.x * p.x) + p.z * n.z.
		auto plane = [&](std::uint32_t a_index, const float (&a_n)[3], const float (&a_p)[3]) {
			auto& p = a_out.plane[a_index];
			p[0] = a_n[0];
			p[1] = a_n[1];
			p[2] = a_n[2];
			const float yy = a_p[1] * a_n[1];
			const float xx = a_n[0] * a_p[0];
			const float zz = a_p[2] * a_n[2];
			p[3] = (yy + xx) + zz;
		};
		auto along = [&](const float (&a_axis)[3], float a_distance, float (&a_out3)[3]) {
			for (std::uint32_t i = 0; i < 3; ++i)
				a_out3[i] = a_axis[i] * a_distance + eye[i];
		};
		auto negate = [](const float (&a_v)[3], float (&a_out3)[3]) {
			for (std::uint32_t i = 0; i < 3; ++i)
				a_out3[i] = -a_v[i];
		};
		float point[3], normal[3];
		// Near: the view direction, through the eye moved to the near distance; far: its opposite at the far distance.
		along(dir, a_frustum.fNear, point);
		plane(0, dir, point);
		along(dir, a_frustum.fFar, point);
		negate(dir, normal);
		plane(1, normal, point);
		if (!a_frustum.bOrtho) {
			// The side planes through the eye: each normal a sum of the side axis and the view direction, scaled by 1 / sqrt(t*t + 1).
			auto side = [&](std::uint32_t a_index, const float (&a_axis)[3], float a_axisScale, float a_dirScale) {
				for (std::uint32_t i = 0; i < 3; ++i)
					normal[i] = a_axis[i] * a_axisScale + dir[i] * a_dirScale;
				plane(a_index, normal, eye);
			};
			float s = 1.0f / std::sqrt(a_frustum.fLeft * a_frustum.fLeft + 1.0f);
			side(2, right, s, -(a_frustum.fLeft * s));
			s = 1.0f / std::sqrt(a_frustum.fRight * a_frustum.fRight + 1.0f);
			side(3, right, -s, a_frustum.fRight * s);
			s = 1.0f / std::sqrt(a_frustum.fTop * a_frustum.fTop + 1.0f);
			side(4, up, -s, a_frustum.fTop * s);
			s = 1.0f / std::sqrt(a_frustum.fBottom * a_frustum.fBottom + 1.0f);
			side(5, up, s, -(a_frustum.fBottom * s));
		} else {
			// Orthographic: the side planes face inward through the eye moved along their axis to the frustum's edge.
			along(right, a_frustum.fLeft, point);
			plane(2, right, point);
			along(right, a_frustum.fRight, point);
			negate(right, normal);
			plane(3, normal, point);
			along(up, a_frustum.fTop, point);
			negate(up, normal);
			plane(4, normal, point);
			along(up, a_frustum.fBottom, point);
			plane(5, up, point);
		}
		a_out.mask = 0x3F;  // NiCullingProcess::SetFrustum
	}

	void SunViews::CullPlanes(const RE::NiCullingProcess* a_process, const RE::NiCamera& a_camera, Planes& a_out)
	{
		// Process2: SetFrustum through the camera, then the custom planes copied over all of it (the six planes and the active mask).
		if (a_process && a_process->doCustomCullPlanes) {
			const auto& planes = a_process->customCullPlanes;
			for (std::uint32_t p = 0; p < 6; ++p)
				a_out.plane[p] = { planes.cullingPlanes[p].normal.x, planes.cullingPlanes[p].normal.y, planes.cullingPlanes[p].normal.z, planes.cullingPlanes[p].constant };
			a_out.mask = planes.activePlanes.underlying() & 0x3Fu;
			return;
		}
		FrustumPlanes(const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2().viewFrustum, a_camera.world, a_out);
	}

	void SunViews::CameraMatrices(const RE::NiCamera& a_camera, Cascade& a_out)
	{
		// FUN_140e57ae0 (AE), from FUN_140e58b80's arguments: the camera's rotation columns (direction, up, right) and its frustum.
		const auto& r = a_camera.world.rotate.entry;
		auto& v = a_out.view;
		v = { r[0][2], r[0][1], r[0][0], 0.0f, r[1][2], r[1][1], r[1][0], 0.0f, r[2][2], r[2][1], r[2][0], 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
		const auto& f = const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2().viewFrustum;
		const float one = 1.0f;
		const float rl = one / (f.fRight - f.fLeft);
		const float fn = one / (f.fFar - f.fNear);
		const float tb = one / (f.fTop - f.fBottom);
		auto& p = a_out.proj;
		p = {};
		p[0] = rl + rl;
		p[5] = tb + tb;
		const float x = -((f.fLeft + f.fRight) * rl);
		const float y = -(tb * (f.fBottom + f.fTop));
		if (!f.bOrtho) {
			p[8] = x;
			p[9] = y;
			p[10] = fn * f.fFar;
			p[14] = -(fn * (f.fNear * f.fFar));
			p[11] = one;
			p[15] = 0.0f;
		} else {
			p[12] = x;
			p[13] = y;
			p[10] = fn;
			p[14] = -(fn * f.fNear);
			p[11] = 0.0f;
			p[15] = one;
		}
		// FUN_1404a6ed0: each row of the product is (x * P0 + z * P2) + (y * P1 + w * P3), four lanes at once.
		for (std::uint32_t row = 0; row < 4; ++row) {
			const float* a = v.data() + row * 4;
			for (std::uint32_t c = 0; c < 4; ++c) {
				const float zx = a[2] * p[8 + c] + a[0] * p[c];
				const float wy = a[3] * p[12 + c] + a[1] * p[4 + c];
				a_out.viewProj[row * 4 + c] = zx + wy;
			}
		}
		a_out.eye = { a_camera.world.translate.x, a_camera.world.translate.y, a_camera.world.translate.z };
		// NiRect<float> (its members protected): left, right, top, bottom.
		const auto& port = const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2().port;
		static_assert(sizeof(port) == sizeof(float) * 4);
		std::memcpy(a_out.port.data(), &port, sizeof(float) * 4);
	}

	void SunViews::Block(const Cascade& a_cascade, std::byte* a_out)
	{
		std::memset(a_out, 0, kBlockBytes);
		auto* block = reinterpret_cast<float*>(a_out);
		auto transposed = [&](std::uint32_t a_register, const std::array<float, 16>& a_m) {
			for (std::uint32_t row = 0; row < 4; ++row)
				for (std::uint32_t c = 0; c < 4; ++c)
					block[a_register * 4 + row * 4 + c] = a_m[c * 4 + row];
		};
		transposed(0, a_cascade.view);
		transposed(4, a_cascade.proj);
		transposed(8, a_cascade.viewProj);
		transposed(12, a_cascade.viewProj);
		block[160] = a_cascade.eye[0];
		block[161] = a_cascade.eye[1];
		block[162] = a_cascade.eye[2];
	}

	std::array<std::uint32_t, 4> SunViews::Viewport(const std::array<float, 4>& a_port, std::uint32_t a_width, std::uint32_t a_height)
	{
		// Renderer::UpdateViewPort with the target's size: x = left * W, y = (1 - top) * H, width = (right - left) * W, height =
		// (top - bottom) * H; the capture truncates each to an integer as the viewport's floats.
		const float w = static_cast<float>(a_width), h = static_cast<float>(a_height);
		const float x = a_port[0] * w;
		const float y = (1.0f - a_port[2]) * h;
		const float width = (a_port[1] - a_port[0]) * w;
		const float height = (a_port[2] - a_port[3]) * h;
		return { static_cast<std::uint32_t>(std::max(0.0f, x)), static_cast<std::uint32_t>(std::max(0.0f, y)), static_cast<std::uint32_t>(width),
			static_cast<std::uint32_t>(height) };
	}

	bool SunViews::ParityEnabled()
	{
		return SwitchEnabled(Switch::SetParity) || SwitchEnabled(Switch::PersistentParity);
	}

	bool SunViews::Close(float a_a, float a_b)
	{
		return std::bit_cast<std::uint32_t>(a_a) == std::bit_cast<std::uint32_t>(a_b) || std::abs(a_a - a_b) <= 1e-5f * std::max(1.0f, std::abs(a_a));
	}

	bool SunViews::SamePlanes(const Planes& a_a, const Planes& a_b)
	{
		if (a_a.mask != a_b.mask)
			return false;
		for (std::uint32_t p = 0; p < 6; ++p) {
			if (!((a_a.mask >> p) & 1))
				continue;
			for (std::uint32_t i = 0; i < 4; ++i)
				if (!Close(a_a.plane[p][i], a_b.plane[p][i]))
					return false;
		}
		return true;
	}

	std::string SunViews::DescribePlanes(const Planes& a_mine, const Planes& a_engine)
	{
		std::string match, sets;
		for (std::uint32_t e = 0; e < 6; ++e) {
			int found = -1;
			for (std::uint32_t m = 0; m < 6 && found < 0; ++m) {
				bool same = true;
				for (std::uint32_t i = 0; i < 4; ++i)
					same = same && Close(a_mine.plane[m][i], a_engine.plane[e][i]);
				if (same)
					found = static_cast<int>(m);
			}
			match += found < 0 ? std::string("-") : std::to_string(found);
		}
		for (std::uint32_t p = 0; p < 6; ++p)
			sets += fmt::format(" [{}: ({:.4f} {:.4f} {:.4f} {:.1f}) / ({:.4f} {:.4f} {:.4f} {:.1f})]", p, a_mine.plane[p][0], a_mine.plane[p][1], a_mine.plane[p][2], a_mine.plane[p][3],
				a_engine.plane[p][0], a_engine.plane[p][1], a_engine.plane[p][2], a_engine.plane[p][3]);
		return fmt::format("the engine's planes are DCLF's {} (masks {:#x} {:#x});{}", match, a_mine.mask, a_engine.mask, sets);
	}

	void SunViews::Parity::Note(const char* a_what, float a_size, std::string a_detail)
	{
		largest = std::max(largest, a_size);
		// The first of each kind, up to four kinds.
		if (first.find(a_what) == std::string::npos && std::count(first.begin(), first.end(), '|') < 3)
			first += fmt::format("{}{}: {}", first.empty() ? "" : " | ", a_what, a_detail);
	}

	void SunViews::NoteDifference(const char* a_what, float a_size, std::string a_detail)
	{
		parity.Note(a_what, a_size, std::move(a_detail));
	}

	void SunViews::Report()
	{
		auto& p = parity;
		const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.cascadePlanes + p.cascadeCasters + p.fullFrustumPlanes + p.missing;
		logger::info("[DCLF] sun views (T2a: DCLF's, from the cameras UpdateCamera set up, against the engine's): {} views ({} view-projection, {} eye, {} viewport, "
					 "{} caster volume differ), {} cascades ({} planes, {} caster planes differ), {} full-frustum processes ({} differ), {} without DCLF's{}{}",
			p.views, p.viewProj, p.eye, p.viewport, p.casters, p.cascades, p.cascadePlanes, p.cascadeCasters, p.fullFrusta, p.fullFrustumPlanes, p.missing,
			differ ? " <- SUN VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		p = {};
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
		Frame& next = frame;
		next.sceneFrame = SceneStore::Get().GetFrame();
		next.valid = false;
		next.cascades = {};
		const auto& descriptors = light.GetRuntimeData().shadowmapDescriptors;
		next.count = std::min<std::uint32_t>(descriptors.size(), kMaxCascades);
		for (std::uint32_t d = 0; d < next.count; ++d) {
			const auto& descriptor = descriptors[d];
			const auto* camera = descriptor.camera.get();
			if (!camera)
				continue;
			auto& cascade = next.cascades[d];
			CameraMatrices(*camera, cascade);
			FrustumPlanes(const_cast<RE::NiCamera*>(camera)->GetRuntimeData2().viewFrustum, camera->world, cascade.frustum);
			// The caster volume UpdateCamera has just written to the cascade's culling process (FUN_141514c10): its output, not a cull's.
			const auto* process = static_cast<const RE::NiCullingProcess*>(descriptor.cullingProcess);
			if (process && process->doCustomCullPlanes)
				CullPlanes(process, *camera, cascade.caster);
			CullPlanes(process, *camera, cascade.cull);
			cascade.valid = true;
		}
		// Each process of the full-frustum cull culls through the full-frustum camera (FUN_141511f30 sets it as theirs).
		auto& runtime = light.GetShadowDirectionalLightRuntimeData();
		next.fullFrusta.clear();
		if (const auto* camera = runtime.fullFrustumCamera.get())
			for (const auto& process : runtime.fullFrustumCullingProcessArray)
				if (process)
					CullPlanes(process.get(), *camera, next.fullFrusta.emplace_back());
		next.valid = true;
		if (ParityEnabled() && ++reportFrames >= 300) {
			reportFrames = 0;
			Report();
		}
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
