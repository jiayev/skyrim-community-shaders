#pragma once

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramModel.h"

#include <array>
#include <cmath>
#include <cstdint>

// The engine's portal graph and occlusion shapes as PortalViews reads them (AE 1.6.1170): the layouts, and the arithmetic in the engine's
// grouping (skyrim-engine-notes.md, "Portals, rooms and occluders"). PortalViews builds its programs from them; its observer compares them.

namespace DCLF::PortalShapes
{
	using Engine::At;

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

	// A light's room process's camera: the NiCamera constructor's (FUN_140d2afa0) frustum and viewport, and the
	// world-to-camera matrix FUN_140d2c910 makes of them at the identity (nothing updates it after).
	constexpr float kLightWorldToCam[16]{ 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, -2.0f, 1.0f, 0.0f, 0.0f, 0.0f };
	constexpr float kLightFrustum[6]{ -0.5f, 0.5f, 0.5f, -0.5f, 1.0f, 2.0f };
	constexpr float kLightViewport[4]{ 0.0f, 1.0f, 1.0f, 0.0f };

	// The token words and the plane arithmetic are the program model's (PortalProgramModel.h).
	using PortalModel::kAnd;
	using PortalModel::kClose;
	using PortalModel::kOccluderTest;
	using PortalModel::kOr;
	using PortalModel::kPortalTest;
	using PortalModel::IsOpen;
	using PortalModel::IsTest;
	using PortalModel::Dist;
	using PortalModel::PlaneFromPoints;

	/** @brief BSOcclusionShape vfunc 0x25: a plane (or portal), else a box. */
	inline bool IsPlane(const void* a_shape) { return static_cast<const RE::BSOcclusionShape*>(a_shape)->IsOcclusionPlane(); }

	inline float M(const void* a_shape, std::uint32_t a_row, std::uint32_t a_column) { return At<float>(a_shape, kShapeRotation + 4 * (3 * a_row + a_column)); }

	template <class F>
	void ForList(const void* a_owner, std::size_t a_head, F&& a_each)
	{
		for (const auto* node = At<const std::byte*>(a_owner, a_head); node; node = At<const std::byte*>(node, 0))
			a_each(At<const void*>(node, kListNodeItem));
	}

	inline std::uint32_t ListCount(const void* a_owner, std::size_t a_head) { return At<std::uint32_t>(a_owner, a_head + kListCount); }

	/** @brief FUN_140e16f70's and the shapes' camera: a world position. */
	struct V
	{
		float x, y, z;
	};
	inline V Sub(const V& a, const V& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline V Add(const V& a, const V& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline V Scale(const V& a, float s) { return { a.x * s, a.y * s, a.z * s }; }

	/** @brief FUN_140e209e0: the box's eight corners from its centre, rotation columns and extents. */
	inline std::array<V, 8> BoxCorners(const void* a_box)
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
}
