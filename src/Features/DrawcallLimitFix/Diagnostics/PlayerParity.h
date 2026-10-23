#pragma once

// The compatibility monitor: the parity checks a player can switch on while playing (the menu, or CS_DCLF_PLAYER_PARITY=1 at the start),
// which watch for engine state another plugin changed where DCLF has no hook, and name the objects' references and their plugins.
//
// Only checks that leave the main path as it is run under it (Hooks.h's switchable observers): the scene mirror's probe, a slice of
// the tracked set captured live each frame and compared with the mirror, whose differences no event explains are the offenders. The
// checks that change what DCLF draws or when (persistent, set, walk, build, capture parity) stay developer switches.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Diagnostics::PlayerParity
{
	/** @brief One offender: a field of the objects under one reference that changed with no event, counted over the session. */
	struct Offender
	{
		std::uint32_t form = 0;    // the (first) owning reference's form ID (0: an object above any reference's root, a cell's or the world's)
		std::uint32_t references = 1;  // the references it was found under: a property or an alpha is shared by every instance of its mesh
		std::uint8_t type = 0;     // the record type (SceneMirror::kTypeNames: node, geometry, property, alpha)
		std::uint32_t fields = 0;  // the fields' bits that differed (the record type's kFieldNames)
		std::uint64_t count = 0;   // the probes that found them missed
		std::uint32_t firstFrame = 0, lastFrame = 0;
		std::string example;       // the first difference, as the mirror parity logs it
	};

	/** @brief The offenders since the monitor last started or was cleared, oldest first; any thread. */
	struct Offenders
	{
		std::vector<Offender> entries;
		std::uint64_t probes = 0;  // checks since the session started
		std::uint64_t overflow = 0;  // new offenders past the table's limit, counted only
		std::uint32_t sessionFrame = 0;
	};

	/** @brief PostPostLoad (Diagnostics::Install): the frame-start hook, and the request seeded from CS_DCLF_PLAYER_PARITY. */
	void Install();

	/** @brief The menu: the monitor on or off from the next frame. */
	void Request(bool a_on);
	bool Requested();

	/** @brief The monitor is on this frame (applied at its start). */
	bool Active();

	/** @brief The coordinator, the mirror check: true once after the monitor started, so its observers drop what they kept while off. */
	bool TakeReset();

	/** @brief The coordinator, the mirror check: a missed difference on an object of reference a_form. */
	void NoteMissed(const void* a_object, std::uint32_t a_form, std::uint8_t a_type, std::uint32_t a_fields, std::string_view a_example);

	/** @brief The coordinator, after each mirror check: the offenders published when they changed (a clear the menu asked for applied). */
	void NoteCheck();

	/** @brief The current offenders (a snapshot, published by the coordinator on change). */
	std::shared_ptr<const Offenders> Current();

	/** @brief The menu: the offenders cleared at the coordinator's next check. */
	void Clear();

	/** @brief Main or render thread: the report (build, game, plugins, offenders) written beside the log; its path, or why not. */
	std::string WriteReport();

	/** @brief The menu's section (DrawcallLimitFix::DrawSettings). */
	void DrawPanel();
}
