// PortalViews: the main camera's and the shadow lights' portal and occluder programs, built on the render thread. Each function names
// the engine function it ports (AE 1.6.1170); the arithmetic keeps the engine's grouping where it decides a comparison
// (skyrim-engine-notes.md, "Portals, rooms and occluders").

#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBytecode.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramModel.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalShapes.h"
#include "Features/DrawcallLimitFix/Features/Portals/RoomVisibility.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"
#include <Tracy/Tracy.hpp>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>

namespace DCLF
{
	namespace
	{
		using namespace Engine;
		using namespace PortalShapes;

		// BSParabolicCullingProcess::TestBaseVisibility2: a shadow light room process's portal test (vfunc 0xD8).
		constexpr Engine::Offset kParabolicPortalTest{ 0x151a120, 0x132c890 };

		/** @brief The ceiling as the box silhouette takes it: cvttss2si, then up by one unless exact or negative (INT_MIN saturates). */
		std::int32_t Ceil(float a_value)
		{
			std::int32_t t = std::isfinite(a_value) && std::abs(a_value) < 2147483520.0f ? static_cast<std::int32_t>(a_value) : std::numeric_limits<std::int32_t>::min();
			if (t != std::numeric_limits<std::int32_t>::min() && static_cast<float>(t) != a_value)
				t += std::signbit(a_value) ? 0 : 1;
			return t;
		}

		/** @brief FUN_140e19490 / FUN_140e1a030: a quickhull of eight integer points, evaluated in float. */
		struct Hull
		{
			struct Node
			{
				std::uint32_t index = 0, next = 0;
			};
			std::array<Node, 16> nodes{};
			std::uint32_t used = 0;
			const std::array<std::array<std::int32_t, 2>, 8>* points = nullptr;

			struct Line
			{
				float a, b, c;
				float Side(const std::array<std::int32_t, 2>& p) const { return (static_cast<float>(p[0]) * a + static_cast<float>(p[1]) * b) - c; }
			};
			Line Through(const std::array<std::int32_t, 2>& a_from, const std::array<std::int32_t, 2>& a_to) const
			{
				const float a = static_cast<float>(a_from[1]) - static_cast<float>(a_to[1]);
				const float b = static_cast<float>(a_to[0]) - static_cast<float>(a_from[0]);
				return { a, b, static_cast<float>(a_from[0]) * a + b * static_cast<float>(a_from[1]) };
			}
			void Recurse(std::uint32_t a_from, std::uint32_t a_to, const Line& a_line, const std::vector<std::uint32_t>& a_set)
			{
				const auto& p = *points;
				std::size_t farthest = 0;
				float best = std::abs(a_line.Side(p[a_set[0]]));
				for (std::size_t i = 1; i < a_set.size(); ++i)
					if (const float d = std::abs(a_line.Side(p[a_set[i]])); d > best)
						best = d, farthest = i;
				if (used >= nodes.size())
					return;
				const std::uint32_t k = used++;
				nodes[k] = { a_set[farthest], a_to };
				nodes[a_from].next = k;
				if (a_set.size() <= 1)
					return;
				const auto& f = p[a_set[farthest]];
				const Line l1 = Through(p[nodes[a_from].index], f);
				const Line l2 = Through(f, p[nodes[a_to].index]);
				std::vector<std::uint32_t> s1, s2;
				for (std::size_t i = 0; i < a_set.size(); ++i) {
					if (i == farthest)
						continue;
					if (l1.Side(p[a_set[i]]) > 0.0f)
						s1.push_back(a_set[i]);
					else if (l2.Side(p[a_set[i]]) > 0.0f)
						s2.push_back(a_set[i]);
				}
				if (!s1.empty())
					Recurse(a_from, k, l1, s1);
				if (!s2.empty())
					Recurse(k, a_to, l2, s2);
			}
			std::uint32_t Build(const std::array<std::array<std::int32_t, 2>, 8>& a_points, std::array<std::uint32_t, 8>& a_out)
			{
				points = &a_points;
				const auto& p = a_points;
				nodes[0].index = nodes[1].index = 0;
				for (std::uint32_t i = 1; i < 8; ++i) {
					if (p[i][0] < p[nodes[0].index][0])
						nodes[0].index = i;
					if (p[i][0] > p[nodes[1].index][0])
						nodes[1].index = i;
				}
				nodes[0].next = 1;
				nodes[1].next = 0;
				used = 2;
				const Line line = Through(p[nodes[0].index], p[nodes[1].index]);
				std::vector<std::uint32_t> upper, lower;
				for (std::uint32_t i = 0; i < 8; ++i) {
					if (i == nodes[0].index || i == nodes[1].index)
						continue;
					const float side = line.Side(p[i]);
					if (side > 0.0f)
						upper.push_back(i);
					else if (side < 0.0f)
						lower.push_back(i);
				}
				if (!upper.empty())
					Recurse(0, 1, line, upper);
				if (!lower.empty())
					Recurse(1, 0, line, lower);
				std::uint32_t count = 0;
				a_out[count++] = nodes[0].index;
				for (std::uint32_t n = nodes[0].next; n != 0 && count < 9; n = nodes[n].next) {
					if (count >= a_out.size())
						return count + 1;  // more than eight: past any limit the caller takes
					a_out[count++] = nodes[n].index;
				}
				return count;
			}
		};

		// The box's faces (0x1419d2680): two triangles each; the plane is the first three, the quad (t0, t1, t2, t5).
		constexpr std::array<std::array<std::uint32_t, 6>, 6> kBoxFaces{ { { 0, 1, 2, 0, 2, 3 }, { 5, 1, 0, 5, 0, 4 }, { 3, 2, 6, 3, 6, 7 }, { 6, 5, 4, 4, 6, 7 },
			{ 0, 3, 7, 0, 7, 4 }, { 2, 1, 5, 2, 5, 6 } } };

		/** @brief Calls a virtual of the engine's: slot a_slot of a_object's vtable, with a pointer argument. */
		template <class R, class A>
		R CallVirtual(const void* a_object, std::size_t a_slot, const A* a_argument)
		{
			using Fn = R (*)(const void*, const A*);
			return (*reinterpret_cast<Fn const* const*>(a_object))[a_slot](a_object, a_argument);
		}
	}

	PortalViews& PortalViews::Get()
	{
		static PortalViews instance;
		return instance;
	}

	const PortalViews::Program* PortalViews::RoomProgram(const void* a_room) const
	{
		const auto index = FindRoom(a_room);
		return index >= 0 ? &frame.rooms[index] : nullptr;
	}

	std::int32_t PortalViews::FindRoom(const void* a_room) const
	{
		// The visibility map's lookup: the first inserted under the key.
		for (std::size_t i = 0; i < frame.roomKeys.size(); ++i)
			if (frame.roomKeys[i] == a_room)
				return static_cast<std::int32_t>(i);
		return -1;
	}

	std::uint32_t PortalViews::AddRoom(const void* a_room)
	{
		frame.roomKeys.push_back(a_room);
		frame.rooms.emplace_back();
		return static_cast<std::uint32_t>(frame.rooms.size() - 1);
	}

	// ---- BSCompoundFrustum's builders ----

	void PortalViews::Reset(Program& a_program) const
	{
		PortalModel::Reset(a_program, { camera.eye.x, camera.eye.y, camera.eye.z });
	}

	void PortalViews::CopyMasked(Program& a_out, const Program& a_parent, const void* a_portal)
	{
		// FUN_140e33740: a set the portal lies wholly inside with no edge clipped is turned off for the child room.
		PortalModel::CopyMasked(a_out, a_parent, [&](const PlaneSet& a_set) {
			const bool inside = PortalVsSetEdges(a_portal, a_set);
			const auto& f = edgeFlags[a_portal];
			return inside && f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0;
		});
	}

	PortalViews::Quad PortalViews::Corners(const void* a_quad)
	{
		// BSPortal::sub (0x140e230c0): A = hx * column 0, B = hy * column 2.
		const float cx = At<float>(a_quad, kShapeCentre), cy = At<float>(a_quad, kShapeCentre + 4), cz = At<float>(a_quad, kShapeCentre + 8);
		const float hx = At<float>(a_quad, kShapeHalfX), hy = At<float>(a_quad, kShapeHalfY);
		const float ax = hx * M(a_quad, 0, 0), ay = hx * M(a_quad, 1, 0), az = hx * M(a_quad, 2, 0);
		const float bx = hy * M(a_quad, 0, 2), by = hy * M(a_quad, 1, 2), bz = hy * M(a_quad, 2, 2);
		const float sx = bx + ax, sy = by + ay, sz = bz + az;
		const float dx = ax - bx, dy = ay - by, dz = az - bz;
		return { Vec3{ sx + cx, sy + cy, sz + cz }, Vec3{ dx + cx, dy + cy, dz + cz }, Vec3{ cx - sx, cy - sy, cz - sz }, Vec3{ cx - dx, cy - dy, cz - dz } };
	}

	PortalViews::PlaneSet PortalViews::PortalPlanes(const void* a_quad, const Vec3& a_eye)
	{
		// FUN_140e233b0: the quad's plane (the camera on its non-positive side), its four edges through the camera; slot 1 the zero
		// plane, off; a clipped edge's plane off.
		const auto c = Corners(a_quad);
		PlaneSet out;
		out.mask = 0x3F;
		const auto plane = [](const Vec3& a, const Vec3& b, const Vec3& d) { return PlaneFromPoints(a.x, a.y, a.z, b.x, b.y, b.z, d.x, d.y, d.z); };
		const auto p = plane(c[0], c[1], c[2]);
		if (Dist(p, a_eye.x, a_eye.y, a_eye.z) <= 0.0f) {
			out.plane[0] = p;
			out.plane[2] = plane(c[0], c[1], a_eye);
			out.plane[3] = plane(c[2], c[3], a_eye);
			out.plane[4] = plane(c[3], c[0], a_eye);
			out.plane[5] = plane(c[1], c[2], a_eye);
		} else {
			out.plane[0] = plane(c[1], c[0], c[2]);
			out.plane[2] = plane(c[1], c[0], a_eye);
			out.plane[3] = plane(c[3], c[2], a_eye);
			out.plane[4] = plane(c[0], c[3], a_eye);
			out.plane[5] = plane(c[2], c[1], a_eye);
		}
		out.mask &= ~2u;
		const auto& f = edgeFlags[a_quad];
		if (f[0] == 1)
			out.mask &= ~0x04u;
		if (f[1] == 1)
			out.mask &= ~0x08u;
		if (f[2] == 1)
			out.mask &= ~0x10u;
		if (f[3] == 1)
			out.mask &= ~0x20u;
		return out;
	}

	void PortalViews::AddPortal(Program& a_program, const void* a_portal)
	{
		// FUN_140e32710: a portal test against the portal's planes from the camera, all six on (off within 5 units of its plane).
		const auto c = Corners(a_portal);
		const auto p = PlaneFromPoints(c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z);
		const std::uint32_t s = a_program.freePlane;
		EmitTest(a_program, kPortalTest, s);
		const Vec3 eye{ a_program.eye[0], a_program.eye[1], a_program.eye[2] };
		auto& set = Grow(a_program, s);
		set = PortalPlanes(a_portal, eye);
		set.mask = std::abs(((p[1] * eye.y + p[0] * eye.x) + p[2] * eye.z) - p[3]) <= 5.0f ? 0u : 0x3Fu;
		++a_program.freePlane;
		++programCounts.portals;
	}

	void PortalViews::AddPlane(Program& a_program, const void* a_plane)
	{
		// FUN_140e32a40: an occlusion plane, or with an accepted neighbour on the same side the linked pair.
		const void* links[4];
		for (std::size_t i = 0; i < 4; ++i)
			links[i] = At<const void*>(a_plane, kPlaneLinks[i]);
		const std::uint32_t type = (links[0] || links[2]) ? 2u : (links[1] || links[3]) ? 1u :
		                                                                                  0u;
		const void* neighbour = type == 1 ? (links[1] ? links[1] : links[3]) : (links[0] ? links[0] : links[2]);
		const Vec3 eye{ a_program.eye[0], a_program.eye[1], a_program.eye[2] };
		if (neighbour) {
			const auto side = [&](const void* a_quad) {
				const auto c = Corners(a_quad);
				const auto p = PlaneFromPoints(c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z);
				const float d = Dist(p, eye.x, eye.y, eye.z);
				return d < 0.0f ? 2 : d > 0.0f ? 1 :
				                                 0;
			};
			const int s1 = side(a_plane), s2 = side(neighbour);
			if (states[neighbour] == 1 && s1 == s2) {
				if (type == 2)
					return;
				const std::uint32_t s = a_program.freePlane;
				Emit(a_program, kAnd);
				EmitTest(a_program, kOccluderTest, s);
				EmitTest(a_program, kOccluderTest, s + 1);
				Emit(a_program, kOr);
				EmitTest(a_program, kOccluderTest, s + 2);
				EmitTest(a_program, kOccluderTest, s + 3);
				Emit(a_program, kClose);
				Emit(a_program, kClose);
				Grow(a_program, s + 3);
				const void* other = links[1] ? links[1] : links[3];
				a_program.sets[s] = PortalPlanes(a_plane, eye);
				a_program.sets[s + 2] = PortalPlanes(a_plane, eye);
				a_program.sets[s + 2].mask &= ~(links[1] ? 0x04u : 0x08u);
				a_program.sets[s + 1] = PortalPlanes(other, eye);
				a_program.sets[s + 3] = PortalPlanes(other, eye);
				a_program.sets[s + 3].mask &= ~(links[1] ? 0x08u : 0x10u);
				a_program.freePlane += 4;
				programCounts.planes += 2;
				return;
			}
		}
		const std::uint32_t s = a_program.freePlane;
		EmitTest(a_program, kOccluderTest, s);
		Grow(a_program, s) = PortalPlanes(a_plane, eye);
		++a_program.freePlane;
		++programCounts.planes;
	}

	bool PortalViews::AddBox(Program& a_program, const void* a_box)
	{
		// FUN_140e32fe0: OR(not wholly inside set 0, not wholly inside set 1); both sets copied, counted only when the box is valid.
		auto& scratch = boxes[a_box];
		const bool ok = BoxSilhouette(a_box, scratch);
		const std::uint32_t s = a_program.freePlane;
		Grow(a_program, s + 1);
		a_program.sets[s] = scratch.set[0];
		a_program.sets[s + 1] = scratch.set[1];
		if (ok) {
			if (a_program.ops.size() < a_program.freeOp + 8)
				a_program.ops.resize(a_program.freeOp + 16);
			auto& ops = a_program.ops;
			auto& n = a_program.freeOp;
			ops[n++].word = kOr;
			ops[n++].word = kOccluderTest;
			ops[n++].word = s;
			ops[n++].word = kOccluderTest;
			ops[n++].word = s + 1;
			ops[n++].word = kClose;
			a_program.freePlane += 2;
			++programCounts.boxes;
		}
		return ok;
	}

	void PortalViews::AddOccluderTop(Program& a_program, const void* a_occluder, bool& a_needOpen)
	{
		// FUN_140e32920: an occluder of the start room's or unbound space's frustum, opening the AND on the first one used.
		auto& state = states[a_occluder];
		if (!IsPlane(a_occluder)) {
			bool ok = false;
			if (state == 1 || (state == 0 && BoxInView(a_occluder))) {
				if (a_needOpen)
					Emit(a_program, kAnd);
				const std::uint32_t before = a_program.freeOp;
				ok = AddBox(a_program, a_occluder);
				if (a_needOpen) {
					if (a_program.freeOp == before)
						--a_program.freeOp;
					else
						a_needOpen = false;
				}
			}
			states[a_occluder] = ok ? 1 : 2;
			return;
		}
		if (state == 2)
			return;
		// A graph plane is judged at Build (PlaneJudged); an untested one here is the start room's, judged the same way rather than by
		// the walk's WithinFrustumDistFirst, whose rectangle bug takes planes wholly outside the view (skyrim-engine-notes.md).
		if (state == 0 && !(engineWalkTest ? WithinFrustumDistFirst(a_occluder, camera.view, camera.eye) : PlaneJudged(a_occluder)))
			return;
		if (a_needOpen) {
			Emit(a_program, kAnd);
			a_needOpen = false;
		}
		AddPlane(a_program, a_occluder);
	}

	PortalViews::Program PortalViews::UnboundWithEngineStates(const void* a_graph)
	{
		const auto savedStates = states;
		const auto savedBoxes = boxes;
		const auto savedFlags = edgeFlags;
		const auto savedCounts = programCounts;
		ForList(a_graph, kGraphOccluders, [&](const void* a_occluder) {
			if (!a_occluder)
				return;
			states[a_occluder] = At<std::uint32_t>(a_occluder, 0x40);
			// A plane the engine's other tests judged keeps the edge flags their test wrote (+0xF0): the walk takes it untested.
			if (IsPlane(a_occluder) && states[a_occluder] != 0)
				std::memcpy(edgeFlags[a_occluder].data(), reinterpret_cast<const std::byte*>(a_occluder) + 0xF0, 4);
		});
		Program program;
		Reset(program);
		bool needOpen = true;
		engineWalkTest = true;
		ForList(a_graph, kGraphOccluders, [&](const void* a_occluder) {
			if (a_occluder)
				AddOccluderTop(program, a_occluder, needOpen);
		});
		engineWalkTest = false;
		if (!needOpen)
			Emit(program, kClose);
		Finalize(program);
		states = savedStates;
		boxes = savedBoxes;
		edgeFlags = savedFlags;
		programCounts = savedCounts;
		return program;
	}

	void PortalViews::AddOccluderRoom(Program& a_program, const void* a_occluder, const Program* a_parent)
	{
		// FUN_140e32860: an occluder of a room reached through a portal. A plane only once accepted this frame: the engine resets
		// every state before its walk and nothing in the walk accepts a plane, so a room's own planes take no part; the graph's take
		// part where Build's judgment (the actor culler's) accepted them, for unbound space reached through a portal.
		if (!IsPlane(a_occluder)) {
			bool ok = false;
			if (!a_parent || TestShapeProgram(*a_parent, a_occluder))
				ok = AddBox(a_program, a_occluder);
			states[a_occluder] = ok ? 1 : 2;
			return;
		}
		if (states[a_occluder] != 1)
			return;
		if (a_parent && !TestQuadProgram(*a_parent, a_occluder))
			return;
		AddPlane(a_program, a_occluder);
	}

	// ---- The shapes' tests ----

	bool PortalViews::QuadVsSet(const PlaneSet& a_set, const Quad& a_quad, Flags& a_flags)
	{
		// FUN_140e164e0: whether the quad meets the frustum, and which edges are clipped (written unless wholly outside a plane).
		std::array<std::uint32_t, 4> outside{};
		for (std::uint32_t p = 0; p < 6; ++p) {
			if (!((a_set.mask >> p) & 1))
				continue;
			std::uint32_t count = 0;
			for (std::uint32_t j = 0; j < 4; ++j)
				if (Dist(a_set.plane[p], a_quad[j].x, a_quad[j].y, a_quad[j].z) < 0.0f) {
					outside[j] |= 1u << p;
					++count;
				}
			if (count == 4)
				return false;
		}
		constexpr std::array<std::array<std::uint32_t, 2>, 4> kEdges{ { { 0, 1 }, { 3, 2 }, { 0, 3 }, { 2, 1 } } };
		for (std::uint32_t e = 0; e < 4; ++e) {
			const auto& a = a_quad[kEdges[e][0]];
			const auto& b = a_quad[kEdges[e][1]];
			const std::uint32_t oa = outside[kEdges[e][0]], ob = outside[kEdges[e][1]];
			std::uint8_t flag = 0;
			if (oa & ob) {
				flag = 1;
			} else {
				const std::uint32_t x = oa ^ ob;
				bool visible = x == 0;
				for (std::uint32_t p = 0; p < 6 && !visible; ++p) {
					if (!((a_set.mask & x) >> p & 1))
						continue;
					const auto& n = a_set.plane[p];
					const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
					const float den = (n[0] * dx + n[1] * dy) + n[2] * dz;
					if (den == 0.0f)
						continue;
					const float t = (((n[0] * n[3] - a.x) * n[0] + (n[1] * n[3] - a.y) * n[1]) + (n[2] * n[3] - a.z) * n[2]) / den;
					if (!(0.0f <= t && t <= 1.0f))
						continue;
					const float hx = dx * t + a.x, hy = dy * t + a.y, hz = dz * t + a.z;
					visible = true;
					for (std::uint32_t q = 0; q < 6; ++q)
						if (q != p && ((a_set.mask >> q) & 1) && Dist(a_set.plane[q], hx, hy, hz) < 0.0f)
							visible = false;
				}
				flag = visible ? 0 : 1;
			}
			a_flags[e] = flag;
		}
		if (a_flags[0] == 0 || a_flags[1] == 0 || a_flags[2] == 0 || a_flags[3] == 0)
			return true;
		// Every edge clipped: does the frustum's axis pierce the quad, or a diagonal cross a side plane inside the frustum?
		const auto& n0 = a_set.plane[0];
		const float p0x = n0[0] * n0[3], p0y = n0[1] * n0[3], p0z = n0[2] * n0[3];
		const float dirx = (p0x + n0[0]) - p0x, diry = (p0y + n0[1]) - p0y, dirz = (p0z + n0[2]) - p0z;
		const auto& q = a_quad;
		const auto normal = PlaneFromPoints(q[0].x, q[0].y, q[0].z, q[1].x, q[1].y, q[1].z, q[2].x, q[2].y, q[2].z);
		const float den = (normal[1] * diry + normal[0] * dirx) + normal[2] * dirz;
		if (den != 0.0f) {
			const float t = (((q[0].y - p0y) * normal[1] + (q[0].x - p0x) * normal[0]) + (q[0].z - p0z) * normal[2]) / den;
			if (t > 0.0f) {
				const float hx = dirx * t + p0x, hy = diry * t + p0y, hz = dirz * t + p0z;
				const float cx = (((q[1].x + q[0].x) + q[2].x) + q[3].x) * 0.25f, cy = (((q[1].y + q[0].y) + q[2].y) + q[3].y) * 0.25f,
							cz = (((q[1].z + q[0].z) + q[2].z) + q[3].z) * 0.25f;
				const auto unit = [](float x, float y, float z, float& length) {
					length = std::sqrt((y * y + x * x) + z * z);
					if (length <= 1e-6f)
						return std::array<float, 3>{ 0.0f, 0.0f, 0.0f };
					const float inverse = 1.0f / length;
					return std::array<float, 3>{ x * inverse, y * inverse, z * inverse };
				};
				float lu = 0.0f, lv = 0.0f;
				const auto u = unit(q[2].x - q[1].x, q[2].y - q[1].y, q[2].z - q[1].z, lu);
				const auto v = unit(q[1].x - q[0].x, q[1].y - q[0].y, q[1].z - q[0].z, lv);
				const float rx = hx - cx, ry = hy - cy, rz = hz - cz;
				if (std::abs((u[1] * ry + u[0] * rx) + u[2] * rz) <= lu * 0.5f && std::abs((v[1] * ry + v[0] * rx) + v[2] * rz) <= lv * 0.5f)
					return true;
			}
		}
		// FUN_140e16390 on the diagonals against the side planes.
		const auto crosses = [&](const Vec3& a, const Vec3& b, const std::array<float, 4>& pl) {
			const float den2 = (pl[0] * (b.x - a.x) + pl[1] * (b.y - a.y)) + pl[2] * (b.z - a.z);
			if (den2 == 0.0f)
				return false;
			const float t = (pl[3] - ((pl[0] * a.x + pl[1] * a.y) + pl[2] * a.z)) / den2;
			if (!(0.0f <= t && t <= 1.0f))
				return false;
			const float x = (b.x - a.x) * t + a.x, y = (b.y - a.y) * t + a.y, z = (b.z - a.z) * t + a.z;
			for (std::uint32_t p = 0; p < 6; ++p)
				if (((a_set.mask >> p) & 1) && !(Dist(a_set.plane[p], x, y, z) > -0.001f))
					return false;
			return true;
		};
		for (std::uint32_t p = 2; p < 6; ++p)
			if (crosses(q[0], q[2], a_set.plane[p]) || crosses(q[1], q[3], a_set.plane[p]))
				return true;
		return false;
	}

	bool PortalViews::AllCornersInside(const Quad& a_quad, const PlaneSet& a_set)
	{
		// FUN_140e23cf0.
		for (std::uint32_t p = 0; p < 6; ++p)
			if ((a_set.mask >> p) & 1)
				for (const auto& c : a_quad)
					if (Dist(a_set.plane[p], c.x, c.y, c.z) < 0.0f)
						return false;
		return true;
	}

	bool PortalViews::PortalVsSetEdges(const void* a_portal, const PlaneSet& a_set)
	{
		// FUN_140e23a50: false when the portal is wholly outside a plane; the edge flags from the corners' outside masks.
		const auto c = Corners(a_portal);
		std::array<std::uint32_t, 4> m{};
		bool result = true;
		for (std::uint32_t k = 0; k < 6; ++k) {
			if (!((a_set.mask >> k) & 1))
				continue;
			bool all = true;
			for (std::uint32_t j = 0; j < 4; ++j) {
				const bool out = Dist(a_set.plane[k], c[j].x, c[j].y, c[j].z) < 0.0f;
				m[j] |= (out ? 1u : 0u) << k;
				all = all && out;
			}
			if (all)
				result = false;
		}
		auto& f = edgeFlags[a_portal];
		f[0] = (m[1] & m[0]) != 0;
		f[1] = (m[3] & m[2]) != 0;
		f[2] = (m[3] & m[0]) != 0;
		f[3] = (m[2] & m[1]) != 0;
		return result;
	}

	bool PortalViews::WithinFrustumDistFirst(const void* a_quad, const PlaneSet& a_planes, const Vec3& a_eye)
	{
		// BSOcclusionPlane::WithinFrustumDistFirst (0x140e23e10): a camera within 5 units of the quad's plane and over it sees a
		// portal (not an occlusion plane); else the quad against the frustum, or the camera looking straight at it.
		const auto c = Corners(a_quad);
		const auto p = PlaneFromPoints(c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z);
		const float dP = ((a_eye.x * p[0] + a_eye.y * p[1]) + a_eye.z * p[2]) - p[3];
		const float px = p[0] * -dP + a_eye.x, py = p[1] * -dP + a_eye.y, pz = p[2] * -dP + a_eye.z;
		const auto& n0 = a_planes.plane[0];
		const float dN = ((n0[0] * a_eye.x + n0[1] * a_eye.y) + n0[2] * a_eye.z) - n0[3];
		const auto inRect = [&]() {
			const float rx = px - At<float>(a_quad, kShapeCentre), ry = py - At<float>(a_quad, kShapeCentre + 4), rz = pz - At<float>(a_quad, kShapeCentre + 8);
			const float u = (rx * M(a_quad, 1, 0) + ry * M(a_quad, 1, 1)) + rz * M(a_quad, 1, 2);
			const float v = (rx * M(a_quad, 2, 0) + ry * M(a_quad, 2, 1)) + rz * M(a_quad, 2, 2);
			return std::abs(u) <= At<float>(a_quad, kShapeHalfX) && std::abs(v) <= At<float>(a_quad, kShapeHalfY);
		};
		const float s = dP + dN;
		if (-5.0f <= s && s < 5.0f && inRect())
			return RttiIs(a_quad, "BSPortal");
		bool visible = QuadVsSet(a_planes, c, edgeFlags[a_quad]);
		if (!visible && dN < ((n0[0] * px + n0[1] * py) + n0[2] * pz) - n0[3])
			visible = inRect();
		return visible;
	}

	bool PortalViews::PlaneJudged(const void* a_plane)
	{
		// FUN_1414a8ba0 (HighActorCuller::Operate's occluder test, which judges every untested graph occluder before Main::Update's walk
		// takes them): FUN_140e23920, the corners relative to the eye against the camera-relative frustum (its edge flags written), and
		// FUN_140e242d0, a one-sided plane only with the eye on its negative side.
		const auto c = Corners(a_plane);
		const auto& e = camera.eye;
		Quad relative;
		for (std::size_t i = 0; i < 4; ++i)
			relative[i] = { c[i].x - e.x, c[i].y - e.y, c[i].z - e.z };
		const bool inView = QuadVsSet(camera.relative, relative, edgeFlags[a_plane]);
		if (At<std::uint8_t>(a_plane, kPlaneTwoSided) != 0)
			return inView;
		const auto p = PlaneFromPoints(c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z);
		return inView && Dist(p, e.x, e.y, e.z) < 0.0f;
	}

	bool PortalViews::TestQuadProgram(const Program& a_program, const void* a_quad)
	{
		// FUN_140e322e0: the program run on a quad (portal tests write its edge flags).
		if (a_program.freeOp == 0)
			return true;
		const auto corners = Corners(a_quad);
		std::uint32_t i = a_program.firstOp;
		for (std::uint32_t guard = 0; guard < 4096 && i < a_program.ops.size(); ++guard) {
			const std::uint32_t t = a_program.ops[i].word;
			if (t == 2 || t == 3)
				return t == 2;
			bool result = false;
			if (IsTest(t) && i + 1 < a_program.ops.size()) {
				const std::uint32_t s = a_program.ops[i + 1].word;
				const PlaneSet& set = s < a_program.sets.size() ? a_program.sets[s] : PlaneSet{};
				result = t == kPortalTest ? QuadVsSet(set, corners, edgeFlags[a_quad]) : !AllCornersInside(corners, set);
			}
			i = result ? a_program.ops[i].yes : a_program.ops[i].no;
		}
		return true;
	}

	bool PortalViews::TestShapeProgram(const Program& a_program, const void* a_box) const
	{
		// FUN_140e32400: the program run on a box's bound shape (its WithinFrustum, vfunc 0x29, and CompletelyWithinFrustum, 0x2A).
		const void* shape = At<const void*>(a_box, kBoxBound);
		if (a_program.freeOp == 0 || !shape)
			return true;
		std::uint32_t i = a_program.firstOp;
		for (std::uint32_t guard = 0; guard < 4096 && i < a_program.ops.size(); ++guard) {
			const std::uint32_t t = a_program.ops[i].word;
			if (t == 2 || t == 3)
				return t == 2;
			bool result = false;
			if (IsTest(t) && i + 1 < a_program.ops.size()) {
				const std::uint32_t s = a_program.ops[i + 1].word;
				const PlaneSet set = s < a_program.sets.size() ? a_program.sets[s] : PlaneSet{};
				result = t == kPortalTest ? CallVirtual<bool>(shape, 0x29, &set) : !CallVirtual<bool>(shape, 0x2A, &set);
			}
			i = result ? a_program.ops[i].yes : a_program.ops[i].no;
		}
		return true;
	}

	bool PortalViews::BoxInView(const void* a_box) const
	{
		// FUN_140e21750: not with the camera inside it (its shape's GetWithinPoint, vfunc 0x2C), not wholly outside a view plane, and
		// its extents overlapping the view frustum's eight corners projected on its axes.
		const void* shape = At<const void*>(a_box, kBoxBound);
		const RE::NiPoint3 eye{ camera.eye.x, camera.eye.y, camera.eye.z };
		if (shape && CallVirtual<bool>(shape, 0x2C, &eye))
			return false;
		const auto k = BoxCorners(a_box);
		for (std::uint32_t p = 0; p < 6; ++p) {
			if (!((camera.view.mask >> p) & 1))
				continue;
			bool all = true;
			for (const auto& c : k)
				all = all && ((camera.view.plane[p][0] * c.x + camera.view.plane[p][1] * c.y) + camera.view.plane[p][2] * c.z) - camera.view.plane[p][3] < 0.0f;
			if (all)
				return false;
		}
		const auto* cam = camera.camera;
		const auto r = [&](std::uint32_t a_row, std::uint32_t a_column) { return At<float>(cam, kCameraRotation + 4 * (3 * a_row + a_column)); };
		const float right = At<float>(cam, kCameraFrustum + 4), top = At<float>(cam, kCameraFrustum + 8);
		const float distances[2]{ At<float>(cam, kCameraFrustum + 0x10), At<float>(cam, kCameraFrustum + 0x14) };
		const V relative{ camera.eye.x - At<float>(a_box, kShapeCentre), camera.eye.y - At<float>(a_box, kShapeCentre + 4), camera.eye.z - At<float>(a_box, kShapeCentre + 8) };
		float lo[3]{ FLT_MAX, FLT_MAX, FLT_MAX }, hi[3]{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const float d : distances)
			for (const float su : { 1.0f, -1.0f })
				for (const float sr : { 1.0f, -1.0f }) {
					V p{};
					for (std::uint32_t i = 0; i < 3; ++i)
						(&p.x)[i] = ((r(i, 0) + right * r(i, 2) * sr) + top * r(i, 1) * su) * d + (&relative.x)[i];
					for (std::uint32_t a = 0; a < 3; ++a) {
						const float projected = (p.x * M(a_box, 0, a) + p.y * M(a_box, 1, a)) + p.z * M(a_box, 2, a);
						lo[a] = std::min(lo[a], projected);
						hi[a] = std::max(hi[a], projected);
					}
				}
		for (std::uint32_t a = 0; a < 3; ++a) {
			const float e = At<float>(a_box, 0x48 + 4 * a);
			if (e < lo[a] || hi[a] < -e)
				return false;
		}
		return true;
	}

	bool PortalViews::BoxSilhouette(const void* a_box, BoxScratch& a_scratch)
	{
		// FUN_140e21090: cached while the box's state is set this frame; else the silhouette's edge planes from the projected corners'
		// hull, then the faces turned away from the camera, refined against the camera-relative frustum.
		if (const auto it = states.find(a_box); it != states.end() && it->second != 0)
			return it->second == 1;
		const auto k = BoxCorners(a_box);
		const auto* cam = camera.camera;
		const float* m = &At<float>(cam, kCameraWorldToCam);
		const float vl = At<float>(cam, kCameraViewport), vr = At<float>(cam, kCameraViewport + 4), vt = At<float>(cam, kCameraViewport + 8),
					vb = At<float>(cam, kCameraViewport + 12);
		const auto rot = [&](std::uint32_t a_row, std::uint32_t a_column) { return At<float>(cam, kCameraRotation + 4 * (3 * a_row + a_column)); };
		const V forward{ rot(0, 0), rot(1, 0), rot(2, 0) };
		const float nearDistance = At<float>(cam, kCameraFrustum + 0x10);
		float x = 0.0f, y = 0.0f;
		const auto project = [&](const V& p) {
			// FUN_140d2c5b0 (WorldPtToScreen): on failure x, y keep the last corner's.
			const float w = std::abs(((p.x * m[12] + p.y * m[13]) + p.z * m[14]) + m[15]);
			if (w <= 1e-5f)
				return;
			const float inverse = 1.0f / w;
			const float sx = (((p.y * m[1] + p.x * m[0]) + p.z * m[2]) + m[3]) * inverse;
			const float sy = (((m[5] * p.y + m[4] * p.x) + m[6] * p.z) + m[7]) * inverse;
			x = (vr - vl) * 0.5f * sx + (vr + vl) * 0.5f;
			y = (vt - vb) * 0.5f * sy + (vt + vb) * 0.5f;
		};
		std::array<std::array<std::int32_t, 2>, 8> points{};
		float nearest = FLT_MAX;
		for (std::uint32_t i = 0; i < 8; ++i) {
			project({ k[i].x, k[i].y, k[i].z });
			points[i] = { Ceil(x * 1024.0f), Ceil(y * 1024.0f) };
			const float d = (((k[i].y - camera.eye.y) * forward.y + (k[i].x - camera.eye.x) * forward.x) + (k[i].z - camera.eye.z) * forward.z) - nearDistance;
			nearest = std::min(d, nearest);
		}
		if (nearest < 0.0f) {
			const V offset = Scale(forward, nearest * 1.1f);
			for (std::uint32_t i = 0; i < 8; ++i) {
				project(Sub(k[i], offset));
				points[i] = { Ceil(x * 1024.0f), Ceil(y * 1024.0f) };
			}
		}
		Hull hull;
		a_scratch.hullCount = hull.Build(points, a_scratch.hull);
		if (a_scratch.hullCount > 6)
			return false;
		a_scratch.set[0].mask = a_scratch.set[1].mask = 0;
		std::uint32_t s = 0, j = 0;
		const auto put = [&](const std::array<float, 4>& a_plane) {
			a_scratch.set[s].mask |= 1u << j;
			a_scratch.set[s].plane[j] = a_plane;
			if (++j >= 6)
				++s, j = 0;
		};
		const auto count = a_scratch.hullCount;
		for (std::uint32_t i = 0; i < count; ++i) {
			const V a = k[a_scratch.hull[i]], b = k[a_scratch.hull[(i + 1) % count]];
			const V e{ (a.x - camera.eye.x) + a.x, (a.y - camera.eye.y) + a.y, (a.z - camera.eye.z) + a.z };
			put(PlaneFromPoints(a.x, a.y, a.z, e.x, e.y, e.z, b.x, b.y, b.z));
		}
		for (std::uint32_t f = 0; f < 6; ++f) {
			const auto& t = kBoxFaces[f];
			const auto plane = PlaneFromPoints(k[t[0]].x, k[t[0]].y, k[t[0]].z, k[t[1]].x, k[t[1]].y, k[t[1]].z, k[t[2]].x, k[t[2]].y, k[t[2]].z);
			if (Dist(plane, camera.eye.x, camera.eye.y, camera.eye.z) > 0.0f) {
				a_scratch.face[f] = -1;
			} else {
				a_scratch.face[f] = static_cast<std::int32_t>((s << 8) | j);
				put(plane);
			}
		}
		return BoxRefine(a_box, a_scratch);
	}

	bool PortalViews::BoxRefine(const void* a_box, BoxScratch& a_scratch) const
	{
		// FUN_140e22220: the silhouette edges the camera-relative frustum clips taken off; valid when a back face meets it.
		const auto& g = camera.relative;
		const auto k = BoxCorners(a_box);
		std::array<V, 8> rel{};
		for (std::uint32_t i = 0; i < 8; ++i)
			rel[i] = Sub(k[i], { camera.eye.x, camera.eye.y, camera.eye.z });
		const auto count = a_scratch.hullCount;
		std::array<std::uint32_t, 8> outside{};
		bool allOutside = false;
		for (std::uint32_t p = 0; p < 6 && !allOutside; ++p) {
			if (!((g.mask >> p) & 1))
				continue;
			bool all = true;
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& c = rel[a_scratch.hull[i]];
				if (Dist(g.plane[p], c.x, c.y, c.z) < 0.0f)
					outside[i] |= 1u << p;
				else
					all = false;
			}
			allOutside = all;
		}
		if (!allOutside) {
			for (std::uint32_t i = 0; i < count; ++i) {
				const std::uint32_t next = (i + 1) % count;
				const auto& a = rel[a_scratch.hull[i]];
				const auto& b = rel[a_scratch.hull[next]];
				auto& set = a_scratch.set[i / 6];
				const std::uint32_t bit = 1u << (i % 6);
				bool visible;
				if (outside[i] & outside[next]) {
					visible = false;
				} else {
					const std::uint32_t x = outside[i] ^ outside[next];
					visible = x == 0;
					for (std::uint32_t p = 0; p < 6 && !visible; ++p) {
						if (!(((g.mask & x) >> p) & 1))
							continue;
						const auto& n = g.plane[p];
						const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
						const float den = (n[0] * dx + n[1] * dy) + n[2] * dz;
						if (den == 0.0f)
							continue;
						const float t = (((n[0] * n[3] - a.x) * n[0] + (n[1] * n[3] - a.y) * n[1]) + (n[2] * n[3] - a.z) * n[2]) / den;
						if (!(0.0f <= t && t <= 1.0f))
							continue;
						visible = true;
						const float hx = dx * t + a.x, hy = dy * t + a.y, hz = dz * t + a.z;
						for (std::uint32_t q = 0; q < 6; ++q)
							if (q != p && ((g.mask >> q) & 1) && Dist(g.plane[q], hx, hy, hz) < 0.0f)
								visible = false;
					}
				}
				set.mask = visible ? (set.mask | bit) : (set.mask & ~bit);
			}
		}
		bool valid = false;
		for (std::uint32_t f = 0; f < 6; ++f) {
			if (a_scratch.face[f] == -1)
				continue;
			const auto& t = kBoxFaces[f];
			const Quad quad{ Vec3{ rel[t[0]].x, rel[t[0]].y, rel[t[0]].z }, Vec3{ rel[t[1]].x, rel[t[1]].y, rel[t[1]].z }, Vec3{ rel[t[2]].x, rel[t[2]].y, rel[t[2]].z },
				Vec3{ rel[t[5]].x, rel[t[5]].y, rel[t[5]].z } };
			Flags scratch{};
			if (QuadVsSet(g, quad, scratch))
				valid = true;
		}
		return valid;
	}

	// ---- The walk ----

	void PortalViews::Walk(const void* a_graph, const void* a_startRoom, bool a_isFirst)
	{
		// FUN_1414a41e0: a FIFO over the rooms from the start (or unbound space), each portal crossed once.
		if (a_isFirst)
			frame.visibleUnbound = false;
		Reset(frame.unbound);
		std::deque<const void*> queue;
		if (!a_startRoom) {
			if (ListCount(a_graph, kGraphOccluders) != 0) {
				bool needOpen = true;
				ForList(a_graph, kGraphOccluders, [&](const void* a_occluder) {
					if (a_occluder)
						AddOccluderTop(frame.unbound, a_occluder, needOpen);
				});
				if (!needOpen)
					Emit(frame.unbound, kClose);
			}
			queue.push_back(nullptr);
		} else {
			Program start;
			Reset(start);
			bool needOpen = true;
			if (ListCount(a_startRoom, kRoomOccluders) != 0) {
				ForList(a_startRoom, kRoomOccluders, [&](const void* a_occluder) {
					if (a_occluder)
						AddOccluderTop(start, a_occluder, needOpen);
				});
				if (!needOpen)
					Emit(start, kClose);
			}
			if (FindRoom(a_startRoom) < 0)
				frame.rooms[AddRoom(a_startRoom)] = std::move(start);
			queue.push_back(a_startRoom);
		}
		while (!queue.empty()) {
			const void* room = queue.front();
			queue.pop_front();
			std::uint32_t index = 0;
			const bool unbound = room == nullptr;
			if (unbound) {
				frame.visibleUnbound = true;
			} else {
				if (std::find(frame.accumulated.begin(), frame.accumulated.end(), room) == frame.accumulated.end())
					frame.accumulated.push_back(room);
				const auto found = FindRoom(room);
				if (found < 0)
					continue;
				index = static_cast<std::uint32_t>(found);
			}
			Finalize(ProgramAt(index, unbound));
			const void* owner = unbound ? a_graph : room;
			ForList(owner, unbound ? kGraphPortals : kRoomPortals, [&](const void* a_portal) {
				if (!a_portal)
					return;
				const void* shared = At<const void*>(a_portal, kPortalShared);
				if (crossed.contains(shared))
					return;
				{
					const Program& from = ProgramAt(index, unbound);
					// The process's vfunc 0xD8: the camera's WithinFrustumDistFirst, a light's sphere (ParabolicVisible).
					if (!from.skipView && !(parabolic ? ParabolicVisible(*parabolic, a_portal) : WithinFrustumDistFirst(a_portal, camera.view, camera.eye)))
						return;
					if (!TestQuadProgram(from, a_portal))
						return;
				}
				const void* other = At<const void*>(a_portal, kPortalRoomA);
				if (other == room) {
					other = At<const void*>(a_portal, kPortalRoomB);
					if (other == room)
						return;
				}
				if (other != a_startRoom) {
					queue.push_back(other);
					if (!other)
						BuildUnbound(index, unbound, a_portal, a_graph);
					else
						BuildRoom(other, index, unbound, a_portal, a_startRoom);
				}
				crossed.insert(shared);
			});
		}
	}

	void PortalViews::BuildRoom(const void* a_room, std::uint32_t a_parent, bool a_parentUnbound, const void* a_portal, [[maybe_unused]] const void* a_startRoom)
	{
		const std::uint32_t occluders = ListCount(a_room, kRoomOccluders);
		std::int32_t found = FindRoom(a_room);
		if (found >= 0 && frame.rooms[found].freeOp != 0) {
			// Reached again: another path ORed in, the parent whole (no operator limit, no masking).
			auto& g = frame.rooms[found];
			const Program& f = ProgramAt(a_parent, a_parentUnbound);
			ReopenAsOr(g);
			const bool andOpen = f.freeOp != 0 || occluders != 0;
			if (andOpen)
				Emit(g, kAnd);
			if (f.freeOp != 0)
				Merge(g, f);
			AddPortal(g, a_portal);
			ForList(a_room, kRoomOccluders, [&](const void* a_occluder) {
				if (a_occluder)
					AddOccluderRoom(g, a_occluder, &f);
			});
			Emit(g, kClose);
			if (andOpen)
				Emit(g, kClose);
			return;
		}
		if (found < 0)
			found = static_cast<std::int32_t>(AddRoom(a_room));
		auto& g = frame.rooms[found];
		const Program& f = ProgramAt(a_parent, a_parentUnbound);
		Reset(g);
		// The operator limit: a parent of 26 operators or more is dropped (the child then skips the view test).
		const Program* parent = f.freeOp <= 0x19 ? &f : nullptr;
		const bool andOpen = (parent && parent->freeOp != 0) || occluders != 0;
		if (andOpen)
			Emit(g, kAnd);
		if (parent && parent->freeOp != 0) {
			CopyMasked(temp, *parent, a_portal);
			Merge(g, temp);
		}
		AddPortal(g, a_portal);
		ForList(a_room, kRoomOccluders, [&](const void* a_occluder) {
			if (a_occluder)
				AddOccluderRoom(g, a_occluder, parent);
		});
		if (andOpen) {
			if (g.freeOp == 1)
				Reset(g);
			else
				Emit(g, kClose);
		}
		const auto& flags = edgeFlags[a_portal];
		const bool unclipped = flags[0] == 0 && flags[1] == 0 && flags[2] == 0 && flags[3] == 0;
		if (!parent || parent->skipView)
			g.skipView = true;
		else if (parent->freeOp == 0 && unclipped)
			g.skipView = true;
	}

	void PortalViews::BuildUnbound(std::uint32_t a_parent, bool a_parentUnbound, const void* a_portal, const void* a_graph)
	{
		auto& u = frame.unbound;
		const Program& f = ProgramAt(a_parent, a_parentUnbound);
		const std::uint32_t occluders = ListCount(a_graph, kGraphOccluders);
		const bool andOpen = f.freeOp != 0 || occluders != 0;
		if (u.freeOp == 0) {
			if (andOpen)
				Emit(u, kAnd);
			if (f.freeOp != 0) {
				CopyMasked(temp, f, a_portal);
				Merge(u, temp);
			}
			AddPortal(u, a_portal);
			ForList(a_graph, kGraphOccluders, [&](const void* a_occluder) {
				if (a_occluder)
					AddOccluderRoom(u, a_occluder, &f);
			});
			if (andOpen)
				Emit(u, kClose);
			const auto& flags = edgeFlags[a_portal];
			if (f.freeOp == 0 && flags[0] == 0 && flags[1] == 0 && flags[2] == 0 && flags[3] == 0)
				u.skipView = true;
		} else {
			ReopenAsOr(u);
			if (andOpen)
				Emit(u, kAnd);
			if (f.freeOp != 0)
				Merge(u, f);
			AddPortal(u, a_portal);
			ForList(a_graph, kGraphOccluders, [&](const void* a_occluder) {
				if (a_occluder)
					AddOccluderRoom(u, a_occluder, &f);
			});
			Emit(u, kClose);
			if (andOpen)
				Emit(u, kClose);
		}
	}

	void PortalViews::Build()
	{
		// FUN_1401a43c0: every room holding the camera's near-plane centre walked from (only the first as the first), else unbound space.
		ZoneScopedN("CS.DCLF.PortalViews");
		frame.valid = false;
		frame.sceneFrame = Scene::FrameView::Get().GetFrame();
		// Whatever this frame builds (nothing: no programs), the words FadeStateCS, BuildDraws and the checks read, and the VisibleSet.
		struct EncodeOnExit
		{
			PortalViews& self;
			~EncodeOnExit() { self.AfterBuild(); }
		} encodeOnExit{ *this };
		const auto* sceneGraph = Global<const std::byte*>(Engine::Address::kWorldSceneGraph);
		const auto* cam = sceneGraph ? At<const std::byte*>(sceneGraph, 0x128) : nullptr;
		const auto* sceneNode = Global<const std::byte*>(Engine::Address::kShadowSceneNode);
		const void* graph = sceneNode ? At<const void*>(sceneNode, kSceneNodeGraph) : nullptr;
		frame.graph = graph;
		if (!cam || !graph) {
			++skipped;
			return;
		}
		if (graph != edgeGraph) {
			edgeFlags.clear();
			edgeGraph = graph;
			++graphVersion;
			roomIndex.clear();
			const auto* graphRooms = At<const void* const*>(graph, kGraphRooms);
			const std::uint32_t count = graphRooms ? At<std::uint32_t>(graph, kGraphRoomCount) : 0;
			for (std::uint32_t i = 0; i < count; ++i)
				if (graphRooms[i])
					roomIndex.emplace(graphRooms[i], i);
		}
		frame.roomKeys.clear();
		frame.rooms.clear();
		frame.accumulated.clear();
		frame.cameraRoom = nullptr;
		crossed.clear();
		states.clear();  // FUN_1401a4a00, before the walk
		boxes.clear();
		// The process's planes (NiCullingProcess::SetFrustum through the camera) and FUN_140e16f70's camera-relative copy.
		camera.camera = cam;
		const auto& world = *reinterpret_cast<const RE::NiTransform*>(cam + kCameraRotation);
		const auto& frustum = *reinterpret_cast<const RE::NiFrustum*>(cam + kCameraFrustum);
		camera.eye = { world.translate.x, world.translate.y, world.translate.z };
		ShadowCameras::Planes planes;
		ShadowCameras::FrustumPlanes(frustum, world, planes);
		camera.view = {};
		for (std::uint32_t p = 0; p < 6; ++p)
			camera.view.plane[p] = planes.plane[p];
		camera.view.mask = planes.mask;
		camera.relative = camera.view;
		const auto& e = camera.eye;
		for (std::uint32_t p = 0; p < 2; ++p)
			if ((camera.view.mask >> p) & 1) {
				const auto& n = camera.view.plane[p];
				camera.relative.plane[p][3] = n[3] - ((e.y * n[1] + n[0] * e.x) + e.z * n[2]);
			}
		for (std::uint32_t p = 2; p < 6; ++p)
			camera.relative.plane[p][3] = 0.0f;
		// The graph's planes judged before the walk, as the frame's actor culler judges them (FUN_1414a8ba0, PlaneJudged): the walk takes
		// an accepted one untested and skips a rejected one, and a linked pair needs its neighbour accepted.
		if (graph)
			ForList(graph, kGraphOccluders, [&](const void* a_occluder) {
				if (a_occluder && IsPlane(a_occluder))
					states[a_occluder] = PlaneJudged(a_occluder) ? 1 : 2;
			});
		const auto& r = world.rotate.entry;
		const RE::NiPoint3 nearPoint{ frustum.fNear * r[0][0] + e.x, frustum.fNear * r[1][0] + e.y, frustum.fNear * r[2][0] + e.z };
		const auto* rooms = At<const void* const*>(graph, kGraphRooms);
		const std::uint32_t roomCount = rooms ? At<std::uint32_t>(graph, kGraphRoomCount) : 0;
		const void* first = nullptr;
		bool isFirst = true;
		for (std::uint32_t i = 0; i < roomCount; ++i) {
			const void* room = rooms[i];
			// BSMultiBoundRoom::QPointWithin (vfunc 0x3F): its multibound or a joined one holds the point.
			if (room && CallVirtual<bool>(room, 0x3F, &nearPoint)) {
				Walk(graph, room, isFirst);
				if (!first) {
					first = room;
					isFirst = false;
				}
			}
		}
		if (!first)
			Walk(graph, nullptr, true);
		frame.cameraRoom = first;
		// CalculateAndDrawShadowCasterLights finalizes unbound space's before the lists take their copies.
		Finalize(frame.unbound);
		frame.valid = true;
		visibleUnbound.store(frame.visibleUnbound, std::memory_order_release);
		++built;
		Diagnostics::Notify<Diagnostics::EnginePoint::PortalWalkBuilt>();
	}

	// ---- A shadow light's rooms ----

	bool PortalViews::ParabolicVisible(const Parabolic& a_parabolic, const void* a_portal)
	{
		// TestBaseVisibility2 (0x14151a120): the portal's sphere {centre, the larger half extent}; TestBaseVisibility3: outside
		// the light's sphere culls, else a sphere reaching behind the plane or in front of it is seen; with +0x30200 set (a
		// shadow light's second hemisphere) every sphere inside.
		const float cx = At<float>(a_portal, kShapeCentre), cy = At<float>(a_portal, kShapeCentre + 4), cz = At<float>(a_portal, kShapeCentre + 8);
		const float hx = At<float>(a_portal, kShapeHalfX), hy = At<float>(a_portal, kShapeHalfY);
		const float r = hx > hy ? hx : hy;
		const float dx = cx - a_parabolic.centre.x, dy = cy - a_parabolic.centre.y, dz = cz - a_parabolic.centre.z;
		const float outside = (std::sqrt((dy * dy + dx * dx) + dz * dz) - r) - a_parabolic.radius;
		if (outside >= 0.0f)
			return false;
		const auto& n = a_parabolic.plane;
		const float side = ((cy * n[1] + cx * n[0]) + cz * n[2]) - n[3];
		if (!(side - r >= 0.0f) || a_parabolic.bothSides)
			return true;
		return side > 0.0f;
	}

	void PortalViews::WalkLight(const void* a_light, LightRooms& a_rooms)
	{
		// FUN_1414a2530 for a shadow light (vfunc 0x18 true).
		const auto* sceneNode = Global<const std::byte*>(Engine::Address::kShadowSceneNode);
		const void* graph = sceneNode ? At<const void*>(sceneNode, kSceneNodeGraph) : nullptr;
		const std::uint32_t roomCount = graph ? At<std::uint32_t>(graph, kGraphRoomCount) : 0;
		const auto* niLight = At<const std::byte*>(a_light, 0x48);
		a_rooms.walk = kLightUntouched;
		a_rooms.context.reset();
		lightFlagsPending = false;
		if (!roomCount) {
			a_rooms.walk = kLightNoGraph;
			return;
		}
		if (!niLight)
			return;
		// FUN_141509a00 (hidden, or faded under 0.05) and FUN_141509520 (on an object node) walk nothing.
		if ((At<std::uint32_t>(niLight, 0xF4) & 1) || 0.05f > At<float>(niLight, 0x134) || At<const void*>(a_light, 0x130))
			return;
		const auto* rooms = At<const void* const*>(graph, kGraphRooms);
		const auto& world = *reinterpret_cast<const RE::NiTransform*>(niLight + kCameraRotation);
		const Vec3 position{ world.translate.x, world.translate.y, world.translate.z };
		const float radius = At<float>(niLight, 0x128);
		a_rooms.rooms.clear();
		if (!At<std::uint8_t>(a_light, 0x47)) {
			// Not portal-strict: unbound space, and every room whose bound meets the light's sphere (FUN_141509080, FUN_140e14120).
			a_rooms.visibleUnbound = true;
			const RE::NiBound bound{ { position.x, position.y, position.z }, radius };
			for (std::uint32_t i = 0; i < roomCount; ++i)
				if (const void* room = rooms[i];
					room && CallVirtual<std::uint32_t>(room, 0x41, &bound) != 0 && std::find(a_rooms.rooms.begin(), a_rooms.rooms.end(), room) == a_rooms.rooms.end())
					a_rooms.rooms.push_back(room);
			a_rooms.walk = kLightBounds;
			return;
		}
		// The light's room process camera (+0x128 -> +0x18): a shadow light's own, set up with the light (an orthographic frustum,
		// near 0.1; its world-to-camera matrix and viewport as set up), read as a camera, as the shadow sources read theirs. Before the
		// engine has made one, the one FUN_1414a6400 would: the NiCamera constructor's (FUN_140d2afa0, then FUN_140d2c910 at the
		// identity). The walk puts the light's rotation and position on it.
		{
			auto* bytes = lightCamera.data();
			const auto* roomProcess = At<const std::byte*>(a_light, 0x128);
			const auto* processCamera = roomProcess ? At<const std::byte*>(roomProcess, 0x18) : nullptr;
			if (processCamera) {
				std::memcpy(bytes + kCameraWorldToCam, processCamera + kCameraWorldToCam, 0x40);
				std::memcpy(bytes + kCameraFrustum, processCamera + kCameraFrustum, 0x1C);
				std::memcpy(bytes + kCameraViewport, processCamera + kCameraViewport, 0x10);
			} else {
				std::memcpy(bytes + kCameraWorldToCam, kLightWorldToCam, sizeof(kLightWorldToCam));
				std::memcpy(bytes + kCameraFrustum, kLightFrustum, sizeof(kLightFrustum));
				bytes[kCameraFrustum + 0x18] = std::byte{ 0 };
				std::memcpy(bytes + kCameraViewport, kLightViewport, sizeof(kLightViewport));
			}
			std::memcpy(bytes + kCameraRotation, &world.rotate, sizeof(world.rotate));
			std::memcpy(bytes + kCameraEye, &world.translate, sizeof(world.translate));
		}
		const auto& r = world.rotate.entry;
		// FUN_141519d20: the plane through the light along its camera's forward (the rotation's first column).
		const auto* lightProcess = At<const std::byte*>(a_light, 0x128);
		const Parabolic sphere{ position, radius, { r[0][0], r[1][0], r[2][0], (position.y * r[1][0] + r[0][0] * position.x) + position.z * r[2][0] },
			lightProcess && At<std::uint64_t>(lightProcess, 0x30200) != 0 };
		// The walk's state swapped in: the light's own frame, camera and crossed portals, and a copy of the edge flags (the engine's
		// are on the shapes, shared: CommitLightWalk keeps the copy when the light is kept, as only a kept light's walk runs). The
		// camera-relative frustum the boxes refine against stays the main camera's (only FUN_1401a43c0 sets it); the occluder
		// states are shared.
		Camera light = camera;
		light.camera = lightCamera.data();
		light.eye = position;
		light.view = {};
		light.view.mask = 0;
		std::swap(camera, light);
		std::swap(frame, lightFrame);
		std::swap(crossed, lightCrossed);
		lightEdgeFlags = edgeFlags;
		std::swap(edgeFlags, lightEdgeFlags);
		const ProgramCounts saved = std::exchange(programCounts, {});
		parabolic = &sphere;
		frame.graph = graph;
		frame.valid = true;
		frame.sceneFrame = Scene::FrameView::Get().GetFrame();
		frame.roomKeys.clear();
		frame.rooms.clear();
		frame.accumulated.clear();
		frame.visibleUnbound = false;
		crossed.clear();
		// The camera's near point (its near along the forward) in each room: each walked as a first walk; none, unbound space.
		const float nearDistance = At<float>(lightCamera.data(), kCameraFrustum + 0x10);
		const RE::NiPoint3 nearPoint{ nearDistance * r[0][0] + position.x, nearDistance * r[1][0] + position.y, nearDistance * r[2][0] + position.z };
		bool any = false;
		for (std::uint32_t i = 0; i < roomCount; ++i)
			if (const void* room = rooms[i]; room && CallVirtual<bool>(room, 0x3F, &nearPoint)) {
				Walk(graph, room, true);
				any = true;
			}
		if (!any)
			Walk(graph, nullptr, true);
		a_rooms.visibleUnbound = frame.visibleUnbound;
		a_rooms.rooms = frame.accumulated;
		a_rooms.walk = kLightWalked;
		// Snapshot while the light's scratch and camera are active. Never expose lightFrame's reused vectors to a consumer.
		auto context = std::make_shared<Portals::Context>();
		context->kind = Portals::Context::Kind::PointLight;
		context->frame = frame.sceneFrame;
		context->generation = ++contextGeneration;
		context->roomsGeneration = graphVersion;
		context->walkSphere = { position.x, position.y, position.z, radius };
		context->walkPlane = sphere.plane;
		context->bothSides = sphere.bothSides;
		context->strict = true;
		Encode(context->programs);
		context->sharedNodes.assign(crossed.begin(), crossed.end());
		// A strict point process starts in mode 4, not the main list process's mode 0.
		if (context->programs[kPortalSetBytes / 4])
			context->programs[kPortalHeaderBytes / 4 + 6] = 4;
		a_rooms.context = std::move(context);
		lightFlagsPending = true;
		parabolic = nullptr;
		programCounts = saved;
		std::swap(edgeFlags, lightEdgeFlags);
		std::swap(crossed, lightCrossed);
		std::swap(frame, lightFrame);
		std::swap(camera, light);
	}

	bool PortalViews::LightProcessAsAssumed(const void* a_roomProcess, std::string& a_why) const
	{
		// Plane 0 still takes part in an occlusion plane's WithinFrustumDistFirst (its camera-distance test) with no plane on.
		const bool planeZero = At<float>(a_roomProcess, 0x3C) == 0.0f && At<float>(a_roomProcess, 0x40) == 0.0f && At<float>(a_roomProcess, 0x44) == 0.0f &&
		                       At<float>(a_roomProcess, 0x48) == 0.0f;
		if (At<std::uint32_t>(a_roomProcess, 0x9C) != 0 || !planeZero)
			return a_why = fmt::format("view mask {:#x}, plane 0 {} {} {} {}", At<std::uint32_t>(a_roomProcess, 0x9C), At<float>(a_roomProcess, 0x3C),
					   At<float>(a_roomProcess, 0x40), At<float>(a_roomProcess, 0x44), At<float>(a_roomProcess, 0x48)),
			       false;
		// Its portal test (vfunc 0xD8) BSParabolicCullingProcess's TestBaseVisibility2 (the process has no RTTI name to read).
		if (const std::uintptr_t test = (*reinterpret_cast<const std::uintptr_t* const*>(a_roomProcess))[0xD8 / 8]; test != REL::Offset(kParabolicPortalTest).address())
			return a_why = fmt::format("its vfunc 0xD8 at {:#x}", test - REL::Module::get().base() + 0x140000000), false;
		return true;
	}

	void PortalViews::CommitLightWalk()
	{
		if (std::exchange(lightFlagsPending, false))
			edgeFlags = std::move(lightEdgeFlags);
	}

	bool PortalViews::SharesRoom(const LightRooms& a_light) const
	{
		// The world camera's entry is this frame's walk's (a frame with no graph keeps the last walk's, as the engine's does).
		if (a_light.visibleUnbound && frame.visibleUnbound)
			return true;
		for (const void* room : a_light.rooms)
			if (std::find(frame.accumulated.begin(), frame.accumulated.end(), room) != frame.accumulated.end())
				return true;
		return false;
	}

	void PortalViews::Encode(std::vector<std::uint32_t>& a_words) const
	{
		const auto* graph = frame.valid ? frame.graph : nullptr;
		const auto* graphRooms = graph ? At<const void* const*>(graph, kGraphRooms) : nullptr;
		const std::uint32_t roomCount = graphRooms ? At<std::uint32_t>(graph, kGraphRoomCount) : 0;
		const std::uint32_t count = graph ? 1 + roomCount : 0;
		// Unbound space, then each room of the graph with its own cull mode (BSMultiBoundRoom::OnVisible gives it to the process for its
		// children; unbound space the lists').
		std::vector<PortalModel::EncodeEntry> entries(count);
		for (std::uint32_t p = 0; p < count; ++p) {
			const void* room = p ? graphRooms[p - 1] : nullptr;
			entries[p].program = p ? (room ? RoomProgram(room) : nullptr) : &frame.unbound;
			entries[p].cullMode = room ? static_cast<std::uint32_t>(static_cast<const RE::BSMultiBoundNode*>(room)->GetRuntimeData().cullingMode) : 0u;
			entries[p].room = p != 0;
		}
		// Encode runs from Build's exit guard and inside the light walk's swapped state: a stream the lowering rejects does not throw.
		PortalModel::Encode(camera.view, entries, a_words, [](const PortalModel::EncodeError& a_error) {
			static std::uint64_t rejected = 0;  // render thread
			if (std::has_single_bit(++rejected))
				logger::error("[DCLF][structural-gap] portal program {} not lowered ({}); accepting its bounds. Occurrences: {}", a_error.program, a_error.what, rejected);
		});
	}

	int PortalViews::Visible(std::span<const std::uint32_t> a_words, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags,
		std::string& a_why)
	{
		using PortalModel::Reason;
		const auto verdict = PortalModel::Visible(a_words, a_program, a_centre, a_radius, a_nodeFlags);
		switch (verdict.reason) {
		case Reason::kNoProgram:
			a_why = "no program";
			break;
		case Reason::kZeroRadius:
			a_why = "zero radius";
			break;
		case Reason::kNotReached:
			a_why = fmt::format("room {} not reached", verdict.detail);
			break;
		case Reason::kCullMode2:
			a_why = "cull mode 2";
			break;
		case Reason::kAlways:
			a_why = "always";
			break;
		case Reason::kPreprocessed:
			a_why = "preprocessed";
			break;
		case Reason::kViewPlane:
			a_why = fmt::format("view plane {}", verdict.detail);
			break;
		case Reason::kView:
			a_why = "view";
			break;
		case Reason::kAccepted:
			a_why = fmt::format("program {} accepted at op {}", a_program, verdict.detail);
			break;
		case Reason::kRejected:
			a_why = fmt::format("program {} rejected at op {}", a_program, verdict.detail);
			break;
		}
		return verdict.visible;
	}

	std::uint32_t PortalViews::ProgramOf(const RE::NiAVObject* a_node) const
	{
		if (!edgeGraph)
			return kPortalNoProgram;
		for (const auto* node = a_node; node; node = node->parent)
			if (const auto it = roomIndex.find(node); it != roomIndex.end())
				return 1 + it->second;
		return 0;
	}

	std::uint32_t PortalViews::ProgramOfRoom(const void* a_room) const
	{
		const auto it = edgeGraph ? roomIndex.find(a_room) : roomIndex.end();
		return it != roomIndex.end() ? 1 + it->second : kPortalNoProgram;
	}

	void PortalViews::NoteStructure(const RE::NiNode* a_parent)
	{
		// The rooms (ProgramOf), and the portals' shared nodes and multibound nodes a strict light's gates pass through (StrictGates.h).
		static const std::uintptr_t roomVtable = RE::VTABLE_BSMultiBoundRoom[0].address();
		static const std::uintptr_t sharedVtable = RE::VTABLE_BSPortalSharedNode[0].address();
		static const std::uintptr_t multiBoundVtable = RE::VTABLE_BSMultiBoundNode[0].address();
		if (Engine::VtableIs(a_parent, roomVtable) || Engine::VtableIs(a_parent, sharedVtable) || Engine::VtableIs(a_parent, multiBoundVtable))
			structure.fetch_add(1, std::memory_order_relaxed);
	}

	// ---- The VisibleSet ----

	PortalViews::VisibleSet::Answer PortalViews::VisibleSet::Visible(const void* a_key, bool a_previous) const
	{
		// Main::IsRoomVisible (0x140649f20): true for no node, for a node that is neither a room nor a shared node, and with no
		// entry; else the entry's map holds it, or (a shared node) its portal's entrance or exit room (+0x140 -> +0x118, +0x120).
		if (!a_key)
			return { true, kNoKey };
		if (!graph)
			return { true, kOther };
		const Keys& keys = a_previous ? previous : current;
		if (graph->rooms.contains(a_key))
			return { keys.rooms.contains(a_key), kRoom };
		const auto it = graph->shared.find(a_key);
		if (it == graph->shared.end())
			return { true, kOther };
		const auto& [a, b] = it->second;
		return { keys.shared.contains(a_key) || (a && keys.rooms.contains(a)) || (b && keys.rooms.contains(b)), kShared };
	}

	PortalViews::VisibleSetHold::VisibleSetHold()
	{
		// Counted before the load (both sequentially consistent): a publish that replaces this set then sees the count.
		auto& self = PortalViews::Get();
		self.visibleReaders.fetch_add(1, std::memory_order_seq_cst);
		set = self.visibleSet.load(std::memory_order_seq_cst);
	}

	PortalViews::VisibleSetHold::~VisibleSetHold()
	{
		PortalViews::Get().visibleReaders.fetch_sub(1, std::memory_order_release);
	}

	void PortalViews::AfterBuild()
	{
		Encode(encoded);
		auto context = std::make_shared<Portals::Context>();
		context->frame = frame.sceneFrame;
		context->generation = ++contextGeneration;
		context->roomsGeneration = graphVersion;
		context->programs = encoded;
		mainContext = std::move(context);
		if (!publishVisible.load(std::memory_order_acquire))
			return;
		PublishVisible();
		if (visibleReports.Tick())
			Features::Portals::RoomVisibilityLines();
	}

	void PortalViews::PublishVisible()
	{
		ZoneScopedN("CS.DCLF.PortalViews.VisibleSet");
		// No walk this frame (no graph or camera): no graph, so every key answers true.
		const void* graph = frame.valid ? frame.graph : nullptr;
		if (graph) {
			// The graph's rooms and portals by count: a cell's attach or detach changes them without a new graph.
			const auto* rooms = At<const void* const*>(graph, kGraphRooms);
			const std::uint32_t roomCount = rooms ? At<std::uint32_t>(graph, kGraphRoomCount) : 0;
			std::uint64_t signature = (static_cast<std::uint64_t>(roomCount) << 32) ^ ListCount(graph, kGraphPortals);
			for (std::uint32_t i = 0; i < roomCount; ++i)
				if (rooms[i])
					signature = signature * 0x100000001B3ull ^ ListCount(rooms[i], kRoomPortals);
			if (!graphKeys || graphKeys->graph != graph || graphKeys->signature != signature) {
				auto keys = std::make_shared<VisibleSet::GraphKeys>();
				keys->graph = graph;
				keys->signature = signature;
				const auto addPortal = [&](const void* a_portal) {
					if (const void* shared = a_portal ? At<const void*>(a_portal, kPortalShared) : nullptr)
						keys->shared.try_emplace(shared, std::array<const void*, 2>{ At<const void*>(a_portal, kPortalRoomA), At<const void*>(a_portal, kPortalRoomB) });
				};
				ForList(graph, kGraphPortals, addPortal);
				for (std::uint32_t i = 0; i < roomCount; ++i)
					if (rooms[i]) {
						keys->rooms.insert(rooms[i]);
						ForList(rooms[i], kRoomPortals, addPortal);
					}
				graphKeys = std::move(keys);
			}
		}
		auto set = std::make_shared<VisibleSet>();
		set->generation = visibleGeneration.load(std::memory_order_relaxed) + 1;
		set->sceneFrame = frame.sceneFrame;
		// A cell change (another graph, or none): the engine's map may still hold the last graph's keys for a frame (the observers' aside).
		const void* graphNow = graph ? graphKeys->graph : nullptr;
		const void* graphBefore = visibleOwned && visibleOwned->graph ? visibleOwned->graph->graph : nullptr;
		set->graphAge = visibleOwned && graphNow == graphBefore ? std::min(visibleOwned->graphAge + 1, 1000u) : 0u;
		if (visibleOwned)
			set->previous = visibleOwned->current;
		if (graph) {
			set->graph = graphKeys;
			// The map after the frame's lists: the walk's rooms and crossed portals' shared nodes (Traverse adds both), then every
			// portal of each room it popped (DrawWorld_BuildSceneLists over multiBoundRoomAccumList: BuildSharedCompoundFrustum).
			set->current.rooms.insert(frame.roomKeys.begin(), frame.roomKeys.end());
			set->current.shared.insert(crossed.begin(), crossed.end());
			for (const void* room : frame.accumulated)
				ForList(room, kRoomPortals, [&](const void* a_portal) {
					if (const void* shared = a_portal ? At<const void*>(a_portal, kPortalShared) : nullptr)
						set->current.shared.insert(shared);
				});
		}
		const std::uint64_t generation = set->generation;
		if (visibleOwned)
			visibleRetired.push_back(std::move(visibleOwned));
		visibleOwned = std::move(set);
		visibleSet.store(visibleOwned.get(), std::memory_order_seq_cst);
		visibleGeneration.store(generation, std::memory_order_release);
		// A reader counted at this point may hold any retired set; with none, nothing can.
		if (visibleReaders.load(std::memory_order_seq_cst) == 0)
			visibleRetired.clear();
	}

	PortalViews::BuildCounts PortalViews::TakeBuildCounts()
	{
		BuildCounts c{ built, skipped, programCounts };
		built = skipped = 0;
		programCounts = {};
		return c;
	}
}
