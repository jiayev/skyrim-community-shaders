#pragma once

// The engine addresses Fades alone reads or calls (module offsets, AE 1.6.1170); those Core or another feature also reads stay in
// Core/Engine/EngineAccess.h (Engine::Address). Included by the feature's sources only.

#include <cstdint>

namespace DCLF::Features::Fades::Address
{
	inline constexpr std::uintptr_t kFadeStepDivisor = 0x2032e2c;  // float: the fade-in time
	inline constexpr std::uintptr_t kFadeStepMax = 0x2032e4c;  // float: the largest fade step
	inline constexpr std::uintptr_t kFadeFrameCounter = 0x2032e50;  // std::int32_t: the fade code's frame counter
	inline constexpr std::uintptr_t kFadeStepTime = 0x2033084;  // float: the frame time the fades step by
	inline constexpr std::uintptr_t kFloatOne = 0x1ad2870;  // float 1.0
	inline constexpr std::uintptr_t kFadeOnVisible = 0x1479f50;  // BSFadeNode::OnVisible
	inline constexpr std::uintptr_t kLeafOnVisible = 0x147c9c0;  // BSLeafAnimNode::OnVisible (its leaf LOD step, then BSFadeNode's)
	inline constexpr std::uintptr_t kFadeDistance = 0x147b110;  // FUN_14147b110(node, camera, out): the distance and LOD level
	// BSFadeNode's fade and LOD globals (FadeState's frame inputs).
	inline constexpr std::uintptr_t kFadeOutTime = 0x2032e30;
	inline constexpr std::uintptr_t kFadeInAbove = 0x2032e34;
	inline constexpr std::uintptr_t kFadeBlendTime = 0x2032e3c;
	inline constexpr std::uintptr_t kFadeAmountTime = 0x332a214;
	inline constexpr std::uintptr_t kLodFadeFar = 0x332a22c;
	inline constexpr std::uintptr_t kLodFadeNear = 0x332a25c;
	inline constexpr std::uintptr_t kTreeLodFadeFar = 0x332a238;
	inline constexpr std::uintptr_t kTreeLodFadeNear = 0x332a268;
	inline constexpr std::uintptr_t kLodFadeMinimum = 0x33dcfa8;
	inline constexpr std::uintptr_t kFadeAmountSnapAbove = 0x1ad2874;
	inline constexpr std::uintptr_t kFadeAmountSnapOffset = 0x1ad288c;
	inline constexpr std::uintptr_t kFadeSnapRadiusLimit = 0x1ad29f4;
}
