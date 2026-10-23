#pragma once

// The engine addresses Shadows/Engine alone reads or calls (module offsets, AE 1.6.1170 and SE 1.5.97); those Core or another feature also
// reads stay in Core/Engine/EngineAccess.h (Engine::Address). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstdint>

namespace DCLF::Features::Shadows::Engine::Address
{
	// The shadow lights' selection (skyrim-engine-notes.md, "The shadow lights' selection"): each call site checked with CallsTo before it is
	// patched.
	inline constexpr DCLF::Engine::Offset kCalculateAndDrawShadowCasterLights{ 0x14cbb90, 0x12e2660 };  // NiCamera::CalculateAndDrawShadowCasterLights
	inline constexpr DCLF::Engine::Offset kCastersCallSite{ 0x644674, 0x5b1a24 };                      // Main::Draw's call of it (Main::Draw +0x1C4)
	inline constexpr DCLF::Engine::Offset kKeepLight{ 0x14a3fb0, 0x12bc770 };                           // FUN_1414a3fb0(node, light, first slot, slots)
	inline constexpr DCLF::Engine::Offset kKeepSunCallSite{ 0x14cbc66, 0x12e2ecd };                     // the sun's, in CalculateAndDrawShadowCasterLights (SE: in its sun part, FUN_1412e2e10)
	inline constexpr DCLF::Engine::Offset kKeepLightCallSite{ 0x14cc7e1, 0x12e31d1 };                   // each kept light's, in CalculateActiveShadowCasterLights
}
