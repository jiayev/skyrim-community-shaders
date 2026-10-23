#pragma once

// The engine addresses Fades alone reads or calls (module offsets, AE 1.6.1170 and SE 1.5.97); those Core or another feature also reads
// stay in Core/Engine/EngineAccess.h (Engine::Address). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstddef>
#include <cstdint>

namespace DCLF::Features::Fades::Address
{
	inline constexpr DCLF::Engine::Offset kFadeStepDivisor{ 0x2032e2c, 0x1e0dc8c };  // float: the fade-in time
	inline constexpr DCLF::Engine::Offset kFadeStepMax{ 0x2032e4c, 0x1e0dcac };  // float: the largest fade step
	inline constexpr DCLF::Engine::Offset kFadeFrameCounter{ 0x2032e50, 0x1e0dcb0 };  // std::int32_t: the fade code's frame counter
	inline constexpr DCLF::Engine::Offset kFadeStepTime{ 0x2033084, 0x1e0def4 };  // float: the frame time the fades step by
	inline constexpr DCLF::Engine::Offset kFloatOne{ 0x1ad2870, 0x15232d8 };  // float 1.0
	inline constexpr DCLF::Engine::Offset kFadeOnVisible{ 0x1479f50, 0x128fe30 };  // BSFadeNode::OnVisible
	inline constexpr DCLF::Engine::Offset kLeafOnVisible{ 0x147c9c0, 0x1292760 };  // BSLeafAnimNode::OnVisible (its leaf LOD step, then BSFadeNode's)
	inline constexpr DCLF::Engine::Offset kFadeDistance{ 0x147b110, 0x1290f40 };  // FUN_14147b110(node, camera, out): the distance and LOD level
	// BSFadeNode's fade and LOD globals (FadeState's frame inputs).
	inline constexpr DCLF::Engine::Offset kFadeOutTime{ 0x2032e30, 0x1e0dc90 };
	inline constexpr DCLF::Engine::Offset kFadeInAbove{ 0x2032e34, 0x1e0dc94 };
	inline constexpr DCLF::Engine::Offset kFadeBlendTime{ 0x2032e3c, 0x1e0dc9c };
	inline constexpr DCLF::Engine::Offset kFadeAmountTime{ 0x332a214, 0x31d0d1c };
	// The LOD fade thresholds, far and near, for a fade node and a tree. AE reads four floats; SE (the thresholds' function, out of line
	// there: FUN_141290590) indexes two float arrays by the node's kind, 1 for a fade node and 4 for a tree.
	inline constexpr std::uintptr_t kSeLodFadeFarArray = 0x31d0d30, kSeLodFadeNearArray = 0x31d0d60;
	inline constexpr std::uintptr_t kSeLodKindFade = 1, kSeLodKindTree = 4;
	inline constexpr DCLF::Engine::Offset kLodFadeFar{ 0x332a22c, kSeLodFadeFarArray + 4 * kSeLodKindFade };
	inline constexpr DCLF::Engine::Offset kLodFadeNear{ 0x332a25c, kSeLodFadeNearArray + 4 * kSeLodKindFade };
	inline constexpr DCLF::Engine::Offset kTreeLodFadeFar{ 0x332a238, kSeLodFadeFarArray + 4 * kSeLodKindTree };
	inline constexpr DCLF::Engine::Offset kTreeLodFadeNear{ 0x332a268, kSeLodFadeNearArray + 4 * kSeLodKindTree };
	inline constexpr DCLF::Engine::Offset kLodFadeMinimum{ 0x33dcfa8, 0x3283b7c };
	inline constexpr DCLF::Engine::Offset kFadeAmountSnapAbove{ 0x1ad2874, 0x15a8a70 };
	inline constexpr DCLF::Engine::Offset kFadeAmountSnapOffset{ 0x1ad288c, 0x1525c5c };
	inline constexpr DCLF::Engine::Offset kFadeSnapRadiusLimit{ 0x1ad29f4, 0x1536ba8 };
	// BSTreeNode::OnVisible (AE 0x14147d3c0): the height test (the frame start's capture, TreeOnVisible's test). The switch byte is on
	// the culling process (a field offset); the rest are module offsets.
	inline constexpr std::size_t kProcessTreeHeightTest = 0x121;  // byte on the culling process
	inline constexpr DCLF::Engine::Offset kTreeHeightTestOn{ 0x2032fb8, 0x1e0de18 };  // byte
	inline constexpr DCLF::Engine::Offset kTreeHeightBase{ 0x332a2f0, 0x31d0df8 };  // float
	inline constexpr DCLF::Engine::Offset kTreeHeightLimit{ 0x2032fa0, 0x1e0de00 };  // float
}
