#pragma once

// Latched copies: the per-frame values a commit writes into a latch block and its epoch's first pass copies to their targets.
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief A copy a main epoch's latched-copies pass (LatchedCopiesPass) records: bytes of the latch block's slot region, at
	 * latchOffset, to a target the pass declared, at dstOffset. The commit wrote the bytes (LatchedUploads).
	 */
	struct LatchedCopy
	{
		const void* target = nullptr;  // LatchedTarget::key: the buffer, or a versioned buffer's (VersionedBuffer::Key)
		std::uint32_t latchOffset = 0, bytes = 0;
		std::uint64_t dstOffset = 0;
		bool operator==(const LatchedCopy&) const = default;
	};
	/**
	 * @brief A commit's latched copies: the block they read, and the list (part of the frame's shape). fixed: one region for every
	 * frame slot (the counters' zeros), else the frame slot's.
	 */
	struct LatchedList
	{
		std::shared_ptr<const org::LatchBlock> latch;
		std::vector<LatchedCopy> copies;
		bool fixed = false;
		bool operator==(const LatchedList& o) const { return latch == o.latch && copies == o.copies && fixed == o.fixed; }
	};
	/** @brief A target a latched copy may write: a buffer, or a versioned buffer (copied into the version its preparation resolved). */
	struct LatchedTarget
	{
		std::shared_ptr<org::Buffer> buffer;
		Versioned versioned;
		const void* Key() const { return versioned ? versioned->Key() : buffer.get(); }
	};
}
