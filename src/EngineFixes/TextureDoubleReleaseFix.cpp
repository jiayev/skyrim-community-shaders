#include "TextureDoubleReleaseFix.h"

#include "Utils/VersionedRelocation.h"

namespace
{
	// From the reference count test to the destructors: jne past the release; release +0x10, +0x00, +0x08 explicitly; nop
	// (the destructors' state); then the destructors' first release, of +0x00.
	constexpr auto ExplicitReleases = REL::make_pattern<
		"83 F8 01 75 ?? "
		"48 8B 4A 10 48 85 C9 74 06 48 8B 01 FF 50 10 "
		"48 8B 0B 48 85 C9 74 06 48 8B 01 FF 50 10 "
		"48 8B 4B 08 48 85 C9 74 07 48 8B 01 FF 50 10 "
		"90 48 8B 0B">();
	constexpr std::uintptr_t ExplicitReleasesAt = 0x1c;  // the cmp, inside the release
	constexpr std::uintptr_t JumpAt = 0x21;              // the first explicit release
	constexpr std::uintptr_t DestructorsAt = 0x4e;       // the destructors' first release
}

void TextureDoubleReleaseFix::Install()
{
	if (!Util::VersionedRelocation::IsAtLeastAE1799())
		return;  // 1.6 and SE release each object once

	const auto release = REL::ID(77322).address();
	if (!REL::verify_code(release + ExplicitReleasesAt, ExplicitReleases)) {
		logger::warn("[Engine Fixes] {} not applied: unexpected code in the renderer texture release (another mod or an unverified runtime)", GetName());
		return;
	}
	constexpr std::uint8_t jump[] = { 0xEB, static_cast<std::uint8_t>(DestructorsAt - (JumpAt + 2)) };  // jmp short
	REL::safe_write(release + JumpAt, jump, sizeof(jump));
}
