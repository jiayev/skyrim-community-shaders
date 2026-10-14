// Billboards: the billboard row (SampleRow, FrameValues' producer inside the engine-read window) and the CPU port of the engine's turn
// (Orient, for the diagnostics).

#include "Features/DrawcallLimitFix/Engine/Capture/Billboards.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>

namespace DCLF::Billboards
{
	namespace
	{
		// 0x140d3e650's and FUN_140d3f240's constants, by their bits: DAT_141ad27f8 (an axis or a distance this short is zero),
		// DAT_141ad280c (a centre mode's camera this close to the pivot leaves the node unturned), DAT_14193aca4 (the camera's axis
		// already this aligned with the direction to the pivot: no turn), DAT_141ad28ac (pi: the angle of opposite directions), DAT_1419afb08
		// (the up modes' horizontal distance below which the node is left unturned).
		const float kShort = std::bit_cast<float>(0x358637bdu);
		const float kNear = std::bit_cast<float>(0x3a83126fu);
		const float kAligned = std::bit_cast<float>(0x3f7fffefu);
		const float kPi = std::bit_cast<float>(0x40490fdbu);
		const float kFlat = std::bit_cast<float>(0x2b8cbcccu);

		struct V3
		{
			float x, y, z;
		};
		V3 Load(const float (&a_v)[3]) { return { a_v[0], a_v[1], a_v[2] }; }
		// R0^T v: a world vector in the node's base frame (0x140d3e650's sums, in its order).
		V3 ToBase(const float (&a_r)[3][3], const V3& a_v)
		{
			return { a_r[1][0] * a_v.y + a_r[0][0] * a_v.x + a_r[2][0] * a_v.z, a_r[1][1] * a_v.y + a_r[0][1] * a_v.x + a_r[2][1] * a_v.z,
				a_r[1][2] * a_v.y + a_r[0][2] * a_v.x + a_r[2][2] * a_v.z };
		}

		/**
		 * @brief FUN_140d3f240: the camera's axes for the centre modes (ALWAYS_FACE_CENTER, RIGID_FACE_CENTER), a_toward (-forward), a_up and
		 * a_right turned by the rotation that takes -forward onto the direction from the pivot to the camera. False (the node left unturned)
		 * when the camera is within kNear (squared) of the pivot.
		 */
		bool CentreAxes(const Camera& a_camera, const float (&a_pivot)[3], V3& a_toward, V3& a_up, V3& a_right)
		{
			float dx = a_camera.eye[0] - a_pivot[0], dy = a_camera.eye[1] - a_pivot[1], dz = a_camera.eye[2] - a_pivot[2];
			float length = dy * dy + dx * dx + dz * dz;
			if (length < kNear)
				return false;
			length = std::sqrt(length);
			if (length <= kShort) {
				dx = dy = dz = 0.0f;
			} else {
				const float inverse = 1.0f / length;
				dx *= inverse;
				dy *= inverse;
				dz *= inverse;
			}
			a_toward = { -a_camera.forward[0], -a_camera.forward[1], -a_camera.forward[2] };
			a_up = Load(a_camera.up);
			a_right = Load(a_camera.right);
			const float cosine = a_toward.x * dx + a_toward.y * dy + a_toward.z * dz;
			if (cosine >= kAligned)
				return true;
			const float angle = -1.0f < cosine ? (1.0f <= cosine ? 0.0f : std::acos(cosine)) : kPi;
			float nx = a_toward.z * dy - a_toward.y * dz, ny = a_toward.x * dz - a_toward.z * dx, nz = a_toward.y * dx - a_toward.x * dy;
			const float n = std::sqrt(ny * ny + nx * nx + nz * nz);
			if (n <= kShort) {
				nx = ny = nz = 0.0f;
			} else {
				const float inverse = 1.0f / n;
				nx *= inverse;
				ny *= inverse;
				nz *= inverse;
			}
			const float s = std::sin(angle), c = std::cos(angle), k = 1.0f - c;
			a_toward = { dx, dy, dz };
			const float xy = ny * nx * k, xz = nz * nx * k, yz = nz * ny * k;
			const float xyPlus = nz * s + xy, xyMinus = xy - nz * s, xzMinus = xz - ny * s, xzPlus = ny * s + xz;
			const float yzPlus = nx * s + yz, yzMinus = yz - nx * s;
			const float xx = nx * nx * k + c, yy = ny * ny * k + c, zz = nz * nz * k + c;
			for (V3* v : { &a_up, &a_right }) {
				const V3 o = *v;
				*v = { o.y * xyPlus + o.x * xx + o.z * xzMinus, o.x * xyMinus + o.y * yy + o.z * yzPlus, o.y * yzMinus + o.x * xzPlus + o.z * zz };
			}
			return true;
		}

		void Multiply(const float (&a_l)[3][3], const float (&a_r)[3][3], float (&a_out)[3][3])
		{
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					a_out[i][j] = a_l[i][0] * a_r[0][j] + a_l[i][1] * a_r[1][j] + a_l[i][2] * a_r[2][j];
		}

	}

	std::uint32_t FaceMode(const void* a_node)
	{
		return Engine::At<std::uint16_t>(a_node, 0x128) & 7u;
	}

	bool SampleRow(const RE::BSGeometry& a_geometry, BindlessPlacement& a_out)
	{
		static const REL::Relocation<const RE::NiRTTI*> effectRtti{ RE::BSEffectShaderProperty::Ni_RTTI };
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		if (!property || property->GetRTTI() != effectRtti.get())
			return false;
		// The leaf relative to the nearest billboard node: the locals between them, composed as the children's world updates compose them
		// (NiAVObject's world = the parent's world * the local).
		RE::NiTransform relative = a_geometry.local;
		const RE::NiNode* node = a_geometry.parent;
		while (node && !netimmerse_cast<RE::NiBillboardNode*>(const_cast<RE::NiNode*>(node))) {
			relative = node->local * relative;
			node = node->parent;
		}
		if (!node)
			return false;
		// The node's base, as 0x140d3e650 rebuilds it before turning it: its parent's world times its local (the identity without a parent).
		const RE::NiTransform base = node->parent ? node->parent->world * node->local : node->local;
		// The unturned world (the node's turn the identity): a world transform like any row's, so a pass that does not turn the leaf still
		// draws it where the node stands.
		const RE::NiTransform unturned = base * relative;
		const auto& r = unturned.rotate.entry;
		const float scale = unturned.scale;
		const float t[3]{ unturned.translate.x, unturned.translate.y, unturned.translate.z };
		for (int row = 0; row < 3; ++row) {
			a_out.world[row * 4 + 0] = r[row][0] * scale;
			a_out.world[row * 4 + 1] = r[row][1] * scale;
			a_out.world[row * 4 + 2] = r[row][2] * scale;
			a_out.world[row * 4 + 3] = t[row];
			a_out.previousWorld[row * 4 + 0] = base.rotate.entry[row][0];
			a_out.previousWorld[row * 4 + 1] = base.rotate.entry[row][1];
			a_out.previousWorld[row * 4 + 2] = base.rotate.entry[row][2];
			a_out.previousWorld[row * 4 + 3] = 0.0f;
		}
		// The leaf's bound: its model bound by the unturned world (its centre's distance from the pivot does not change as the node turns).
		const auto& model = a_geometry.GetModelData().modelBound;
		float offset = 0.0f;
		for (int row = 0; row < 3; ++row) {
			const float c = (r[row][0] * model.center.x + r[row][1] * model.center.y + r[row][2] * model.center.z) * scale + t[row] -
			                (row == 0 ? base.translate.x : row == 1 ? base.translate.y : base.translate.z);
			offset += c * c;
		}
		const float radius = model.radius * scale;
		a_out.previousWorld[3] = radius;
		a_out.bound[0] = base.translate.x;
		a_out.bound[1] = base.translate.y;
		a_out.bound[2] = base.translate.z;
		a_out.bound[3] = std::sqrt(offset) + radius;
		return true;
	}

	void Orient(const float (&a_base)[3][3], const float (&a_pivot)[3], std::uint32_t a_mode, const Camera& a_camera, float (&a_out)[3][3])
	{
		float m[3][3]{ { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
		float base[3][3];
		std::copy_n(&a_base[0][0], 9, &base[0][0]);
		switch (a_mode) {
		case 0:  // ALWAYS_FACE_CAMERA
		case 3:  // ALWAYS_FACE_CENTER
			{
				V3 toward{ -a_camera.forward[0], -a_camera.forward[1], -a_camera.forward[2] }, up = Load(a_camera.up), right = Load(a_camera.right);
				if (a_mode == 3 && !CentreAxes(a_camera, a_pivot, toward, up, right))
					break;
				const V3 a = ToBase(base, toward), u = ToBase(base, up), rr = ToBase(base, right);
				// Turned about the toward axis until the right axis has no base-frame y.
				const float length = std::sqrt(rr.y * rr.y + u.y * u.y);
				if (length <= kShort) {
					m[0][0] = -rr.x;
					m[1][0] = -rr.y;
					m[2][0] = -rr.z;
					m[0][1] = -u.x;
					m[1][1] = -u.y;
					m[2][1] = -u.z;
				} else {
					const float inverse = 1.0f / length;
					const float c = u.y * inverse, s = -(rr.y * inverse);
					m[0][0] = rr.x * c + u.x * s;
					m[0][1] = u.x * c - rr.x * s;
					m[1][0] = rr.y * c + u.y * s;
					m[1][1] = u.y * c - rr.y * s;
					m[2][0] = rr.z * c + u.z * s;
					m[2][1] = u.z * c - rr.z * s;
				}
				m[0][2] = a.x;
				m[1][2] = a.y;
				m[2][2] = a.z;
				break;
			}
		case 1:  // ROTATE_ABOUT_UP: about the base's y, toward the camera (the node's scale, which the engine divides by, cancels)
			{
				const V3 d = ToBase(base, { a_camera.eye[0] - a_pivot[0], a_camera.eye[1] - a_pivot[1], a_camera.eye[2] - a_pivot[2] });
				const float length = std::sqrt(d.z * d.z + d.x * d.x);
				if (kFlat <= length) {
					const float inverse = 1.0f / length;
					m[0][0] = d.z * inverse;
					m[0][2] = d.x * inverse;
					m[2][0] = -(d.x * inverse);
					m[2][2] = d.z * inverse;
				}
				break;
			}
		case 2:  // RIGID_FACE_CAMERA
		case 4:  // RIGID_FACE_CENTER
			{
				V3 toward{ -a_camera.forward[0], -a_camera.forward[1], -a_camera.forward[2] }, up = Load(a_camera.up), right = Load(a_camera.right);
				if (a_mode == 4 && !CentreAxes(a_camera, a_pivot, toward, up, right))
					break;
				const V3 a = ToBase(base, toward), u = ToBase(base, up), rr = ToBase(base, right);
				m[0][0] = rr.x;
				m[1][0] = rr.y;
				m[2][0] = rr.z;
				m[0][1] = u.x;
				m[1][1] = u.y;
				m[2][1] = u.z;
				m[0][2] = a.x;
				m[1][2] = a.y;
				m[2][2] = a.z;
				break;
			}
		case 5:  // BSROTATE_ABOUT_UP: the base's rotation dropped, about the world's z toward the camera
			{
				for (int i = 0; i < 3; ++i)
					for (int j = 0; j < 3; ++j)
						base[i][j] = i == j ? 1.0f : 0.0f;
				const float dx = a_camera.eye[0] - a_pivot[0], dy = a_camera.eye[1] - a_pivot[1];
				const float length = std::sqrt(dy * dy + dx * dx);
				if (kFlat <= length) {
					const float inverse = 1.0f / length;
					m[0][0] = -(dy * inverse);
					m[0][1] = dx * inverse;
					m[1][0] = dx * inverse;
					m[1][1] = dy * inverse;
				}
				break;
			}
		default:
			break;
		}
		Multiply(base, m, a_out);
	}
}
