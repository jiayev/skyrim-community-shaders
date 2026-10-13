#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics
{
	/**
	 * @brief What this run observes: the subjects the parity checks and the diagnostics watch, from the `CS_DCLF_*` switches.
	 *
	 * The one place the main path learns which observers run. Read once, on first use (before the first frame), so nothing
	 * changes under a running frame. Each field names a subject; its comment names the switch (Switches.cpp holds the
	 * descriptions). Observers that change what the main path does are not here: they are questions in Policy.h, whose
	 * answers with every switch off are the normal path's.
	 */
	struct Observation
	{
		// The parity checks: a live path against a reference built from scratch, each mismatch logged with a `<-` flag.
		bool scene = false;          // CS_DCLF_PERSISTENT_PARITY: every kept store (slots, logs, records, bindings, shading, kept builds) against a rebuild
		bool claims = false;         // CS_DCLF_SET_PARITY: the claims against what the engine registers, the GPU's drawn bits against the builds
		bool claimsOrScene = false;  // either of the two: the observers both share (LOD gate coverage, the shadow, light and reflection views, prefetched buffers)
		bool walk = false;           // CS_DCLF_WALK_PARITY: the delta walk against a dense walk
		bool walkOrScene = false;    // either of the two: the hidden witness
		bool changeLog = false;      // CS_DCLF_CHANGE_LOG_PARITY: the change log against a diff of the tables
		bool residents = false;      // CS_DCLF_RESIDENT_PARITY: the resident records against their passes built again
		bool fade = false;           // CS_DCLF_FADE_PARITY: the fade port against the engine's functions, FadeStateCS against the port
		bool builds = false;         // CS_DCLF_BUILD_PARITY: BuildDraws' output and the index ranges against the CPU's
		bool mirror = false;         // CS_DCLF_MIRROR_PARITY: the scene mirror's reads against the live objects
		bool capture = false;        // CS_DCLF_CAPTURE_PARITY: the tables against the engine's own lighting draws
		bool engineDrawsAll = false; // CS_DCLF_PARITY_BOTH: nothing is withheld from the engine (Policy::WithholdFromEngine)
		bool capturePoint = false;   // CS_DCLF_CAPTURE_POINT_PARITY: the colour epoch's captured bindings against the first native lighting draw
		bool skylight = false;       // CS_DCLF_SKYLIGHT_PARITY: DCLF's occlusion maps against the engine's, texel by texel
		bool accumulated = false;    // CS_DCLF_ACCUMULATED_PARITY: the decoded visibility and the animation gate against the engine's
		bool foliage = false;        // CS_DCLF_FOLIAGE_PARITY (1, wind or land): the alpha-tested colour draws' pixels against the frame before's
		bool foliageFrozen = false;  // ... with tree wind frozen (1 or land)
		bool foliageLand = false;    // ... and every colour draw records its pixels (land)

		// The diagnostics: statistics, probes and watches.
		bool stats = false;            // CS_DCLF_STATS: the periodic report
		bool passStats = false;        // CS_DCLF_PASS_STATS: per-pass counts
		bool profile = false;          // CS_DCLF_PROFILE: scene-phase timings
		bool releaseGuard = false;     // CS_DCLF_RELEASE_GUARD: the engine's last releases on DCLF's threads
		std::string targetProbe;       // CS_DCLF_TARGET_PROBE: x,y
	};

	/** @brief The run's observation, read from the switches on first use. */
	const Observation& Observe();

	/**
	 * @brief The parity checks' schedule: every 60 frames (or scene passes), each check at its own offset so their rebuilds do not
	 * share a frame.
	 */
	constexpr bool ParityDue(std::uint64_t a_serial, std::uint32_t a_offset = 0) { return a_serial % 60 == a_offset; }
}
