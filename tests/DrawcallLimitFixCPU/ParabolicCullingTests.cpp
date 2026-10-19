#include "Features/DrawcallLimitFix/Features/Portals/ParabolicCulling.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <random>

namespace
{
	using namespace DCLF::Portals::Parabolic;
	using Sphere = std::array<float, 4>;

	// Independent transcription of FUN_14151a1e0, including its sign-code branch and unordered comparisons.
	std::uint32_t NativeSphere(const Sphere& bound, const Sphere& sphere, const Sphere& plane, bool back)
	{
		const float x = bound[0] - sphere[0], y = bound[1] - sphere[1], z = bound[2] - sphere[2];
		std::uint32_t result = 0;
		if ((std::sqrt(x * x + y * y + z * z) - bound[3]) - sphere[3] < 0) {
			const float d = (bound[1] * plane[1] + bound[0] * plane[0] + bound[2] * plane[2]) - plane[3];
			if (d - bound[3] < 0)
				return back ? 3 : 1;
			const auto sign = 0.0f <= d ? std::uint32_t(0.0f < d) : 2u;
			result = (sign != 1) + 1;
			if (!back && sign != 1)
				result = 0;
		}
		return result;
	}

	Decision NativeProcess(std::uint32_t mode, std::uint32_t flags, bool ignore, bool compound, bool skip, bool pass, std::uint32_t sphere)
	{
		Decision out;
		if (mode - 1u < 2u) {
			if (mode != 1)
				return out;
		} else {
			if ((flags & kPreprocessed) == 0 || ignore) {
				if (!compound || mode == 3) {
					if ((flags & kAlwaysDraw) == 0)
						return { sphere != 0, sphere, 1, 0 };
				} else if ((flags & kAlwaysDraw) == 0) {
					if (!skip) {
						if (sphere != 0 && pass)
							out = { 1, sphere, 1, 0 };
					} else if (pass) {
						out = { sphere != 0, sphere, 1, 0 };
					}
					return out;
				}
			} else if (flags & kHidden) {
				return out;
			}
		}
		out.visible = 1;
		return out;
	}

	void Boundaries()
	{
		const Sphere light{ 0, 0, 0, 10 }, plane{ 1, 0, 0, 0 };
		assert(SphereMask({ 11, 0, 0, 1 }, light, plane, true) == 0);  // Tangency is excluded, not <=.
		assert(SphereMask({ std::nextafter(11.0f, 0.0f), 0, 0, 1 }, light, plane, true) != 0);
		assert(SphereMask({ 0, 0, 0, 0 }, light, plane, false) == 0);
		assert(SphereMask({ 0, 0, 0, 0 }, light, plane, true) == 2);
		assert(SphereMask({ -1, 0, 0, 0 }, light, plane, false) == 1);
		assert(SphereMask({ -1, 0, 0, 0 }, light, plane, true) == 3);
		assert(SphereMask({ 1, 0, 0, 0 }, light, plane, true) == 1);
		const auto nan = std::numeric_limits<float>::quiet_NaN();
		for (const auto back : { false, true })
			for (float x : { -5.0f, -1.0f, -0.0f, 0.0f, 1.0f, 5.0f, nan })
				for (float radius : { 0.0f, 1.0f, nan }) {
					const Sphere bound{ x, 0, 0, radius };
					assert(SphereMask(bound, light, plane, back) == NativeSphere(bound, light, plane, back));
				}
		assert(SphereMask({ 1, 0, 0, 0 }, light, { nan, 0, 0, 0 }, true) == 2);
	}

	void Differential()
	{
		std::mt19937 random(0x519a90);
		std::uniform_real_distribution<float> value(-64, 64), radius(0, 64);
		for (std::uint32_t i = 0; i < 100000; ++i) {
			const Sphere bound{ value(random), value(random), value(random), i % 13 ? radius(random) : 0 };
			const Sphere light{ value(random), value(random), value(random), radius(random) };
			const Sphere plane{ value(random), value(random), value(random), value(random) };
			const bool back = (i & 1) != 0;
			assert(SphereMask(bound, light, plane, back) == NativeSphere(bound, light, plane, back));
		}
		for (std::uint32_t mode = 0; mode < 5; ++mode)
			for (std::uint32_t bits = 0; bits < 128; ++bits)
				for (std::uint32_t mask = 0; mask < 4; ++mask) {
					const auto flags = ((bits & 1) ? kAlwaysDraw : 0u) | ((bits & 2) ? kPreprocessed : 0u) | ((bits & 4) ? kHidden : 0u);
					const bool ignore = bits & 8, compound = bits & 16, skip = bits & 32, pass = bits & 64;
					assert(Process(mode, flags, ignore, compound, skip, pass, mask) == NativeProcess(mode, flags, ignore, compound, skip, pass, mask));
				}
	}

	void StateEffects()
	{
		// Mode 4 respects preprocessing before always-draw; this deliberately differs from the main-camera wrapper.
		assert(Process(4, kPreprocessed | kHidden | kAlwaysDraw, false, true, false, true, 3).visible == 0);
		assert(Process(4, kPreprocessed | kHidden | kAlwaysDraw, true, true, false, true, 3).visible == 1);
		assert(Process(1, kPreprocessed | kHidden, false, true, false, false, 0).visible == 1);
		assert(Process(2, kAlwaysDraw, false, false, false, true, 3).visible == 0);
		std::uint32_t state = 3;
		const auto apply = [&](Decision decision) {
			if (decision.writesHemispheres)
				state = decision.hemispheres;
		};
		apply(Process(4, 0, false, true, false, false, 1));  // Rejected compound does not overwrite the process mask.
		assert(state == 3);
		apply(Process(3, 0, false, true, false, false, 0));  // Rejected sphere through FUN_141519fe0 does overwrite it.
		assert(state == 0);
		apply(Process(4, kAlwaysDraw, false, true, false, true, 2));
		assert(state == 0);  // Untested visits preserve even a previous sibling's zero mask.
		apply(Process(4, 0, false, true, true, true, 2));
		assert(state == 2);
		apply(Process(1, 0, false, true, true, false, 3));
		assert(state == 2);
		const auto failed = Process(4, 0, false, true, true, true, 0);
		assert(failed.visible == 0 && failed.writesHemispheres == 1);
	}
}

int main()
{
	Boundaries();
	Differential();
	StateEffects();
	std::cout << "Parabolic culling reference, boundaries and state effects passed\n";
}
