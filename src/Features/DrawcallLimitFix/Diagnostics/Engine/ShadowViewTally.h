#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics
{
	/**
	 * @brief The shadow views' parity (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY): DCLF's views (the sources' boards, Features/Shadows' Sun, Lights, Focus:
	 * from the cameras the engine's UpdateCamera set up) against the engine's captures - the view-projection, eye, viewport and caster
	 * volume at each view's draw (the shadow epochs' checks, Diagnostics/Draws/ShadowChecks.cpp), the sun's cascade planes at each cascade
	 * cull, the full-frustum processes (the sun source's observers). One tally per kind of view; each kind's line every 300 frames that
	 * had such views, by its source's observers (Features/Shadows/Sun/Diagnostics/SunViewParity.cpp, Features/Shadows/Local/Diagnostics/
	 * LightParity.cpp, FocusParity.cpp).
	 */
	struct ViewTally
	{
		std::uint64_t views = 0, viewProj = 0, eye = 0, viewport = 0, casters = 0;  // differing views, by field
		std::uint64_t cascades = 0, cascadePlanes = 0, cascadeCasters = 0;          // the sun bits' cascades
		std::uint64_t fullFrusta = 0, fullFrustumPlanes = 0;                        // the full-frustum processes
		std::uint64_t missing = 0;                                                   // a view or cascade without DCLF's values
		float largest = 0.0f;
		std::string first;
		/** @brief A difference: the largest so far, and the first of each kind (up to four kinds) for the report. */
		void Note(const char* a_what, float a_size, std::string a_detail);
	};

	enum class ShadowView : std::uint8_t
	{
		Sun,
		Light,
		Focus,
	};

	/** @brief The tally of a_view's kind (render thread). */
	ViewTally& ShadowViewTally(ShadowView a_view);
}
