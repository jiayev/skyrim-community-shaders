#pragma once

// FadeStateCS's layout (the fade roots' step on the GPU: its shader, constants, group and the root buffers' first capacity), which must
// match the HLSL. The rows it reads and writes (FadeRootStatic, FadeNodeState, FadeFrame) are Core's (Core/Scene/Model/Records.h): the cull
// reads them too.

#include <cstdint>

namespace DCLF::Draws
{
	constexpr const char* kFadeStateShader = "DrawcallLimitFix/Features/Fades/FadeStateCS.hlsl";
	// FadeStateCS.hlsl's constants: the depth latch (view-projection), the parity log's first root (~0u: none). Must match the HLSL.
	struct FadeStateConstants
	{
		std::uint32_t rootsIndex = 0, statesIndex = 0, frameIndex = 0;
		std::uint32_t latchIndex = 0, latchOffset = 0, logIndex = 0;
		std::uint32_t outIndices[2]{};        // the published states, by the scene frame's parity
		std::uint32_t programsIndex = 0;      // ByteAddressBuffer: the main cull input's programs (Features/Portals: PortalPrograms.h)
		std::uint32_t rootProgramsIndex = 0;  // StructuredBuffer<uint>: each root's program (the input's judge)
		std::uint32_t animatedIndex = 0;      // StructuredBuffer<uint>: per root, the scene frame whose animation batch updated it
		std::uint32_t eventsIndex = 0;        // RWStructuredBuffer<uint>: the write-back's events (FadeRecords.h, FadeEvent)
		std::uint32_t reportedIndex = 0;      // RWStructuredBuffer<uint2>: per root, the generation and milestone last reported
		std::uint32_t eventCapacity = 0;      // the events the list and every frame slot's readback hold
		std::uint32_t portalQueriesIndex = 0, portalResultsIndex = 0;
	};
	static_assert(sizeof(FadeStateConstants) == 64);
	constexpr std::uint32_t kFadeStateConstantWords = sizeof(FadeStateConstants) / 4;
	constexpr std::uint32_t kFadeStateGroup = 64;
	constexpr std::uint32_t kInitialFadeRoots = 4096;
}
