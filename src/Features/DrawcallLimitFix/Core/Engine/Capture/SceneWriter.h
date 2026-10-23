#pragma once

// DCLF's side of SceneWriterFramework (extern/SceneWriterFramework): the engine's store hooks (hidden bits, switch indices, fade
// resets) are SWF's, shared with every plugin that embeds it or installs its DLL. CS embeds one copy; at kPostLoad every copy in the
// process elects one provider, which patches the engine and serves the events DCLF subscribes to here.

#include <swf/swf.h>

namespace DCLF::SceneWriter
{
	/** @brief The capabilities DCLF cannot run without: switch selections and fade resets are followed by event alone. */
	inline constexpr SWF_Caps kRequiredCaps = SWF_CAP_SWITCH_INDEX | SWF_CAP_FADE_RESET;

	/**
	 * @brief The election (CS's kPostLoad, when DCLF will install): the newest SWF copy in the process patches the engine. CS's
	 * own copy's log lines go to CS's log.
	 */
	void Elect();

	/** @brief The elected provider's API, or null (no election ran, SWF disabled, or this game build has no tables). */
	const SWF_Api* Api();

	/** @brief The provider's installed families (SWF_CAP_*), 0 without one. */
	SWF_Caps Caps();

	/** @brief Why DCLF cannot run on this provider, or empty when kRequiredCaps are all installed. */
	std::string MissingReason();

	/** @brief Subscribes a_fn to a_event's POST phase; false (logged) when the provider refuses it. */
	bool Subscribe(std::uint32_t a_event, SWF_EventFn a_fn, std::int32_t a_priority = 0);
}
