#pragma once

// The engine addresses Fades alone reads or calls (Engine::Offset: Address Library ids); those Core or another feature also reads stay
// in Core/Engine/EngineAccess.h (Engine::Address). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstddef>
#include <cstdint>

namespace DCLF::Features::Fades::Address
{
	inline constexpr DCLF::Engine::Offset kFadeStepDivisor{ { 390877 }, { 513153 } };  // float: the fade-in time
	inline constexpr DCLF::Engine::Offset kFadeStepMax{ { 390885 }, { 513161 } };  // float: the largest fade step
	inline constexpr DCLF::Engine::Offset kFadeFrameCounter{ { 390886 }, { 513162 } };  // std::int32_t: the fade code's frame counter
	inline constexpr DCLF::Engine::Offset kFadeStepTime{ { 390954 }, { 513214 } };  // float: the frame time the fades step by
	inline constexpr DCLF::Engine::Offset kFloatOne{ { 186482 }, { 228662 } };  // float 1.0
	inline constexpr DCLF::Engine::Offset kFadeOnVisible{ { 105485 }, { 98848 } };  // BSFadeNode::OnVisible
	inline constexpr DCLF::Engine::Offset kLeafOnVisible{ { 105588 }, { 98937 } };  // BSLeafAnimNode::OnVisible (its leaf LOD step, then BSFadeNode's)
	inline constexpr DCLF::Engine::Offset kFadeDistance{ { 105506 }, { 98862 } };  // FUN_14147b110(node, camera, out): the distance and LOD level
	// BSFadeNode's fade and LOD globals (FadeState's frame inputs).
	inline constexpr DCLF::Engine::Offset kFadeOutTime{ { 390878 }, { 513154 } };
	inline constexpr DCLF::Engine::Offset kFadeInAbove{ { 390879 }, { 513155 } };
	inline constexpr DCLF::Engine::Offset kFadeBlendTime{ { 390881 }, { 513157 } };
	inline constexpr DCLF::Engine::Offset kFadeAmountTime{ { 414542 }, { 527615 } };
	// The LOD fade thresholds, far and near, for a fade node and a tree. AE reads four floats; SE (the thresholds' function, out of line
	// there: FUN_141290590) indexes two float arrays by the node's kind, 1 for a fade node and 4 for a tree.
	inline constexpr std::uint64_t kSeLodFadeFarArray = 527618, kSeLodFadeNearArray = 527624;  // 0x1431d0d30, 0x1431d0d60
	inline constexpr std::uint32_t kSeLodKindFade = 1, kSeLodKindTree = 4;
	inline constexpr DCLF::Engine::Offset kLodFadeFar{ { 414546 }, { kSeLodFadeFarArray, 4 * kSeLodKindFade } };
	inline constexpr DCLF::Engine::Offset kLodFadeNear{ { 414553 }, { kSeLodFadeNearArray, 4 * kSeLodKindFade } };
	inline constexpr DCLF::Engine::Offset kTreeLodFadeFar{ { 414547 }, { kSeLodFadeFarArray, 4 * kSeLodKindTree } };
	inline constexpr DCLF::Engine::Offset kTreeLodFadeNear{ { 414554 }, { kSeLodFadeNearArray, 4 * kSeLodKindTree } };
	inline constexpr DCLF::Engine::Offset kLodFadeMinimum{ { 415263 }, { 528314 } };
	inline constexpr DCLF::Engine::Offset kFadeAmountSnapAbove{ { 196368 }, { 242717 } };
	inline constexpr DCLF::Engine::Offset kFadeAmountSnapOffset{ { 186632 }, { 228898 } };
	inline constexpr DCLF::Engine::Offset kFadeSnapRadiusLimit{ { 470092 }, { 230694 } };
	// BSTreeNode::OnVisible (AE 0x14147d3c0): the height test (the frame start's capture, TreeOnVisible's test). The switch byte is on
	// the culling process (a field offset); the rest are engine globals.
	inline constexpr std::size_t kProcessTreeHeightTest = 0x121;  // byte on the culling process
	inline constexpr DCLF::Engine::Offset kTreeHeightTestOn{ { 390930 }, { 513192 } };  // byte
	inline constexpr DCLF::Engine::Offset kTreeHeightBase{ { 414568 }, { 527639 } };  // float
	inline constexpr DCLF::Engine::Offset kTreeHeightLimit{ { 390927 }, { 513190 } };  // float
}
