#pragma once

// Synthetic portal graphs: axis-aligned rooms on a grid with doors (portal quads) in their shared walls, optional doors to unbound space,
// occluder boxes and occlusion planes; a camera in a start room; and PortalViews' walk on them (Walk, BuildRoom, BuildUnbound, the
// occluders), through the program model's builders, so the encoding is a real one. The geometric tests the engine makes on a portal's
// quad (WithinFrustumDistFirst, TestQuadProgram, the edge clipping) are approximated by the quad's bounding sphere and its corners:
// the programs' structure and arithmetic are the engine's, the graph is not any real cell. The queries are objects in the rooms, a share
// placed across the programs' planes (the hard cases), with the members' frustum verdict made first (immediate when outside the view).

#include "Corpus.h"

#include <cmath>
#include <deque>
#include <random>
#include <set>
#include <sstream>

namespace Bench::Synthetic
{
	using namespace DCLF::PortalModel;

	struct V3
	{
		float x = 0, y = 0, z = 0;
	};
	inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline float Length(V3 a) { return std::sqrt(Dot(a, a)); }

	using Quad = std::array<V3, 4>;  // corners in order around the quad
	struct Box
	{
		V3 centre, half;
	};
	struct Room
	{
		V3 lo, hi;
		std::vector<std::uint32_t> portals;
		std::vector<Box> boxes;
		std::vector<Quad> planes;  // occlusion planes (only the start room's take part, as the engine's)
	};
	struct Portal
	{
		std::int32_t a = -1, b = -1;  // rooms; -1: unbound space
		Quad quad;
	};
	struct World
	{
		std::vector<Room> rooms;
		std::vector<Portal> portals;
		std::vector<std::uint32_t> unboundPortals;
		std::uint32_t start = 0;
		V3 eye, forward;
	};

	struct Params
	{
		std::string family = "grid";  // corridor, hub, grid, occluders, deep, exterior
		std::uint32_t seed = 1;
		std::uint32_t width = 6, height = 6;  // the grid of rooms (corridor/deep: width rooms; hub: spoke length)
		float roomSize = 1024.0f, roomHeight = 512.0f;
		std::uint32_t objectsPerRoom = 300;
		float edgeShare = 0.25f;      // objects placed across a plane of their room's program
		std::uint32_t boxesPerRoom = 0;
		std::uint32_t startPlanes = 0;
		float exteriorShare = 0.0f;  // border walls with a door to unbound space
		float doorShare = 1.0f;      // neighbour walls with a door
		float doorMin = 0.15f, doorMax = 0.5f;  // a door's width, a share of its wall
		float doorTopMin = 0.45f;               // a door's height, a share of the room's

		std::string Describe() const
		{
			std::ostringstream o;
			o << "synthetic family=" << family << " seed=" << seed << " size=" << width << "x" << height << " objects/room=" << objectsPerRoom
			  << " edge=" << edgeShare << " boxes/room=" << boxesPerRoom << " startPlanes=" << startPlanes << " exterior=" << exteriorShare
			  << " doors=" << doorShare << " width=" << doorMin << "-" << doorMax;
			return o.str();
		}
	};

	/** @brief The rooms, doors and occluders of a family. */
	inline World MakeWorld(const Params& a_p, std::mt19937& a_rng)
	{
		World w;
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		const float s = a_p.roomSize, h = a_p.roomHeight;
		// Cells of the grid that are rooms.
		std::vector<std::pair<std::int32_t, std::int32_t>> cells;
		if (a_p.family == "corridor" || a_p.family == "deep") {
			for (std::uint32_t i = 0; i < a_p.width; ++i)
				cells.push_back({ static_cast<std::int32_t>(i), 0 });
		} else if (a_p.family == "hub") {
			const auto l = static_cast<std::int32_t>(a_p.width);
			cells.push_back({ 0, 0 });
			for (std::int32_t i = 1; i <= l; ++i)
				for (const auto& [dx, dy] : { std::pair{ 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } })
					cells.push_back({ dx * i, dy * i });
		} else {
			for (std::uint32_t j = 0; j < a_p.height; ++j)
				for (std::uint32_t i = 0; i < a_p.width; ++i)
					cells.push_back({ static_cast<std::int32_t>(i), static_cast<std::int32_t>(j) });
		}
		std::map<std::pair<std::int32_t, std::int32_t>, std::uint32_t> at;
		for (const auto& c : cells) {
			at[c] = static_cast<std::uint32_t>(w.rooms.size());
			Room r;
			r.lo = { c.first * s, c.second * s, 0.0f };
			r.hi = { (c.first + 1) * s, (c.second + 1) * s, h };
			w.rooms.push_back(r);
		}
		// A door in the wall at x (or y) = a_coord between a_from and a_to along the other axis.
		const auto door = [&](bool a_alongY, float a_coord, float a_from, float a_to) {
			const float width = (a_to - a_from) * (a_p.doorMin + (a_p.doorMax - a_p.doorMin) * u(a_rng));
			const float start = a_from + (a_to - a_from - width) * u(a_rng);
			const float top = h * (a_p.doorTopMin + (0.95f - a_p.doorTopMin) * u(a_rng));
			Quad q;
			if (a_alongY)
				q = { V3{ a_coord, start, 0.0f }, V3{ a_coord, start + width, 0.0f }, V3{ a_coord, start + width, top }, V3{ a_coord, start, top } };
			else
				q = { V3{ start, a_coord, 0.0f }, V3{ start + width, a_coord, 0.0f }, V3{ start + width, a_coord, top }, V3{ start, a_coord, top } };
			return q;
		};
		const auto link = [&](std::int32_t a_a, std::int32_t a_b, const Quad& a_q) {
			const auto index = static_cast<std::uint32_t>(w.portals.size());
			w.portals.push_back({ a_a, a_b, a_q });
			if (a_a >= 0)
				w.rooms[a_a].portals.push_back(index);
			if (a_b >= 0)
				w.rooms[a_b].portals.push_back(index);
			else
				w.unboundPortals.push_back(index);
		};
		for (const auto& c : cells) {
			const auto i = static_cast<std::int32_t>(at[c]);
			const auto& room = w.rooms[i];
			// The +x and +y neighbours (each wall once).
			if (const auto it = at.find({ c.first + 1, c.second }); it != at.end()) {
				if (u(a_rng) < a_p.doorShare || a_p.family == "corridor" || a_p.family == "deep" || a_p.family == "hub")
					link(i, static_cast<std::int32_t>(it->second), door(true, room.hi.x, room.lo.y, room.hi.y));
			} else if (u(a_rng) < a_p.exteriorShare) {
				link(i, -1, door(true, room.hi.x, room.lo.y, room.hi.y));
			}
			if (const auto it = at.find({ c.first, c.second + 1 }); it != at.end()) {
				if (u(a_rng) < a_p.doorShare || a_p.family == "hub")
					link(i, static_cast<std::int32_t>(it->second), door(false, room.hi.y, room.lo.x, room.hi.x));
			} else if (u(a_rng) < a_p.exteriorShare) {
				link(i, -1, door(false, room.hi.y, room.lo.x, room.hi.x));
			}
			if (!at.contains({ c.first - 1, c.second }) && u(a_rng) < a_p.exteriorShare)
				link(i, -1, door(true, room.lo.x, room.lo.y, room.hi.y));
			if (!at.contains({ c.first, c.second - 1 }) && u(a_rng) < a_p.exteriorShare)
				link(i, -1, door(false, room.lo.y, room.lo.x, room.hi.x));
		}
		// Occluders: boxes in every room (pillars, furniture), occlusion planes in the start room.
		for (auto& room : w.rooms)
			for (std::uint32_t b = 0; b < a_p.boxesPerRoom; ++b) {
				const V3 half{ s * (0.05f + 0.1f * u(a_rng)), s * (0.05f + 0.1f * u(a_rng)), h * (0.2f + 0.3f * u(a_rng)) };
				const V3 centre{ room.lo.x + half.x + (s - 2 * half.x) * u(a_rng), room.lo.y + half.y + (s - 2 * half.y) * u(a_rng), half.z };
				room.boxes.push_back({ centre, half });
			}
		w.start = 0;
		auto& start = w.rooms[w.start];
		for (std::uint32_t p = 0; p < a_p.startPlanes; ++p) {
			const float x = start.lo.x + s * (0.3f + 0.6f * u(a_rng)), y0 = start.lo.y + s * 0.2f * u(a_rng), y1 = y0 + s * (0.2f + 0.4f * u(a_rng));
			start.planes.push_back({ V3{ x, y0, 0.0f }, V3{ x, y1, 0.0f }, V3{ x, y1, h * 0.8f }, V3{ x, y0, h * 0.8f } });
		}
		// The camera: in the start room, eye height, looking into the graph (corridors along it, the hub anywhere).
		w.eye = { start.lo.x + s * (0.2f + 0.3f * u(a_rng)), start.lo.y + s * (0.2f + 0.6f * u(a_rng)), 128.0f };
		float angle = a_p.family == "hub" ? 6.2831853f * u(a_rng) : a_p.family == "corridor" || a_p.family == "deep" ? (u(a_rng) - 0.5f) * 0.5f : 0.785398f + (u(a_rng) - 0.5f) * 0.8f;
		w.forward = { std::cos(angle), std::sin(angle), 0.0f };
		return w;
	}

	/** @brief The camera's view planes (NiFrustumPlanes' order: near, far, left, right, top, bottom; inside dot(n, p) - d >= 0). */
	inline PlaneSet ViewPlanes(const World& a_w)
	{
		const V3 f = a_w.forward, up{ 0, 0, 1 };
		const V3 r{ f.y, -f.x, 0.0f };  // right of forward, horizontal
		const float hh = 0.6545f, hv = 0.3927f;  // 75 and 45 degree fields of view
		const auto plane = [&](V3 n, float d) { return std::array<float, 4>{ n.x, n.y, n.z, d }; };
		PlaneSet v;
		v.mask = 0x3F;
		v.plane[0] = plane(f, Dot(f, a_w.eye) + 15.0f);
		v.plane[1] = plane(f * -1.0f, -(Dot(f, a_w.eye) + 100000.0f));
		const V3 left = r * std::cos(hh) + f * std::sin(hh), right = r * -std::cos(hh) + f * std::sin(hh);
		const V3 top = up * -std::cos(hv) + f * std::sin(hv), bottom = up * std::cos(hv) + f * std::sin(hv);
		v.plane[2] = plane(left, Dot(left, a_w.eye));
		v.plane[3] = plane(right, Dot(right, a_w.eye));
		v.plane[4] = plane(top, Dot(top, a_w.eye));
		v.plane[5] = plane(bottom, Dot(bottom, a_w.eye));
		return v;
	}

	/** @brief PortalViews' walk on a synthetic world (its function names are the engine's). */
	class Walker
	{
	public:
		Walker(const World& a_w) :
			w(a_w), view(ViewPlanes(a_w)), rooms(a_w.rooms.size()) {}

		std::vector<std::uint32_t> Encode()
		{
			Walk();
			std::vector<EncodeEntry> entries(1 + w.rooms.size());
			entries[0] = { reachedUnbound ? &unbound : nullptr, 0, false };
			for (std::size_t i = 0; i < w.rooms.size(); ++i)
				entries[1 + i] = { reached[i] ? &rooms[i] : nullptr, 4, true };
			std::vector<std::uint32_t> words;
			DCLF::PortalModel::Encode(view, entries, words, [](const EncodeError& a_e) {
				throw std::runtime_error(std::string("synthetic program not lowered: ") + a_e.what);
			});
			return words;
		}
		const Program* ProgramOf(std::uint32_t a_room) const { return reached[a_room] ? &rooms[a_room] : nullptr; }
		const PlaneSet& View() const { return view; }

	private:
		const World& w;
		PlaneSet view;
		std::vector<Program> rooms;
		std::vector<bool> reached = std::vector<bool>(w.rooms.size(), false);
		Program unbound, temp;
		bool reachedUnbound = false;

		std::array<float, 3> Eye() const { return { w.eye.x, w.eye.y, w.eye.z }; }
		static void Sphere(const Quad& a_q, float a_c[3], float& a_r)
		{
			const V3 c = (a_q[0] + a_q[1] + a_q[2] + a_q[3]) * 0.25f;
			a_c[0] = c.x, a_c[1] = c.y, a_c[2] = c.z;
			a_r = 0.0f;
			for (const auto& p : a_q)
				a_r = std::max(a_r, Length(p - c));
		}
		bool InView(const float a_c[3], float a_r) const
		{
			for (std::uint32_t p = 0; p < 6; ++p)
				if (((view.mask >> p) & 1) && Dist(view.plane[p], a_c[0], a_c[1], a_c[2]) <= -a_r)
					return false;
			return true;
		}
		/** @brief The edge clipping, approximated: a quad with a corner outside the view has clipped edges. */
		bool Clipped(const Quad& a_q) const
		{
			for (const auto& c : a_q)
				for (std::uint32_t p = 0; p < 6; ++p)
					if (((view.mask >> p) & 1) && Dist(view.plane[p], c.x, c.y, c.z) < 0.0f)
						return true;
			return false;
		}
		static bool Passes(const Program& a_program, const float a_c[3], float a_r)
		{
			if (a_program.freeOp == 0 || !a_program.finalized)
				return true;
			const auto ops = std::min<std::uint32_t>(a_program.freeOp + 3, static_cast<std::uint32_t>(a_program.ops.size()));
			const auto compiled = DCLF::PortalBytecode::Lower<Op>(std::span(a_program.ops).first(ops), a_program.firstOp, a_program.freePlane);
			return DCLF::PortalBytecode::Run<PlaneSet>(compiled, std::span(a_program.sets).first(a_program.freePlane), a_c, a_r);
		}
		/** @brief FUN_140e233b0 with unclipped edges: the quad's plane (the camera on its non-positive side) and its edges through the eye. */
		PlaneSet PortalPlanes(const Quad& a_q) const
		{
			const auto plane = [](V3 a, V3 b, V3 d) { return PlaneFromPoints(a.x, a.y, a.z, b.x, b.y, b.z, d.x, d.y, d.z); };
			const auto& c = a_q;
			PlaneSet out;
			out.mask = 0x3F;
			const auto p = plane(c[0], c[1], c[2]);
			if (Dist(p, w.eye.x, w.eye.y, w.eye.z) <= 0.0f) {
				out.plane[0] = p;
				out.plane[2] = plane(c[0], c[1], w.eye);
				out.plane[3] = plane(c[2], c[3], w.eye);
				out.plane[4] = plane(c[3], c[0], w.eye);
				out.plane[5] = plane(c[1], c[2], w.eye);
			} else {
				out.plane[0] = plane(c[1], c[0], c[2]);
				out.plane[2] = plane(c[1], c[0], w.eye);
				out.plane[3] = plane(c[3], c[2], w.eye);
				out.plane[4] = plane(c[0], c[3], w.eye);
				out.plane[5] = plane(c[2], c[1], w.eye);
			}
			out.mask &= ~2u;
			return out;
		}
		void AddPortal(Program& a_program, const Quad& a_q) const
		{
			// FUN_140e32710: all six on, none within 5 units of the portal's plane.
			const auto p = PlaneFromPoints(a_q[0].x, a_q[0].y, a_q[0].z, a_q[1].x, a_q[1].y, a_q[1].z, a_q[2].x, a_q[2].y, a_q[2].z);
			const std::uint32_t s = a_program.freePlane;
			EmitTest(a_program, kPortalTest, s);
			auto& set = Grow(a_program, s);
			set = PortalPlanes(a_q);
			set.mask = std::abs(Dist(p, w.eye.x, w.eye.y, w.eye.z)) <= 5.0f ? 0u : 0x3Fu;
			++a_program.freePlane;
		}
		/** @brief FUN_140e32fe0, approximated: OR(not wholly behind the face toward the eye along x, nor along y). */
		void AddBox(Program& a_program, const Box& a_b) const
		{
			const auto face = [&](bool a_x) {
				const V3 c = a_b.centre, h = a_b.half;
				if (a_x) {
					const float x = w.eye.x < c.x ? c.x - h.x : c.x + h.x;
					return Quad{ V3{ x, c.y - h.y, c.z - h.z }, V3{ x, c.y + h.y, c.z - h.z }, V3{ x, c.y + h.y, c.z + h.z }, V3{ x, c.y - h.y, c.z + h.z } };
				}
				const float y = w.eye.y < c.y ? c.y - h.y : c.y + h.y;
				return Quad{ V3{ c.x - h.x, y, c.z - h.z }, V3{ c.x + h.x, y, c.z - h.z }, V3{ c.x + h.x, y, c.z + h.z }, V3{ c.x - h.x, y, c.z + h.z } };
			};
			const std::uint32_t s = a_program.freePlane;
			Grow(a_program, s + 1);
			a_program.sets[s] = PortalPlanes(face(true));
			a_program.sets[s + 1] = PortalPlanes(face(false));
			Emit(a_program, kOr);
			EmitTest(a_program, kOccluderTest, s);
			EmitTest(a_program, kOccluderTest, s + 1);
			Emit(a_program, kClose);
			a_program.freePlane += 2;
		}
		static void BoxSphere(const Box& a_b, float a_c[3], float& a_r)
		{
			a_c[0] = a_b.centre.x, a_c[1] = a_b.centre.y, a_c[2] = a_b.centre.z;
			a_r = Length(a_b.half);
		}
		/** @brief FUN_140e32920 for the start room: a box or plane in view joins the AND opened on the first. */
		void AddOccludersTop(Program& a_program, const Room& a_room)
		{
			bool needOpen = true;
			const auto open = [&] {
				if (needOpen)
					Emit(a_program, kAnd);
				needOpen = false;
			};
			for (const auto& b : a_room.boxes) {
				float c[3], r;
				BoxSphere(b, c, r);
				if (InView(c, r)) {
					open();
					AddBox(a_program, b);
				}
			}
			for (const auto& q : a_room.planes) {
				float c[3], r;
				Sphere(q, c, r);
				if (InView(c, r)) {
					open();
					const std::uint32_t s = a_program.freePlane;
					EmitTest(a_program, kOccluderTest, s);
					Grow(a_program, s) = PortalPlanes(q);
					++a_program.freePlane;
				}
			}
			if (!needOpen)
				Emit(a_program, kClose);
		}
		/** @brief FUN_140e32860: a room's boxes that pass the parent's program (its planes take no part). */
		void AddOccludersRoom(Program& a_program, const Room& a_room, const Program* a_parent) const
		{
			for (const auto& b : a_room.boxes) {
				float c[3], r;
				BoxSphere(b, c, r);
				if (!a_parent || Passes(*a_parent, c, r))
					AddBox(a_program, b);
			}
		}
		void CopyMaskedFor(Program& a_out, const Program& a_parent, const Portal& a_portal) const
		{
			const bool unclipped = !Clipped(a_portal.quad);
			CopyMasked(a_out, a_parent, [&](const PlaneSet& a_set) {
				if (!unclipped)
					return false;
				for (const auto& c : a_portal.quad)
					for (std::uint32_t p = 0; p < 6; ++p)
						if (((a_set.mask >> p) & 1) && Dist(a_set.plane[p], c.x, c.y, c.z) < 0.0f)
							return false;
				return true;
			});
		}
		void BuildRoom(std::uint32_t a_room, const Program& a_from, const Portal& a_portal)
		{
			const auto& room = w.rooms[a_room];
			const bool occluders = !room.boxes.empty();
			auto& g = rooms[a_room];
			if (reached[a_room] && g.freeOp != 0) {
				ReopenAsOr(g);
				const bool andOpen = a_from.freeOp != 0 || occluders;
				if (andOpen)
					Emit(g, kAnd);
				if (a_from.freeOp != 0)
					Merge(g, a_from);
				AddPortal(g, a_portal.quad);
				AddOccludersRoom(g, room, &a_from);
				Emit(g, kClose);
				if (andOpen)
					Emit(g, kClose);
				return;
			}
			reached[a_room] = true;
			Reset(g, Eye());
			const Program* parent = a_from.freeOp <= 0x19 ? &a_from : nullptr;
			const bool andOpen = (parent && parent->freeOp != 0) || occluders;
			if (andOpen)
				Emit(g, kAnd);
			if (parent && parent->freeOp != 0) {
				CopyMaskedFor(temp, *parent, a_portal);
				Merge(g, temp);
			}
			AddPortal(g, a_portal.quad);
			AddOccludersRoom(g, room, parent);
			if (andOpen) {
				if (g.freeOp == 1)
					Reset(g, Eye());
				else
					Emit(g, kClose);
			}
			const bool unclipped = !Clipped(a_portal.quad);
			if (!parent || parent->skipView)
				g.skipView = true;
			else if (parent->freeOp == 0 && unclipped)
				g.skipView = true;
		}
		void BuildUnbound(const Program& a_from, const Portal& a_portal)
		{
			auto& u = unbound;
			const bool andOpen = a_from.freeOp != 0;
			if (u.freeOp == 0) {
				if (andOpen)
					Emit(u, kAnd);
				if (a_from.freeOp != 0) {
					CopyMaskedFor(temp, a_from, a_portal);
					Merge(u, temp);
				}
				AddPortal(u, a_portal.quad);
				if (andOpen)
					Emit(u, kClose);
				if (a_from.freeOp == 0 && !Clipped(a_portal.quad))
					u.skipView = true;
			} else {
				ReopenAsOr(u);
				if (andOpen)
					Emit(u, kAnd);
				if (a_from.freeOp != 0)
					Merge(u, a_from);
				AddPortal(u, a_portal.quad);
				Emit(u, kClose);
				if (andOpen)
					Emit(u, kClose);
			}
		}
		void Walk()
		{
			Reset(unbound, Eye());
			{
				Program start;
				Reset(start, Eye());
				AddOccludersTop(start, w.rooms[w.start]);
				rooms[w.start] = std::move(start);
				reached[w.start] = true;
			}
			std::set<std::uint32_t> crossed;
			std::deque<std::int32_t> queue{ static_cast<std::int32_t>(w.start) };
			while (!queue.empty()) {
				const std::int32_t room = queue.front();
				queue.pop_front();
				const bool isUnbound = room < 0;
				Program& from = isUnbound ? unbound : rooms[room];
				if (isUnbound)
					reachedUnbound = true;
				Finalize(from);
				const auto& portals = isUnbound ? w.unboundPortals : w.rooms[room].portals;
				for (const std::uint32_t index : portals) {
					if (crossed.contains(index))
						continue;
					const auto& portal = w.portals[index];
					float c[3], r;
					Sphere(portal.quad, c, r);
					if (!from.skipView && !InView(c, r))
						continue;
					if (!Passes(from, c, r))
						continue;
					const std::int32_t other = portal.a == room ? portal.b : portal.a;
					if (other == room)
						continue;
					if (other != static_cast<std::int32_t>(w.start)) {
						queue.push_back(other);
						if (other < 0)
							BuildUnbound(from, portal);
						else
							BuildRoom(static_cast<std::uint32_t>(other), from, portal);
					}
					crossed.insert(index);
				}
			}
		}
	};

	/** @brief A corpus: the world walked from its camera, and its objects as the member queries the GPU would have prepared. */
	inline Corpus Make(const Params& a_p)
	{
		std::mt19937 rng(a_p.seed * 2654435761u + 17u);
		const World world = MakeWorld(a_p, rng);
		Walker walker(world);
		Corpus corpus;
		corpus.name = a_p.family + "-" + std::to_string(a_p.seed);
		corpus.metadata = a_p.Describe();
		corpus.words = walker.Encode();
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		std::lognormal_distribution<float> radius(std::log(24.0f), 0.9f);
		const auto& view = walker.View();
		const auto inView = [&](const std::array<float, 4>& a_b) {
			for (std::uint32_t p = 0; p < 6; ++p)
				if (((view.mask >> p) & 1) && Dist(view.plane[p], a_b[0], a_b[1], a_b[2]) <= -a_b[3])
					return false;
			return true;
		};
		const auto push = [&](std::array<float, 4> a_bound, std::uint32_t a_program) {
			Query q;
			q.bound = a_bound;
			q.destination = static_cast<std::uint32_t>(corpus.queries.size());
			q.generation = 1;
			const float pick = u(rng);
			if (pick < 0.01f)
				q.bound[3] = 0.0f;  // a zero-radius node
			else if (pick < 0.03f)
				q.flags |= 1u << 11;  // always-draw
			else if (pick < 0.05f)
				q.flags |= (1u << 12) | (u(rng) < 0.5f ? 1u << 20 : 0u);  // preprocessed (and hidden)
			// PreparePortalQueries: the frustum verdict first; outside the view the query is immediate.
			if (q.bound[3] > 0.0f && !(q.flags & (1u << 11)) && !inView(q.bound)) {
				q.flags = kImmediate;
				q.program = 0;
			} else {
				q.program = a_program;
			}
			corpus.queries.push_back(q);
		};
		for (std::uint32_t i = 0; i < world.rooms.size(); ++i) {
			const auto& room = world.rooms[i];
			const Program* program = walker.ProgramOf(i);
			for (std::uint32_t o = 0; o < a_p.objectsPerRoom; ++o) {
				const float r = std::clamp(radius(rng), 1.0f, 400.0f);
				std::array<float, 4> b{ room.lo.x + (room.hi.x - room.lo.x) * u(rng), room.lo.y + (room.hi.y - room.lo.y) * u(rng),
					room.lo.z + (room.hi.z - room.lo.z) * u(rng), r };
				// Across a plane of the room's program: the bound within about its radius of the plane, either side.
				if (program && program->freePlane && u(rng) < a_p.edgeShare) {
					const auto& set = program->sets[rng() % program->freePlane];
					if (set.mask) {
						std::uint32_t p = rng() % 6;
						while (!((set.mask >> p) & 1))
							p = (p + 1) % 6;
						const auto& n = set.plane[p];
						const float d = Dist(n, b[0], b[1], b[2]) - (u(rng) * 2.4f - 1.2f) * r;
						b[0] -= n[0] * d, b[1] -= n[1] * d, b[2] -= n[2] * d;
					}
				}
				push(b, 1 + i);
			}
		}
		// Objects outside every room, in unbound space (program 0), when the graph has doors to it.
		if (!world.unboundPortals.empty())
			for (std::uint32_t o = 0; o < a_p.objectsPerRoom * 2; ++o) {
				const float span = a_p.roomSize * static_cast<float>(std::max(a_p.width, a_p.height) + 4);
				push({ -2.0f * a_p.roomSize + span * u(rng), -2.0f * a_p.roomSize + span * u(rng), a_p.roomHeight * u(rng), std::clamp(radius(rng), 1.0f, 400.0f) }, 0);
			}
		return corpus;
	}

	/**
	 * @brief The evaluator's own cost: only a corpus's queries against a program with operators (not immediate, no early verdict),
	 * repeated to at least a_minimum, destinations renumbered.
	 */
	inline Corpus ProgramsOnly(const Corpus& a_corpus, std::uint32_t a_minimum)
	{
		Corpus out;
		out.name = a_corpus.name + "-programs";
		out.metadata = a_corpus.metadata + " (queries against programs with operators only)";
		out.words = a_corpus.words;
		std::vector<Query> picked;
		for (const auto& q : a_corpus.queries) {
			if (q.flags != 0 || q.bound[3] == 0.0f)
				continue;
			const auto verdict = DCLF::PortalModel::Visible(a_corpus.words, q.program, q.bound.data(), q.bound[3], 0);
			if (verdict.reason == Reason::kAccepted || verdict.reason == Reason::kRejected)
				picked.push_back(q);
		}
		if (picked.empty())
			return out;
		while (out.queries.size() < a_minimum)
			for (const auto& q : picked) {
				auto copy = q;
				copy.destination = static_cast<std::uint32_t>(out.queries.size());
				out.queries.push_back(copy);
			}
		return out;
	}

	/** @brief The standard set: every family at a few seeds and sizes. */
	inline std::vector<Params> Standard()
	{
		std::vector<Params> all;
		const auto add = [&](Params p, std::uint32_t a_seeds) {
			for (std::uint32_t s = 1; s <= a_seeds; ++s) {
				p.seed = s;
				all.push_back(p);
			}
		};
		Params corridor;
		corridor.family = "corridor";
		corridor.width = 12;
		corridor.height = 1;
		add(corridor, 2);
		Params hub;
		hub.family = "hub";
		hub.width = 4;
		add(hub, 2);
		Params grid;
		grid.family = "grid";
		grid.width = grid.height = 6;
		grid.doorShare = 0.7f;
		add(grid, 2);
		Params occluders = grid;
		occluders.family = "occluders";
		occluders.boxesPerRoom = 2;
		occluders.startPlanes = 3;
		add(occluders, 2);
		Params deep;
		deep.family = "deep";
		deep.width = 24;
		deep.height = 1;
		deep.boxesPerRoom = 1;
		add(deep, 2);
		Params exterior = grid;
		exterior.family = "exterior";
		exterior.width = exterior.height = 5;
		exterior.exteriorShare = 0.5f;
		add(exterior, 2);
		// Wall-wide openings: many rooms reached, and reached again (OR merges).
		Params open = grid;
		open.family = "open";
		open.doorShare = 0.9f;
		open.doorMin = 0.6f;
		open.doorMax = 0.95f;
		open.doorTopMin = 0.8f;
		open.boxesPerRoom = 1;
		add(open, 2);
		Params large = grid;
		large.family = "large";
		large.width = large.height = 10;
		large.objectsPerRoom = 600;
		large.boxesPerRoom = 1;
		large.doorMin = 0.3f;
		large.doorMax = 0.8f;
		add(large, 1);
		return all;
	}
}
