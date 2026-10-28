#include "Anchor.h"
#include "RE/B/bhkWorld.h"
#include "RE/H/hkpWorld.h"
#include "RE/H/hkpWorldRayCastInput.h"
#include "RE/H/hkpWorldRayCastOutput.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/T/TESObjectCELL.h"

namespace EnvironmentProbe
{
	namespace
	{
		constexpr float clearance = 8.0f;
		struct Ray
		{
			float fraction = 1;
			bool inside = false;
		};
		Ray Trace(const RE::hkpWorld& world, const RE::CFilter& filter, const RE::NiPoint3& from, const RE::NiPoint3& to)
		{
			if ((to - from).SqrLength() < 0.0001f)
				return {};
			const auto scale = RE::bhkWorld::GetWorldScale();
			RE::hkpWorldRayCastInput input{};
			input.from = RE::hkVector4(from * scale);
			input.to = RE::hkVector4(to * scale);
			input.filterInfo = filter;
			RE::hkpWorldRayCastOutput output{};
			world.CastRay(input, output);
			if (!output.HasHit())
				return {};
			return { output.hitFraction, output.normal.Dot3(RE::hkVector4(to - from)) > 0.001f };
		}
		bool HasClearance(const RE::hkpWorld& world, const RE::CFilter& filter, const RE::NiPoint3& position)
		{
			// Only six short physics rays; no render-scene walking. These guard
			// nearby walls/ceilings, not arbitrary non-colliding render meshes.
			for (uint32_t axis = 0; axis < 3; ++axis) {
				for (float sign : { -1.0f, 1.0f }) {
					RE::NiPoint3 offset{};
					offset[axis] = sign * clearance;
					const auto hit = Trace(world, filter, position, position + offset);
					if (hit.inside || hit.fraction < 1)
						return false;
				}
			}
			return true;
		}
	}

	bool Anchor::Update(const RE::NiPoint3& camera, const RE::NiPoint3& forward, float distance, float delta, uint32_t frame, bool& discontinuity)
	{
		discontinuity = false;
		if (!std::isfinite(camera.x) || !std::isfinite(camera.y) || !std::isfinite(camera.z))
			return false;
		const bool teleport = initialized && (camera - lastCamera).Length() > std::max(256.0f, distance * 4);
		const auto desired = camera + forward * distance;
		if (!initialized || teleport || distance <= 0) {
			discontinuity = !initialized || teleport;
			position = lastCamera = camera;
			lastTarget = desired;
			initialized = true;
			settled = false;
			return true;
		}
		const float cameraMotion = (camera - lastCamera).Length();
		lastCamera = camera;
		const float separation = (camera - position).Length();
		const float targetMotion = (desired - lastTarget).Length();
		lastTarget = desired;
		// Stationary views do no collision work except a low-frequency check for
		// doors/dynamic geometry. Small view jitter stays inside the dead zone.
		const float deadZone = std::max(2.0f, distance * 0.25f);
		if (settled && cameraMotion < 0.01f && targetMotion < 0.01f && (frame & 7u) != 0)
			return true;
		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* cell = player ? player->GetParentCell() : nullptr;
		auto* physics = cell ? cell->GetbhkWorld() : nullptr;
		const auto* world = physics ? physics->GetWorld1() : nullptr;
		if (!world)
			return false;
		RE::CFilter playerFilter{};
		player->GetCollisionFilterInfo(playerFilter);
		RE::CFilter filter{};
		filter.SetCollisionLayer(RE::COL_LAYER::kCameraPick);
		filter.SetSystemGroup(playerFilter.GetSystemGroup());
		// Group zero bypasses the engine's layer filtering entirely. Using the
		// player's group also excludes our own body, including the first-person
		// camera inside its character capsule, while other actors still obstruct.
		// TES::Pick / bhkWorld::PickObject may skip queries once the engine's
		// picking budget is exhausted. That is not evidence of an obstruction.
		// Our bounded camera queries use Havok directly, under one world read lock.
		const RE::BSReadLockGuard lock(physics->worldLock);
		if (separation > 0.01f) {
			const auto connection = Trace(*world, filter, camera, position);
			if (connection.inside || connection.fraction < 1) {
				// A room boundary is a real discontinuity. Do not interpolate through
				// its wall or combine old-room octants with the new capture.
				position = camera;
				discontinuity = true;
				return true;
			}
		}
		// Prefer free space in front of the camera: the receiving surfaces for
		// both SSR and SSGI are in this region. Stop before the first obstacle,
		// never place the anchor on the far side of a wall to gain visibility.
		const auto obstruction = Trace(*world, filter, camera, desired);
		if (obstruction.inside)
			return false;
		const float reach = obstruction.fraction < 1 ? std::max(0.0f, distance * obstruction.fraction - clearance) : distance;
		const auto target = camera + forward * reach;
		const auto offset = target - position;
		const float travel = offset.Length();
		// Hysteresis and exponential following avoid orbiting with tiny camera
		// movements and abrupt octant changes after a turn.
		RE::NiPoint3 next = position;
		if (travel > deadZone) {
			const float response = 1.0f - std::exp(-12.0f * std::clamp(delta, 0.0f, 0.1f));
			next += offset * ((travel - deadZone) / travel * response);
			const auto hit = Trace(*world, filter, position, next);
			if (hit.inside || hit.fraction < 1)
				next = position;
		}
		if (HasClearance(*world, filter, next)) {
			settled = (next - position).Length() < 0.05f;
			position = next;
		} else if (HasClearance(*world, filter, camera)) {
			// The collision-resolved world camera is our safe seed. A blocked
			// anchor is discarded rather than capturing from inside the obstacle.
			position = camera;
			discontinuity = true;
		} else {
			return false;
		}
		return true;
	}
}
