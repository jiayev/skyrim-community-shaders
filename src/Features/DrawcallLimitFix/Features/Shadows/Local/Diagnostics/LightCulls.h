#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace DCLF::Features::Shadows::Local::LightCulls
{
	/**
	 * @brief The point lights' culls' parity (CS_DCLF_PERSISTENT_PARITY): on the frames the culls skip nothing (Policy::FilterLightCulls),
	 * a paraboloid pass the registration did not withhold under an excluded entry is one the skip would lose (<- LIGHT EXCLUSION, by the
	 * object's shadow reject, the first named), a caster under a filtered category node must hang from a child the filter keeps
	 * (<- LIGHT FILTER), a cut entry's geometry's point light bits are compared with the engine's (<- LIGHT BITS), and so are the bits the
	 * empty culls would give every geometry (<- EMPTY CULL BITS: an engine bit the lights' spheres do not give). Subscribed by the
	 * feature's install.
	 */
	void Install();

	/** @brief The counts since the last Take (render thread, the report; the registration threads count). */
	struct Counts
	{
		std::uint64_t frames = 0, passes = 0, lost = 0, filterChecked = 0, filterMissed = 0;
		std::uint64_t compared = 0, agree = 0, engineOnly = 0, dclfOnly = 0, unhandled = 0;
		// The empty culls' bits (every light's predicted sphere) against the engine's, at each geometry's first clearing main registration.
		std::uint64_t sphereCompared = 0, sphereAgree = 0, sphereEngineOnly = 0, sphereDclfOnly = 0;
		std::string sphereFirstEngineOnly;
		std::array<std::uint64_t, 4> engineOnlyBy{}, dclfOnlyBy{};  // by the differing light's reach (untested, sphere, portals, not reached)
		std::array<std::uint64_t, 16> lostByReject{};
		std::string firstLost, filterFirstMissed;
	};
	Counts Take();
}
