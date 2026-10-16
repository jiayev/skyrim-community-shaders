#pragma once

// The engine addresses Shadows/Engine alone reads or calls (module offsets, AE 1.6.1170); those Core or another feature also reads stay in
// Core/Engine/EngineAccess.h (Engine::Address). Included by the feature's sources only.

#include <cstdint>

namespace DCLF::Features::Shadows::Engine::Address
{
	// The shadow lights' selection (skyrim-engine-notes.md, "The shadow lights' selection"): each call site checked with CallsTo before it is
	// patched.
	inline constexpr std::uintptr_t kCalculateAndDrawShadowCasterLights = 0x14cbb90;  // NiCamera::CalculateAndDrawShadowCasterLights
	inline constexpr std::uintptr_t kCastersCallSite = 0x644674;                      // Main::Draw's call of it (Main::Draw +0x1C4)
	inline constexpr std::uintptr_t kKeepLight = 0x14a3fb0;                           // FUN_1414a3fb0(node, light, first slot, slots)
	inline constexpr std::uintptr_t kKeepSunCallSite = 0x14cbc66;                     // the sun's, in CalculateAndDrawShadowCasterLights
	inline constexpr std::uintptr_t kKeepLightCallSite = 0x14cc7e1;                   // each kept light's, in CalculateActiveShadowCasterLights
}
