#pragma once

// A commit's uploads: staged (CommitUploads) or latched (LatchedUploads).
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	// Whether BuildMainPayload reads MainInputs::eye/previousEye: only the records' observers (the engine's eye-relative
	// PerGeometry groups, Policy::BuildAtEpoch) do.
	inline bool BuildReadsEye(const MainInputs& a_in)
	{
		return a_in.readsEye;
	}

	// A commit's own uploads on the render thread, staged directly (StagedUploadBatch) and submitted when the
	// commit ends: each is one memcpy into mapped staging here and one copy in the submission, instead of a
	// trip through the upload pass's per-upload bookkeeping. The batch comes from a pool of released ones.
	/**
	 * @brief A released batch from a pool (neither a payload nor the upload service still holds it), reset, or a new one
	 * of a_pageSize pages added to the pool.
	 */
	inline std::shared_ptr<org::runtime::StagedUploadBatch> AcquireStagedBatch(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool,
		std::size_t a_pageSize = std::size_t{ 4 } << 20)
	{
		ZoneScopedN("CS.DCLF.Stage.AcquireBatch");
		std::shared_ptr<org::runtime::StagedUploadBatch> batch;
		for (const auto& candidate : a_pool) {
			if (candidate.use_count() == 1) {
				batch = candidate;
				break;
			}
		}
		if (!batch) {
			batch = org::runtime::StagedUploadBatch::Create(a_pageSize);
			a_pool.push_back(batch);
		}
		batch->Reset();
		return batch;
	}

	class CommitUploads
	{
	public:
		explicit CommitUploads(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool) :
			batch(AcquireStagedBatch(a_pool, std::size_t{ 1 } << 20))
		{}
		~CommitUploads()
		{
			if (!batch->Entries().empty())
				org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(batch));
		}
		CommitUploads(const CommitUploads&) = delete;
		CommitUploads& operator=(const CommitUploads&) = delete;

		template <class Target>
		void operator()(const Target& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (!a_data || !a_bytes)
				return;
			batch->Stage(org::runtime::UploadTarget::FromShared(a_target), static_cast<std::size_t>(a_offset), a_data, a_bytes);
			FrameData::Note(FrameDataWhere("staged", a_target, a_offset), a_bytes);
		}
		// A versioned buffer: its current version.
		void operator()(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			(*this)(a_target->Get(), a_data, a_bytes, a_offset);
		}

	private:
		std::shared_ptr<org::runtime::StagedUploadBatch> batch;
	};

	/**
	 * @brief A main commit's uploads of per-frame values (a block written whole, a counter zeroed): a target the epoch's latched
	 * copies declared (Resources::latchedTargets) gets its bytes in the commit's latch block and a copy in the frame's list, which
	 * the epoch's first pass records with the ticket, on ORG's host thread; any other goes to a_fallback, staged. A copy is in the
	 * list only in a frame that writes it, so a target not written keeps what it holds, as with staging.
	 */
	class LatchedUploads
	{
	public:
		/** @brief a_block: a latch block of the caller's, written at a_slot, replaced by a larger one when full (a commit writing into a revision's shape uses the layout form). */
		LatchedUploads(const std::atomic<std::shared_ptr<const std::vector<const void*>>>& a_targets, CommitUploads& a_fallback,
			std::shared_ptr<org::LatchBlock>& a_block, std::uint32_t a_slot, std::uint32_t a_slots) :
			targets(a_targets.load(std::memory_order_acquire)), fallback(a_fallback), block(&a_block), slot(a_slot), slots(a_slots)
		{}
		/**
		 * @brief Into a shape's latched copies (a scene revision's, R3c): each value at its copy's place in the list's block, the rest
		 * of the copy zeroed; a value the list has no copy for is staged (a_fallback), as a target no pass declared is. Finish zeroes
		 * the copies no value was written to, and returns the list.
		 */
		LatchedUploads(const LatchedList& a_layout, CommitUploads& a_fallback, std::uint32_t a_slot) :
			fallback(a_fallback), slot(a_slot), layout(&a_layout), written(a_layout.copies.size(), 0)
		{}
		/** @brief Values the layout had no copy for (staged instead). */
		std::uint32_t Misses() const { return misses; }

		// A latched value is counted here (FrameData), a staged one by the fallback.
		void operator()(const std::shared_ptr<org::Buffer>& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (Latch(a_target.get(), a_data, a_bytes, a_offset, [&] { fallback(a_target, a_data, a_bytes, a_offset); }))
				FrameData::Note(FrameDataWhere("latched", a_target, a_offset), a_bytes);
		}
		// A versioned buffer: latched by its key, copied into whichever version the epoch's preparation resolved.
		void operator()(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (Latch(a_target->Key(), a_data, a_bytes, a_offset, [&] { fallback(a_target, a_data, a_bytes, a_offset); }))
				FrameData::Note(FrameDataWhere("latched", a_target->Get(), a_offset), a_bytes);
		}

	private:
		// True when the value went into the latch block (false: nothing, or staged by a_fallback).
		template <class Fallback>
		bool Latch(const void* a_key, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset, Fallback&& a_fallback)
		{
			if (!a_data || !a_bytes)
				return false;
			if (layout) {
				const auto& listed = layout->copies;
				for (std::size_t i = 0; i < listed.size(); ++i) {
					const auto& copy = listed[i];
					if (copy.target != a_key || copy.dstOffset != a_offset || a_bytes > copy.bytes || !layout->latch)
						continue;
					auto region = layout->latch->Slot(slot).subspan(copy.latchOffset, copy.bytes);
					std::memcpy(region.data(), a_data, a_bytes);
					std::memset(region.data() + a_bytes, 0, copy.bytes - a_bytes);
					written[i] = 1;
					return true;
				}
				++misses;
				a_fallback();
				return false;
			}
			if (!targets || std::find(targets->begin(), targets->end(), a_key) == targets->end()) {
				a_fallback();
				return false;
			}
			const std::size_t at = (used + 15) & ~std::size_t(15);
			auto& current = *block;
			if (!current || at + a_bytes > current->Stride()) {
				// A larger block, with what this commit has written so far: frames in flight keep the old one (LatchedList::latch).
				auto grown = std::make_shared<org::LatchBlock>("cs.dclf.latched-copies",
					static_cast<std::uint32_t>(std::bit_ceil(std::max<std::size_t>(2 * (at + a_bytes), 4096))), slots);
				if (current && used)
					std::memcpy(grown->Slot(slot).data(), current->Slot(slot).data(), used);
				current = std::move(grown);
			}
			std::memcpy(current->Slot(slot).data() + at, a_data, a_bytes);
			used = at + a_bytes;
			copies.push_back({ a_key, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(a_bytes), a_offset });
			return true;
		}

	public:
		/** @brief The frame's list: the copies of what this commit latched, from the block it wrote. */
		LatchedList Finish()
		{
			if (layout) {
				// A copy no value was written to this commit copies zeros, never what an older frame left in the slot.
				for (std::size_t i = 0; i < written.size(); ++i)
					if (!written[i] && layout->latch) {
						const auto& copy = layout->copies[i];
						auto region = layout->latch->Slot(slot).subspan(copy.latchOffset, copy.bytes);
						std::memset(region.data(), 0, region.size());
					}
				return *layout;
			}
			LatchedList out;
			if (copies.empty())
				return out;
			out.latch = *block;
			out.copies = std::move(copies);
			return out;
		}

	private:
		std::shared_ptr<const std::vector<const void*>> targets;
		CommitUploads& fallback;
		std::shared_ptr<org::LatchBlock>* block = nullptr;
		std::uint32_t slot = 0, slots = 0;
		std::size_t used = 0;
		std::vector<LatchedCopy> copies;
		const LatchedList* layout = nullptr;
		std::vector<std::uint8_t> written;
		std::uint32_t misses = 0;
	};
}
