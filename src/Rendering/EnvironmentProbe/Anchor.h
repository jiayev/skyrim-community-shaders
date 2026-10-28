#pragma once
#include <RE/N/NiPoint3.h>

namespace EnvironmentProbe
{
	/** World-space hysteresis with bounded local collision queries; no scene traversal or GPU readback. */
	class Anchor
	{
	public:
		void Reset() { initialized = false; }
		bool Update(const RE::NiPoint3& camera, const RE::NiPoint3& forward, float distance, float delta, uint32_t frame, bool& discontinuity);
		const RE::NiPoint3& Position() const { return position; }

	private:
		RE::NiPoint3 position{};
		RE::NiPoint3 lastCamera{};
		RE::NiPoint3 lastTarget{};
		bool initialized = false;
		bool settled = false;
	};
}
