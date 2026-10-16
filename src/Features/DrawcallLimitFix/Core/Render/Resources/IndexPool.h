#pragma once

// The index pool (IndexPool): every geometry slot's indices in one buffer, which every plain indexed draw binds. Self-contained (the
// passes of the features that draw indexed include it); Internal.h includes it before the shadow frame's types.

// volk must precede every Vulkan header in this translation unit (Helpers.h: DrawPipelinesRhi.h's FamilyPipelines).
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

#include "RenderGraph/ComputeProgram.h"
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <rhi.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	/**
	 * @brief Every geometry slot's indices copied into one buffer, so plain indexed draws bind one index buffer and keep vertex
	 * reuse. Ranges are per slot, never shared: a buffer address is no identity (freed addresses are reused at once). The frame's
	 * first drawing commit reassigns logged slots' ranges (UpdateIndexPool), copied before its draws. Render thread. Its capacities
	 * (indices, geometry slots, copies) are the named versions' units (Impl::RequireIndexPool).
	 */
	struct IndexPool
	{
		static constexpr std::uint32_t kNoRange = ~0u;
		// A slot's range: first index and count (even: 4-byte aligned), and the source buffer.
		struct Range
		{
			std::uint32_t first = kNoRange, count = 0;
			std::uint64_t address = 0, bytes = 0;
		};
		Versioned indices;  // 16-bit
		Versioned firsts;   // per geometry slot: its range's first index, kNoRange without one
		Versioned copies;   // this commit's copies: source address (low, high), first index, words
		// The versions the last update filled: an indices version it did not lay out is laid out and every range copied again; a
		// firsts version it did not fill is sent every slot's first index.
		org::services::FilledVersion indicesFilled, firstsFilled;
		// Slots waiting for growth (no room, past the first-index table or this commit's copies); retried by every update.
		std::vector<std::uint32_t> waiting;
		// No room though the pool has twice what it holds: the serial of the indices version found fragmented, published by the
		// render thread; the producer requires a version newer than it (a relayout at the same size). A serial alone, so relaxed
		// order suffices.
		std::atomic<std::uint64_t> fragmentedAt{ 0 };
		// The revision producer's bound (RevisionProducer::poolBound): every slot's index count, kept from the log.
		struct Bound
		{
			LogCursor cursor;
			std::vector<std::uint32_t> indices;
			std::uint64_t total = 0;
		} bound;
		std::shared_ptr<const ComputeProgram> program;
		rhi::CommandSignaturePtr dispatchSignature;
		LogCursor cursor;
		std::vector<Range> slots;                                   // per geometry slot: its range
		std::vector<std::uint32_t> slotFirst;                       // per geometry slot: what firsts holds
		std::map<std::uint32_t, std::uint32_t> free;                // first -> count
		std::uint32_t end = 0;                                      // past the last range ever given out
		std::uint64_t indicesHeld = 0;                              // live indices, for the report
	};
}
