#include "PortalViews.h"

#include "EngineAccess.h"
#include "SunViews.h"
#include "Features/DrawcallLimitFix/Scene/Records.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>

// The main camera's portal and occluder programs (T5a). Each function names the engine function it ports (AE 1.6.1170); the
// arithmetic keeps the engine's grouping where it decides a comparison (skyrim-engine-notes.md, "Portals, rooms and occluders").

namespace DCLF
{
	namespace
	{
		using namespace Engine;

		constexpr std::uintptr_t kShadowSceneNode = 0x2033060;  // the main ShadowSceneNode
		constexpr std::uintptr_t kWorldSceneGraph = 0x338c8e8;  // the world scene graph; its camera at +0x128
		constexpr std::uintptr_t kListProcesses = 0x338c8a0;    // the scene lists' culling processes
		constexpr std::size_t kSceneNodeGraph = 0x228;          // ShadowSceneNode: its BSPortalGraph

		// BSOcclusionShape (and BSOcclusionPlane, BSPortal, BSOcclusionBox).
		constexpr std::size_t kShapeCentre = 0x10;
		constexpr std::size_t kShapeRotation = 0x1C;  // M[r][c] at +0x1C + 4 * (3r + c)
		constexpr std::size_t kShapeHalfX = 0x48;     // a plane's half extents; a box's extents at +0x48, +0x4C, +0x50
		constexpr std::size_t kShapeHalfY = 0x4C;
		constexpr std::size_t kBoxBound = 0x198;      // BSMultiBoundShape*
		constexpr std::array<std::size_t, 4> kPlaneLinks{ 0xF8, 0x100, 0x108, 0x110 };
		constexpr std::size_t kPortalRoomA = 0x118, kPortalRoomB = 0x120, kPortalShared = 0x128;
		// BSMultiBoundRoom: the portal and occluder lists (head; count at +0x10); BSPortalGraph: its occluders, portals, rooms.
		constexpr std::size_t kRoomPortals = 0x138, kRoomOccluders = 0x150;
		constexpr std::size_t kGraphOccluders = 0x10, kGraphPortals = 0x28, kGraphRooms = 0x40, kGraphRoomCount = 0x50;
		constexpr std::size_t kListCount = 0x10;     // a list's count, from its head field
		constexpr std::size_t kListNodeItem = 0x10;  // a list node: {next, prev, item}
		// NiCamera.
		constexpr std::size_t kCameraRotation = 0x7C, kCameraEye = 0xA0, kCameraWorldToCam = 0x110, kCameraFrustum = 0x150,
							  kCameraViewport = 0x174;
		// BSCullingProcess and its BSPortalGraphEntry, BSCompoundFrustum (the parity's reads).
		constexpr std::size_t kProcessEntry = 0x30190;
		constexpr std::size_t kEntryAccum = 0x18, kEntryAccumSize = 0x28, kEntryMapCapacity = 0x44, kEntryMapEntries = 0x60,
							  kEntryUnbound = 0x68, kEntryVisibleUnbound = 0x130;
		constexpr std::size_t kFrustumSets = 0x0, kFrustumOps = 0x18, kFrustumFreePlane = 0xB8, kFrustumFreeOp = 0xBC,
							  kFrustumFirstOp = 0xC0, kFrustumSkipView = 0xC4, kFrustumFinalized = 0xC5;

		// A light's room process's camera (T5a3): the NiCamera constructor's (FUN_140d2afa0) frustum and viewport, and the
		// world-to-camera matrix FUN_140d2c910 makes of them at the identity (nothing updates it after).
		constexpr float kLightWorldToCam[16]{ 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, -2.0f, 1.0f, 0.0f, 0.0f, 0.0f };
		constexpr float kLightFrustum[6]{ -0.5f, 0.5f, 0.5f, -0.5f, 1.0f, 2.0f };
		constexpr float kLightViewport[4]{ 0.0f, 1.0f, 1.0f, 0.0f };

		constexpr std::uint32_t kAnd = 4, kOr = 5, kClose = 6, kPortalTest = 7, kOccluderTest = 8;

		bool IsTest(std::uint32_t a_word) { return a_word == kPortalTest || a_word == kOccluderTest; }
		bool IsOpen(std::uint32_t a_word) { return a_word == kAnd || a_word == kOr; }

		bool RttiIs(const void* a_object, const char* a_name)
		{
			const auto* rtti = static_cast<const RE::NiObject*>(a_object)->GetRTTI();
			return rtti && rtti->name && std::strcmp(rtti->name, a_name) == 0;
		}

		/** @brief BSOcclusionShape vfunc 0x25: a plane (or portal), else a box. */
		bool IsPlane(const void* a_shape) { return static_cast<const RE::BSOcclusionShape*>(a_shape)->IsOcclusionPlane(); }

		float M(const void* a_shape, std::uint32_t a_row, std::uint32_t a_column) { return At<float>(a_shape, kShapeRotation + 4 * (3 * a_row + a_column)); }

		template <class F>
		void ForList(const void* a_owner, std::size_t a_head, F&& a_each)
		{
			for (const auto* node = At<const std::byte*>(a_owner, a_head); node; node = At<const std::byte*>(node, 0))
				a_each(At<const void*>(node, kListNodeItem));
		}

		std::uint32_t ListCount(const void* a_owner, std::size_t a_head) { return At<std::uint32_t>(a_owner, a_head + kListCount); }

		/** @brief FUN_140e16f70's and the shapes' camera: a world position. */
		struct V
		{
			float x, y, z;
		};
		V Sub(const V& a, const V& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		V Add(const V& a, const V& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		V Scale(const V& a, float s) { return { a.x * s, a.y * s, a.z * s }; }

		/** @brief Every plane test's distance: ((n.y * p.y + n.x * p.x) + n.z * p.z) - d. */
		float Dist(const std::array<float, 4>& a_plane, float a_x, float a_y, float a_z)
		{
			return ((a_plane[1] * a_y + a_plane[0] * a_x) + a_plane[2] * a_z) - a_plane[3];
		}

		/** @brief FUN_140d32ab0: the plane through a, b, c (n = (b - a) x (c - b), normalised; zero below 1e-6). */
		std::array<float, 4> PlaneFromPoints(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz)
		{
			const float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
			const float e2x = cx - bx, e2y = cy - by, e2z = cz - bz;
			float nx = e2z * e1y - e2y * e1z;
			float ny = e2x * e1z - e2z * e1x;
			float nz = e2y * e1x - e2x * e1y;
			const float length = std::sqrt((ny * ny + nx * nx) + nz * nz);
			if (length <= 1e-6f) {
				nx = ny = nz = 0.0f;
			} else {
				const float inverse = 1.0f / length;
				nx *= inverse;
				ny *= inverse;
				nz *= inverse;
			}
			return { nx, ny, nz, (ny * ay + nx * ax) + nz * az };
		}

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

		/** @brief FUN_140e209e0: the box's eight corners from its centre, rotation columns and extents. */
		std::array<V, 8> BoxCorners(const void* a_box)
		{
			const V c{ At<float>(a_box, kShapeCentre), At<float>(a_box, kShapeCentre + 4), At<float>(a_box, kShapeCentre + 8) };
			const float ex = At<float>(a_box, 0x48), ey = At<float>(a_box, 0x4C), ez = At<float>(a_box, 0x50);
			const V x{ ex * M(a_box, 0, 0), ex * M(a_box, 1, 0), ex * M(a_box, 2, 0) };
			const V y{ ey * M(a_box, 0, 1), ey * M(a_box, 1, 1), ey * M(a_box, 2, 1) };
			const V z{ ez * M(a_box, 0, 2), ez * M(a_box, 1, 2), ez * M(a_box, 2, 2) };
			// The engine's grouping (from its disassembly): corners 0 and 4 are ((X + C) + Y) +- Z.
			const V yxc = Add(Add(x, c), y);
			const V xcy = Sub(Add(x, c), y), cxy = Sub(Sub(c, x), y), ycx = Add(y, Sub(c, x)), cxy2 = Add(Sub(c, x), y);
			return { Add(yxc, z), Add(xcy, z), Add(cxy, z), Add(ycx, z), Sub(yxc, z), Sub(xcy, z), Sub(cxy, z), Sub(cxy2, z) };
		}

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
		// FUN_140e32580: the counts and flags; the arrays' contents stay.
		a_program.eye = { camera.eye.x, camera.eye.y, camera.eye.z };
		a_program.freePlane = a_program.freeOp = a_program.firstOp = 0;
		a_program.skipView = a_program.finalized = false;
	}

	void PortalViews::Emit(Program& a_program, std::uint32_t a_word)
	{
		// FUN_140e32600 (AND: leaves finalized), FUN_140e32650 (OR), FUN_140e326b0 (close): both clear it.
		if (a_program.ops.size() < a_program.freeOp + 4)
			a_program.ops.resize(a_program.freeOp + 16);
		a_program.ops[a_program.freeOp++].word = a_word;
		if (a_word != kAnd)
			a_program.finalized = false;
	}

	void PortalViews::EmitTest(Program& a_program, std::uint32_t a_type, std::uint32_t a_set)
	{
		if (a_program.ops.size() < a_program.freeOp + 5)
			a_program.ops.resize(a_program.freeOp + 16);
		a_program.ops[a_program.freeOp++].word = a_type;
		a_program.ops[a_program.freeOp++].word = a_set;
	}

	PortalViews::PlaneSet& PortalViews::Grow(Program& a_program, std::uint32_t a_index)
	{
		if (a_program.sets.size() <= a_index)
			a_program.sets.resize(a_index + 4);
		return a_program.sets[a_index];
	}

	void PortalViews::Finalize(Program& a_program)
	{
		// FUN_140e33c70: the token stream into a branch program, then jump threading.
		auto& p = a_program;
		if (p.finalized || p.freeOp == 0) {
			p.finalized = true;
			return;
		}
		const std::uint32_t n = p.freeOp;
		if (p.ops.size() < n + 4)
			p.ops.resize(n + 16);
		auto& ops = p.ops;
		ops[n].word = 1;
		ops[n + 1].word = 2;
		ops[n + 2].word = 3;
		// Pass 1: each group's closing index (groups numbered from 1 in opener order; group 0 is the top level).
		std::vector<std::uint32_t> closeIndex(n + 2, 0), ids(n + 2, 0);
		std::uint32_t depth = 0, nextId = 1;
		for (std::uint32_t i = 0; i <= n; ++i) {
			const std::uint32_t w = ops[i].word;
			if (w == 1) {
				closeIndex[0] = i;
				if (depth != 0) {
					p.freeOp = 0;  // unbalanced: wiped, not finalized
					return;
				}
			} else if (IsOpen(w)) {
				ids[++depth] = nextId++;
			} else if (w == kClose) {
				if (depth == 0)
					break;
				closeIndex[ids[depth]] = i;
				--depth;
			} else if (IsTest(w)) {
				++i;
			}
		}
		// Pass 2: the branch targets.
		std::vector<std::uint32_t> typeStack{ 0 }, idStack{ 0 };
		nextId = 1;
		bool found = false;
		for (std::uint32_t i = 0; i <= n; ++i) {
			auto& op = ops[i];
			const std::uint32_t w = op.word;
			if (w == 1) {
				op.yes = n + 1;
				op.no = n + 2;
			} else if (IsOpen(w)) {
				typeStack.push_back(w);
				idStack.push_back(nextId++);
				op.yes = op.no = i + 1;
			} else if (w == kClose) {
				if (typeStack.size() > 1) {
					typeStack.pop_back();
					idStack.pop_back();
				}
				const std::uint32_t type = typeStack.back(), id = idStack.back();
				if (type == kAnd) {
					op.yes = i + 1;
					op.no = closeIndex[id];
				} else if (type == kOr) {
					op.yes = closeIndex[id];
					op.no = i + 1;
				} else {
					op.yes = op.no = i + 1;
				}
			} else if (IsTest(w)) {
				if (!found) {
					p.firstOp = i;
					found = true;
				}
				const std::uint32_t type = typeStack.back(), id = idStack.back();
				if (type == kAnd) {
					op.yes = i + 2;
					op.no = closeIndex[id];
				} else if (type == kOr) {
					op.yes = closeIndex[id];
					op.no = i + 2;
				} else {
					op.yes = op.no = n;
				}
				++i;
			}
		}
		if (!found) {
			p.firstOp = n + 1;
			p.finalized = true;
			return;
		}
		// Pass 3: tests point straight at tests, ACCEPT or REJECT.
		const auto terminal = [&](std::uint32_t a_index) { return a_index >= ops.size() || IsTest(ops[a_index].word) || ops[a_index].word == 2 || ops[a_index].word == 3; };
		for (std::uint32_t i = p.firstOp; i < p.freeOp; ++i) {
			if (!IsTest(ops[i].word))
				continue;
			for (std::uint32_t guard = 0; !terminal(ops[i].yes) && guard < 4096; ++guard)
				ops[i].yes = ops[ops[i].yes].yes;
			for (std::uint32_t guard = 0; !terminal(ops[i].no) && guard < 4096; ++guard)
				ops[i].no = ops[ops[i].no].no;
			++i;
		}
		p.finalized = true;
	}

	void PortalViews::ReopenAsOr(Program& a_program)
	{
		// FUN_140e334c0: a stream that is one OR group drops its close; any other is wrapped in a new, unclosed OR. The scan
		// does not skip the set-index words (an index of 4, 5 or 6 reads as a token), as the engine's.
		auto& p = a_program;
		bool wrap = true;
		if (p.freeOp > 0 && p.ops[0].word == kOr) {
			std::int32_t depth = 1;
			std::uint32_t i = 1;
			do {
				if (i >= p.freeOp)
					break;
				const std::uint32_t w = p.ops[i].word;
				if (w > 3 && w < 6)
					++depth;
				else if (w == kClose)
					--depth;
				++i;
			} while (depth > 0);
			if (i == p.freeOp && depth == 0)
				wrap = false;
		}
		if (!wrap) {
			--p.freeOp;
			return;
		}
		if (p.ops.size() < p.freeOp + 4)
			p.ops.resize(p.freeOp + 16);
		for (std::uint32_t i = p.freeOp; i > 0; --i)
			p.ops[i].word = p.ops[i - 1].word;
		++p.freeOp;
		p.ops[0].word = kOr;
	}

	void PortalViews::CompactEmptyGroups(Program& a_program)
	{
		// FUN_140e33b90: one pass taking out adjacent {open, close} pairs.
		auto& p = a_program;
		const std::uint32_t original = p.freeOp;
		if (original == 0)
			return;
		if (p.ops.size() < original + 4)
			p.ops.resize(original + 16);
		const auto word = [&](std::uint32_t a_index) { return a_index < p.ops.size() ? p.ops[a_index].word : 0u; };
		std::uint32_t i = 0, k = 0, current = original;
		do {
			while (current > 0 && IsOpen(word(i + k)) && word(i + k + 1) == kClose) {
				current -= 2;
				k += 2;
				p.freeOp = current;
				if (!(i < current))
					break;
			}
			const std::uint32_t j = i + k;
			if (j < original) {
				p.ops[i].word = word(j);
				if (IsTest(p.ops[i].word)) {
					++i;
					p.ops[i].word = word(j + 1);
				}
			}
			current = p.freeOp;
			++i;
		} while (i < current);
	}

	void PortalViews::Merge(Program& a_destination, const Program& a_source)
	{
		// FUN_140e33130: the source's tokens and sets appended, its set indices rebased.
		auto& d = a_destination;
		d.finalized = false;
		if (d.ops.size() < d.freeOp + a_source.freeOp + 4)
			d.ops.resize(d.freeOp + a_source.freeOp + 16);
		if (d.sets.size() < d.freePlane + a_source.freePlane + 1)
			d.sets.resize(d.freePlane + a_source.freePlane + 4);
		for (std::uint32_t i = 0; i < a_source.freeOp; ++i)
			d.ops[d.freeOp + i].word = a_source.ops[i].word;
		for (std::uint32_t j = 0; j < a_source.freePlane; ++j)
			d.sets[d.freePlane + j] = a_source.sets[j];
		if (d.freePlane != 0)
			for (std::uint32_t i = d.freeOp; i < d.freeOp + a_source.freeOp; ++i)
				if (IsTest(d.ops[i].word)) {
					d.ops[i + 1].word += d.freePlane;
					++i;
				}
		d.freeOp += a_source.freeOp;
		d.freePlane += a_source.freePlane;
	}

	void PortalViews::CopyMasked(Program& a_out, const Program& a_parent, const void* a_portal)
	{
		// FUN_140e33740 (after Clear, FUN_140e32040): the parent's stream for a child room, each set whose planes the portal lies
		// wholly inside with no edge clipped turned off (mask 0) for it.
		auto& t = a_out;
		t.freePlane = t.freeOp = t.firstOp = 0;
		t.skipView = t.finalized = false;
		const std::uint32_t n = a_parent.freeOp;
		if (t.ops.size() < n + 4)
			t.ops.resize(n + 16);
		if (t.sets.size() < a_parent.freePlane + 1)
			t.sets.resize(a_parent.freePlane + 4);
		for (std::uint32_t i = 0; i < n; ++i) {
			const std::uint32_t w = a_parent.ops[i].word;
			t.ops[i].word = w;
			if (IsTest(w) && i + 1 < n) {
				const std::uint32_t s = a_parent.ops[i + 1].word;
				const PlaneSet& set = s < a_parent.sets.size() ? a_parent.sets[s] : PlaneSet{};
				const bool inside = PortalVsSetEdges(a_portal, set);
				// The set keeps its index (in a builder's stream the tests' sets ascend, so it is the running count too).
				t.ops[i + 1].word = t.freePlane;
				Grow(t, std::max(s, t.freePlane));
				t.sets[s] = set;
				++t.freePlane;
				const auto& f = edgeFlags[a_portal];
				if (inside && f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0)
					t.sets[s].mask = 0;
				++i;
			}
		}
		t.freeOp = n;
		CompactEmptyGroups(t);
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
		++parity.portals;
	}

	void PortalViews::AddPlane(Program& a_program, const void* a_plane)
	{
		// FUN_140e32a40: an occlusion plane, or with an accepted neighbour on the same side the linked pair.
		const void* links[4];
		for (std::size_t i = 0; i < 4; ++i)
			links[i] = At<const void*>(a_plane, kPlaneLinks[i]);
		const std::uint32_t type = (links[0] || links[2]) ? 2u : (links[1] || links[3]) ? 1u : 0u;
		const void* neighbour = type == 1 ? (links[1] ? links[1] : links[3]) : (links[0] ? links[0] : links[2]);
		const Vec3 eye{ a_program.eye[0], a_program.eye[1], a_program.eye[2] };
		if (neighbour) {
			const auto side = [&](const void* a_quad) {
				const auto c = Corners(a_quad);
				const auto p = PlaneFromPoints(c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z);
				const float d = Dist(p, eye.x, eye.y, eye.z);
				return d < 0.0f ? 2 : d > 0.0f ? 1 : 0;
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
				parity.planes += 2;
				return;
			}
		}
		const std::uint32_t s = a_program.freePlane;
		EmitTest(a_program, kOccluderTest, s);
		Grow(a_program, s) = PortalPlanes(a_plane, eye);
		++a_program.freePlane;
		++parity.planes;
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
			++parity.boxes;
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
		if (state == 0 && !WithinFrustumDistFirst(a_occluder, camera.view, camera.eye))
			return;
		if (a_needOpen) {
			Emit(a_program, kAnd);
			a_needOpen = false;
		}
		AddPlane(a_program, a_occluder);
	}

	void PortalViews::AddOccluderRoom(Program& a_program, const void* a_occluder, const Program* a_parent)
	{
		// FUN_140e32860: an occluder of a room reached through a portal. A plane only once accepted this frame: the engine resets
		// every state before its walk and nothing in it accepts a plane, so the room's planes take no part (as the engine's).
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
		frame.sceneFrame = SceneStore::Get().GetFrame();
		// Whatever this frame builds (nothing: no programs), the words FadeStateCS, BuildDraws and the checks read.
		struct EncodeOnExit
		{
			PortalViews& self;
			~EncodeOnExit() { self.Encode(self.encoded); }
		} encodeOnExit{ *this };
		const auto* sceneGraph = Global<const std::byte*>(kWorldSceneGraph);
		const auto* cam = sceneGraph ? At<const std::byte*>(sceneGraph, 0x128) : nullptr;
		const auto* sceneNode = Global<const std::byte*>(kShadowSceneNode);
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
		SunViews::Planes planes;
		SunViews::FrustumPlanes(frustum, world, planes);
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
		if (SunViews::ParityEnabled()) {
			CheckParity();
			if (++reportFrames >= 300) {
				reportFrames = 0;
				Report();
			}
		}
	}

	// ---- A shadow light's rooms (T5a3) ----

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
		const auto* sceneNode = Global<const std::byte*>(kShadowSceneNode);
		const void* graph = sceneNode ? At<const void*>(sceneNode, kSceneNodeGraph) : nullptr;
		const std::uint32_t roomCount = graph ? At<std::uint32_t>(graph, kGraphRoomCount) : 0;
		const auto* niLight = At<const std::byte*>(a_light, 0x48);
		a_rooms.walk = kLightUntouched;
		lightFlagsPending = false;
		if (!roomCount || !niLight)
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
		// near 0.1; its world-to-camera matrix and viewport as set up), read as a camera, as T2a and T3a read theirs. Before the
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
		Parity saved = std::exchange(parity, {});
		parabolic = &sphere;
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
		lightFlagsPending = true;
		parabolic = nullptr;
		parity = std::move(saved);
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
		if (const std::uintptr_t test = (*reinterpret_cast<const std::uintptr_t* const*>(a_roomProcess))[0xD8 / 8]; test != REL::Offset(0x151a120).address())
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

	void PortalViews::CheckParity()
	{
		// The engine's entry as Main::Update's walk left it (the list jobs, which add the portals' shared-node frustums, have not run).
		auto& p = parity;
		++p.frames;
		const auto* processes = Global<const std::byte* const*>(kListProcesses);
		const auto* entry = processes && processes[0] ? At<const std::byte*>(processes[0], kProcessEntry) : nullptr;
		if (!entry)
			return;
		// The eye the engine's walk measured from (its unbound frustum's, Reset at the walk's start: +0xA0): Main::Update's camera,
		// which the frame's draw may since have moved. A frame whose eye differs is tallied apart: its programs are a later camera's.
		const float* engineEye = &At<float>(entry + kEntryUnbound, 0xA0);
		const float moved = std::max({ std::abs(engineEye[0] - camera.eye.x), std::abs(engineEye[1] - camera.eye.y), std::abs(engineEye[2] - camera.eye.z) });
		// And the camera's world view planes it Reset with (+0x30): a camera turned since gives other side planes, eye unmoved.
		const auto& engineView = At<const PlaneSet>(entry + kEntryUnbound, 0x30);
		bool turned = false;
		for (std::uint32_t q = 0; q < 6 && !turned; ++q)
			for (std::uint32_t c = 0; c < 4; ++c)
				turned = turned || engineView.plane[q][c] != camera.view.plane[q][c];
		p.turned += turned ? 1 : 0;
		// The list process's own view planes (+0x3C, SetFrustum before the jobs), which its sphere test reads: DCLF's must be them.
		{
			const auto& listPlanes = At<const PlaneSet>(processes[0], 0x3C);
			bool differ = listPlanes.mask != camera.view.mask;
			for (std::uint32_t q = 0; q < 6 && !differ; ++q)
				for (std::uint32_t c = 0; c < 4; ++c)
					differ = differ || listPlanes.plane[q][c] != camera.view.plane[q][c];
			if (differ && p.planesDiffer++ == 0)
				p.planesFirst = fmt::format("mask {:#x} (the list's {:#x}), plane 0 ({} {} {} {}) against ({} {} {} {})", camera.view.mask, listPlanes.mask, camera.view.plane[0][0],
					camera.view.plane[0][1], camera.view.plane[0][2], camera.view.plane[0][3], listPlanes.plane[0][0], listPlanes.plane[0][1], listPlanes.plane[0][2],
					listPlanes.plane[0][3]);
		}
		const bool eyeMoved = moved != 0.0f || turned;
		p.eyeMoved += eyeMoved ? 1 : 0;
		p.largestMove = std::max(p.largestMove, moved);
		const auto note = [&](std::string a_what) {
			if (eyeMoved) {
				++p.movedDiffer;
				return;
			}
			if (p.first.empty())
				p.first = std::move(a_what);
		};
		const auto name = [](const void* a_room) {
			const auto* object = static_cast<const RE::NiAVObject*>(a_room);
			return a_room ? fmt::format("'{}'", object->name.c_str() ? object->name.c_str() : "") : std::string("unbound space");
		};
		// One program against an engine BSCompoundFrustum: the structure exactly, the planes of the active sets within rounding.
		const auto compare = [&](const Program& a_ours, const std::byte* a_engine, const void* a_room, bool a_tokensOnly) {
			++p.programs;
			p.ops += a_ours.freeOp;
			p.sets += a_ours.freePlane;
			const std::uint32_t freeOp = At<std::uint32_t>(a_engine, kFrustumFreeOp), freePlane = At<std::uint32_t>(a_engine, kFrustumFreePlane);
			const bool finalized = At<std::uint8_t>(a_engine, kFrustumFinalized) != 0;
			const auto* ops = At<const Op*>(a_engine, kFrustumOps);
			const auto* sets = At<const PlaneSet*>(a_engine, kFrustumSets);
			std::string why;
			if (freeOp != a_ours.freeOp || freePlane != a_ours.freePlane)
				why = fmt::format("{} ops {} sets, the engine's {} ops {} sets", a_ours.freeOp, a_ours.freePlane, freeOp, freePlane);
			else if ((At<std::uint8_t>(a_engine, kFrustumSkipView) != 0) != a_ours.skipView)
				why = fmt::format("skipViewFrustum {}, the engine's {}", a_ours.skipView, !a_ours.skipView);
			if (why.empty() && freeOp && ops) {
				const bool branches = !a_tokensOnly && finalized && a_ours.finalized;
				if (branches && At<std::uint32_t>(a_engine, kFrustumFirstOp) != a_ours.firstOp)
					why = fmt::format("first op {}, the engine's {}", a_ours.firstOp, At<std::uint32_t>(a_engine, kFrustumFirstOp));
				// The tokens (with the tests' set words), the tests' branches, and the root, ACCEPT and REJECT records after them.
				const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(branches ? freeOp + 3 : freeOp, a_ours.ops.size()));
				for (std::uint32_t i = 0; why.empty() && i < count; ++i) {
					const auto& a = a_ours.ops[i];
					const auto& b = ops[i];
					const bool test = IsTest(a.word) && i < freeOp;
					if (a.word != b.word || (branches && test && (a.yes != b.yes || a.no != b.no))) {
						why = fmt::format("op {}: {} -> {}/{}, the engine's {} -> {}/{}", i, a.word, a.yes, a.no, b.word, b.yes, b.no);
					} else if (test && i + 1 < freeOp) {
						++i;
						if (a_ours.ops[i].word != ops[i].word)
							why = fmt::format("op {}'s set {}, the engine's {}", i - 1, a_ours.ops[i].word, ops[i].word);
					}
				}
			}
			if (why.empty() && freePlane && sets) {
				for (std::uint32_t s = 0; why.empty() && s < freePlane && s < a_ours.sets.size(); ++s) {
					const auto& a = a_ours.sets[s];
					const auto& b = sets[s];
					if (a.mask != b.mask) {
						why = fmt::format("set {} mask {:#x}, the engine's {:#x}", s, a.mask, b.mask);
						break;
					}
					for (std::uint32_t q = 0; q < 6; ++q) {
						if (!((a.mask >> q) & 1))
							continue;
						for (std::uint32_t c = 0; c < 4; ++c) {
							const float d = std::abs(a.plane[q][c] - b.plane[q][c]);
							if (!SunViews::Close(a.plane[q][c], b.plane[q][c])) {
								why = fmt::format("set {} plane {} [{}] {}, the engine's {}", s, q, c, a.plane[q][c], b.plane[q][c]);
								break;
							}
							p.largest = std::max(p.largest, d);
						}
					}
				}
			}
			if (!why.empty()) {
				if (!eyeMoved)
					++(a_room ? p.programsDiffer : p.unboundDiffer);
				note(fmt::format("{}: {}", name(a_room), why));
			}
		};
		// The rooms the engine's map holds (its other keys are the portals' shared nodes).
		const auto* graph = frame.graph;
		const auto* graphRooms = graph ? At<const void* const*>(graph, kGraphRooms) : nullptr;
		const std::uint32_t graphRoomCount = graphRooms ? At<std::uint32_t>(graph, kGraphRoomCount) : 0;
		const auto isRoom = [&](const void* a_key) { return a_key && std::find(graphRooms, graphRooms + graphRoomCount, a_key) != graphRooms + graphRoomCount; };
		ankerl::unordered_dense::set<const void*> engineRooms;
		const std::uint32_t capacity = At<std::uint32_t>(entry, kEntryMapCapacity);
		const auto* entries = At<const std::byte*>(entry, kEntryMapEntries);
		for (std::uint32_t i = 0; entries && i < capacity; ++i) {
			const auto* slot = entries + std::size_t(i) * 0x18;
			if (!At<const void*>(slot, 0x10))
				continue;
			const void* key = At<const void*>(slot, 0);
			if (!isRoom(key) || !engineRooms.insert(key).second)
				continue;
			++p.rooms;
			const auto* value = At<const std::byte*>(slot, 8);
			const auto* ours = RoomProgram(key);
			if (!ours) {
				p.roomsMissing += eyeMoved ? 0 : 1;
				note(fmt::format("{}: in the engine's map, not reached", name(key)));
			} else if (value) {
				compare(*ours, value, key, false);
			}
		}
		for (const auto* key : frame.roomKeys)
			if (!engineRooms.contains(key)) {
				p.roomsExtra += eyeMoved ? 0 : 1;
				note(fmt::format("{}: reached, not in the engine's map", name(key)));
			}
		compare(frame.unbound, entry + kEntryUnbound, nullptr, true);
		// Unbound space's operator count against the engine's: each graph plane occluder, with the engine's state and edge flags and
		// DCLF's test of it again (WithinFrustumDistFirst's parts).
		if (!eyeMoved && graph && frame.unbound.freeOp != At<std::uint32_t>(entry + kEntryUnbound, kFrustumFreeOp) && p.occluderFirst.empty()) {
			ForList(graph, kGraphOccluders, [&](const void* a_occluder) {
				if (!a_occluder || !IsPlane(a_occluder))
					return;
				const auto c = Corners(a_occluder);
				const auto pl = PlaneFromPoints(c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z);
				const auto& e = camera.eye;
				const float dP = ((e.x * pl[0] + e.y * pl[1]) + e.z * pl[2]) - pl[3];
				const auto& n0 = camera.view.plane[0];
				const float dN = ((n0[0] * e.x + n0[1] * e.y) + n0[2] * e.z) - n0[3];
				Flags scratch{};
				const bool quad = QuadVsSet(camera.view, c, scratch);
				const auto st = states.find(a_occluder);
				p.occluderFirst += fmt::format("[plane {} engine state {} flags {:#x}, DCLF's state {}; dP {} dN {} quad {} half {}x{}] ", a_occluder, At<std::uint32_t>(a_occluder, 0x40),
					At<std::uint32_t>(a_occluder, 0xF0), st == states.end() ? -1 : static_cast<int>(st->second), dP, dN, quad, At<float>(a_occluder, kShapeHalfX),
					At<float>(a_occluder, kShapeHalfY));
			});
		}
		// The boxes this frame used: their corners (+0x134) and silhouette sets (+0x54, +0xC4) as the engine keeps them, against DCLF's.
		for (const auto& [box, scratch] : boxes) {
			const auto corners = BoxCorners(box);
			float cornerMove = 0.0f;
			for (std::uint32_t i = 0; i < 8; ++i) {
				const float* engineCorner = &At<float>(box, 0x134 + 12 * i);
				cornerMove = std::max({ cornerMove, std::abs(engineCorner[0] - corners[i].x), std::abs(engineCorner[1] - corners[i].y), std::abs(engineCorner[2] - corners[i].z) });
			}
			++p.boxChecks;
			if (cornerMove != 0.0f) {
				++p.boxCornersDiffer;
				p.largestCorner = std::max(p.largestCorner, cornerMove);
			}
			const auto st = states.find(box);
			if (st == states.end() || st->second != 1 || eyeMoved)
				continue;
			for (std::uint32_t s = 0; s < 2; ++s) {
				const auto& ours = scratch.set[s];
				const auto& theirs = At<const PlaneSet>(box, s ? 0xC4 : 0x54);
				std::string why;
				if (ours.mask != theirs.mask)
					why = fmt::format("mask {:#x}, the engine's {:#x}", ours.mask, theirs.mask);
				for (std::uint32_t q = 0; q < 6 && why.empty(); ++q)
					if ((ours.mask >> q) & 1)
						for (std::uint32_t c = 0; c < 4 && why.empty(); ++c)
							if (ours.plane[q][c] != theirs.plane[q][c])
								why = fmt::format("plane {} [{}] {}, the engine's {} (hull {}, corners moved {})", q, c, ours.plane[q][c], theirs.plane[q][c], scratch.hullCount,
									cornerMove);
				if (!why.empty()) {
					++p.boxSetsDiffer;
					if (p.boxFirst.empty())
						p.boxFirst = fmt::format("box {} set {}: {}", box, s, why);
				}
			}
		}
		// The rooms the walk popped, in order, and whether it reached unbound space.
		const auto* accum = At<const void* const*>(entry, kEntryAccum);
		const std::uint32_t accumSize = At<std::uint32_t>(entry, kEntryAccumSize);
		const bool sameAccum = accumSize == frame.accumulated.size() && (accumSize == 0 || std::equal(frame.accumulated.begin(), frame.accumulated.end(), accum));
		if (!sameAccum || (At<std::uint8_t>(entry, kEntryVisibleUnbound) != 0) != frame.visibleUnbound) {
			p.accumDiffer += eyeMoved ? 0 : 1;
			note(fmt::format("the walk popped {} rooms (unbound {}), the engine's {} (unbound {})", frame.accumulated.size(), frame.visibleUnbound, accumSize,
				At<std::uint8_t>(entry, kEntryVisibleUnbound) != 0));
		}
	}

	void PortalViews::Encode(std::vector<std::uint32_t>& a_words) const
	{
		a_words.clear();
		const auto* view = reinterpret_cast<const std::uint32_t*>(&camera.view);
		a_words.insert(a_words.end(), view, view + kPortalSetBytes / 4);
		const std::size_t header = a_words.size();
		const auto* graph = frame.valid ? frame.graph : nullptr;
		const auto* graphRooms = graph ? At<const void* const*>(graph, kGraphRooms) : nullptr;
		const std::uint32_t roomCount = graphRooms ? At<std::uint32_t>(graph, kGraphRoomCount) : 0;
		const std::uint32_t count = graph ? 1 + roomCount : 0;
		a_words.insert(a_words.end(), { count, 0u, 0u, 0u });
		const std::size_t directory = a_words.size();
		a_words.resize(directory + std::size_t(count) * (kPortalDirectoryBytes / 4), 0u);
		const auto programAt = [&](std::uint32_t a_index) -> const Program* {
			if (!a_index)
				return &frame.unbound;
			const void* room = graphRooms[a_index - 1];
			return room ? RoomProgram(room) : nullptr;
		};
		// The directory: each program's records and sets after the last's.
		std::uint32_t records = 0, sets = 0;
		for (std::uint32_t p = 0; p < count; ++p) {
			const void* room = p ? graphRooms[p - 1] : nullptr;
			const Program* program = programAt(p);
			auto* entry = a_words.data() + directory + std::size_t(p) * (kPortalDirectoryBytes / 4);
			// A room's own cull mode (BSMultiBoundRoom::OnVisible gives it to the process for its children); unbound space the lists'.
			entry[6] = room ? static_cast<std::uint32_t>(static_cast<const RE::BSMultiBoundNode*>(room)->GetRuntimeData().cullingMode) : 0u;
			if (!program)
				continue;
			// The lists' copy of unbound space's frustum drops skipViewFrustum; a room's is installed as it is.
			entry[0] = kPortalReached | (p && program->skipView ? kPortalSkipView : 0u);
			const std::uint32_t ops = program->freeOp && program->finalized ? std::min<std::uint32_t>(program->freeOp + 3, static_cast<std::uint32_t>(program->ops.size())) : 0u;
			const std::uint32_t planes = ops ? std::min<std::uint32_t>(program->freePlane, static_cast<std::uint32_t>(program->sets.size())) : 0u;
			entry[1] = ops;
			entry[2] = planes;
			entry[3] = program->firstOp;
			entry[4] = records;
			entry[5] = sets;
			records += ops;
			sets += planes;
		}
		a_words[header + 1] = records;
		a_words[header + 2] = sets;
		// The records, then the sets.
		std::vector<std::uint32_t> setWords;
		for (std::uint32_t p = 0; p < count; ++p) {
			const std::uint32_t ops = a_words[directory + std::size_t(p) * (kPortalDirectoryBytes / 4) + 1];
			const std::uint32_t planes = a_words[directory + std::size_t(p) * (kPortalDirectoryBytes / 4) + 2];
			if (!ops)
				continue;
			const Program& program = *programAt(p);
			for (std::uint32_t i = 0; i < ops; ++i)
				a_words.insert(a_words.end(), { program.ops[i].word, program.ops[i].yes, program.ops[i].no, 0u });
			for (std::uint32_t s = 0; s < planes; ++s) {
				const auto* words = reinterpret_cast<const std::uint32_t*>(&program.sets[s]);
				setWords.insert(setWords.end(), words, words + kPortalSetBytes / 4);
			}
		}
		a_words.insert(a_words.end(), setWords.begin(), setWords.end());
	}

	int PortalViews::Visible(const std::vector<std::uint32_t>& a_words, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags,
		std::string& a_why)
	{
		constexpr std::size_t kHeader = kPortalSetBytes / 4, kDirectory = kPortalHeaderBytes / 4, kEntry = kPortalDirectoryBytes / 4, kOp = kPortalOpBytes / 4,
							  kSet = kPortalSetBytes / 4;
		const auto word = [&](std::size_t a_index) { return a_index < a_words.size() ? a_words[a_index] : 0u; };
		const auto plane = [&](std::size_t a_index) {
			std::array<float, 4> p;
			for (std::size_t c = 0; c < 4; ++c)
				p[c] = std::bit_cast<float>(word(a_index + c));
			return p;
		};
		const std::uint32_t count = word(kHeader);
		if (a_program == kPortalNoProgram || a_program >= count)
			return a_why = "no program", -1;
		const std::size_t entry = kDirectory + std::size_t(a_program) * kEntry;
		const std::uint32_t flags = word(entry), ops = word(entry + 1), sets = word(entry + 2), firstOp = word(entry + 3);
		const std::uint32_t firstRecord = word(entry + 4), firstSet = word(entry + 5), cullMode = word(entry + 6);
		const bool alwaysDraw = (a_nodeFlags >> 11) & 1, preprocessed = (a_nodeFlags >> 12) & 1, hidden = (a_nodeFlags >> 20) & 1;
		if (a_radius == 0.0f && !alwaysDraw)
			return a_why = "zero radius", 0;
		if (a_program != 0 && !(flags & kPortalReached))
			return a_why = fmt::format("room {} not reached", a_program - 1), 0;
		if (cullMode == 2)
			return a_why = "cull mode 2", 0;
		if (cullMode == 1 || alwaysDraw)
			return a_why = "always", 1;
		if (preprocessed && cullMode != 4)
			return a_why = "preprocessed", hidden ? 0 : 1;
		const auto distance = [&](const std::array<float, 4>& p) { return ((p[1] * a_centre[1] + p[0] * a_centre[0]) + a_centre[2] * p[2]) - p[3]; };
		bool view = true;
		const std::uint32_t viewMask = word(24);
		for (std::uint32_t p = 0; p < 6 && view; ++p)
			if (((viewMask >> p) & 1) && distance(plane(std::size_t(p) * 4)) <= -a_radius)
				view = false, a_why = fmt::format("view plane {}", p);
		if (cullMode == 3 || ops == 0)
			return view ? (a_why = "view", 1) : 0;
		if (!(flags & kPortalSkipView) && !view)
			return 0;
		const std::size_t records = kDirectory + std::size_t(count) * kEntry + std::size_t(firstRecord) * kOp;
		const std::size_t setsBase = kDirectory + std::size_t(count) * kEntry + std::size_t(word(kHeader + 1)) * kOp + std::size_t(firstSet) * kSet;
		std::uint32_t op = firstOp;
		for (std::uint32_t step = 0; step < 512; ++step) {
			if (op >= ops)
				return a_why = fmt::format("program {} ran off at op {}", a_program, op), 1;
			const std::uint32_t type = word(records + std::size_t(op) * kOp), yes = word(records + std::size_t(op) * kOp + 1), no = word(records + std::size_t(op) * kOp + 2);
			if (type == 2)
				return a_why = fmt::format("program {} accepted at op {}", a_program, op), 1;
			if (type == 3)
				return a_why = fmt::format("program {} rejected at op {}", a_program, op), 0;
			bool result = false;
			if (type == 7 || type == 8) {
				const std::uint32_t set = op + 1 < ops ? word(records + std::size_t(op + 1) * kOp) : ~0u;
				if (set >= sets)
					return a_why = fmt::format("program {} set {} past {}", a_program, set, sets), 1;
				const std::size_t base = setsBase + std::size_t(set) * kSet;
				const std::uint32_t mask = word(base + 24);
				if (!mask) {
					result = type == 7;
				} else {
					std::uint32_t p = 0;
					for (; p < 6; ++p) {
						if (!((mask >> p) & 1))
							continue;
						const float d = distance(plane(base + std::size_t(p) * 4));
						if (d <= -a_radius)
							break;
						if (type == 8 && d < a_radius)
							break;
					}
					result = type == 7 ? p == 6 : p != 6;
				}
			}
			op = result ? yes : no;
		}
		return a_why = "program loop", 1;
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

	void PortalViews::NoteStructure(const RE::NiNode* a_parent)
	{
		static const std::uintptr_t roomVtable = RE::VTABLE_BSMultiBoundRoom[0].address();
		if (a_parent && *reinterpret_cast<const std::uintptr_t*>(a_parent) == roomVtable)
			structure.fetch_add(1, std::memory_order_relaxed);
	}

	void PortalViews::Report()
	{
		auto& p = parity;
		const std::uint64_t differ = p.roomsMissing + p.roomsExtra + p.programsDiffer + p.unboundDiffer + p.accumDiffer;
		logger::info("[DCLF] portal views (T5a: DCLF's walk of the portal graph, against the engine's entry): {} frames ({} built, {} without a graph or camera), {} rooms in the "
					 "engine's maps ({} not reached, {} reached only by DCLF), {} programs ({} rooms', {} unbound space's differ), {} walks' room lists differ; programs held {} ops, "
					 "{} sets ({} portals, {} occlusion planes, {} boxes); largest plane difference {:.3g}; {} frames the camera moved after the engine's walk (by up to "
					 "{:.3g}, {} of them turned: {} differences there, not counted); boxes: {} checked, {} with other corners (by up to {:.3g}), {} sets differ; view planes other than the list process's on {} frames{}{}{}{}{}",
			p.frames, built, skipped, p.rooms, p.roomsMissing, p.roomsExtra, p.programs, p.programsDiffer, p.unboundDiffer, p.accumDiffer, p.ops, p.sets, p.portals, p.planes, p.boxes,
			p.largest, p.eyeMoved, p.largestMove, p.turned, p.movedDiffer, p.boxChecks, p.boxCornersDiffer, p.largestCorner, p.boxSetsDiffer, p.planesDiffer, p.planesFirst.empty() ? "" : " (first: " + p.planesFirst + ")", p.occluderFirst.empty() ? "" : "; unbound space's occluders: " + p.occluderFirst,
			p.boxFirst.empty() ? "" : "; first box: " + p.boxFirst, differ ? " <- PORTAL VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		p = {};
		built = skipped = 0;
	}
}
