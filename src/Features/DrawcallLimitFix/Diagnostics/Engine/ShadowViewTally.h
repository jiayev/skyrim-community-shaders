#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics
{
	/**
	 * @brief The shadow views' parity (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY): DCLF's views (SunViews, LightViews, FocusViews:
	 * from the cameras the engine's UpdateCamera set up) against the engine's captures - the view-projection, eye, viewport and caster
	 * volume at each view's draw (the shadow epochs), the sun's cascade planes at each cascade cull, the full-frustum processes - and
	 * what the engine registers into a covered focus view against its target's fade root. One tally per kind of view; each kind's line
	 * every 300 frames that had such views (Reports: SunViewsLines, LightViewsLines, FocusViewsLines).
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

	/** @brief The observers: the views' report windows, the cascade planes, the missing cascades, the focus views' membership. */
	void InstallShadowViews();

	/** @brief The focus views' membership check since the last Take: registrations into a covered focus view, those under another fade root, the first named. */
	struct FocusMembers
	{
		std::uint64_t registrations = 0, outside = 0;
		std::string first;
	};
	FocusMembers TakeFocusMembers();
}
