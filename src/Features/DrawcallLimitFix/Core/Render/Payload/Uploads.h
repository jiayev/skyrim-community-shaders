#pragma once

// A commit's uploads on the render thread: staged (CommitUploads) or latched into the epoch's latch block (LatchedUploads).
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
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

	// A commit's own uploads on the render thread, staged directly (StagedUploadBatch) and submitted when the commit ends: each is
	// one memcpy into mapped staging here and one copy in the submission, with no per-upload bookkeeping in the upload pass. The
	// batch comes from a pool of released ones. A versioned buffer's bytes go to the version the adopted set names (a_adopted), the one
	// the epoch's recordings resolve.
	class CommitUploads
	{
	public:
		CommitUploads(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool, const NamedVersions& a_adopted) :
			batch(AcquireStagedBatch(a_pool, std::size_t{ 1 } << 20)), adopted(a_adopted)
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
		// A versioned buffer: the version the adopted set names (every target is one of its buffers: a buffer of the set is named by
		// every set sealed after its registration, and a commit writes only what its revision's shapes name).
		void operator()(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset);
		/** @brief The buffer a write into a_target goes to: the version the adopted set names. */
		const std::shared_ptr<org::Buffer>& TargetOf(const Versioned& a_target) const { return adopted.Of(a_target).version->buffer; }

	private:
		std::shared_ptr<org::runtime::StagedUploadBatch> batch;
		const NamedVersions& adopted;
	};

	/**
	 * @brief A commit's uploads of per-frame values (a block written whole, a counter zeroed) into its shape's latched copies (a
	 * scene revision's): each value at its copy's place in the list's block (the one the revision's set names), the rest of the copy
	 * zeroed, and the epoch's first pass copies them to their targets with the ticket, on ORG's host thread. A value the list has no
	 * copy for is staged (a_fallback), as a target no pass declared is. Finish zeroes the copies no value was written to, and returns
	 * the list.
	 */
	class LatchedUploads
	{
	public:
		LatchedUploads(const LatchedList& a_layout, CommitUploads& a_fallback, std::uint32_t a_slot) :
			fallback(a_fallback), slot(a_slot), layout(a_layout), written(a_layout.copies.size(), 0)
		{}
		/** @brief Values the layout had no copy for (staged instead). */
		std::uint32_t Misses() const { return misses; }

		// A latched value is counted here (FrameData), a staged one by the fallback.
		void operator()(const std::shared_ptr<org::Buffer>& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (Latch(a_target.get(), a_data, a_bytes, a_offset, [&] { fallback(a_target, a_data, a_bytes, a_offset); }))
				FrameData::Note(FrameDataWhere("latched", a_target, a_offset), a_bytes);
		}
		// A versioned buffer: latched by its key, copied into the version the epoch's preparation resolved (the adopted set's).
		void operator()(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (Latch(a_target->Key(), a_data, a_bytes, a_offset, [&] { fallback(a_target, a_data, a_bytes, a_offset); }))
				FrameData::Note(FrameDataWhere("latched", fallback.TargetOf(a_target), a_offset), a_bytes);
		}

	private:
		// True when the value went into the latch block (false: nothing, or staged by a_fallback).
		template <class Fallback>
		bool Latch(const void* a_key, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset, Fallback&& a_fallback)
		{
			if (!a_data || !a_bytes)
				return false;
			const auto& listed = layout.copies;
			for (std::size_t i = 0; i < listed.size(); ++i) {
				const auto& copy = listed[i];
				if (copy.target != a_key || copy.dstOffset != a_offset || a_bytes > copy.bytes || !layout.latch)
					continue;
				auto region = layout.latch->Slot(slot).subspan(copy.latchOffset, copy.bytes);
				std::memcpy(region.data(), a_data, a_bytes);
				std::memset(region.data() + a_bytes, 0, copy.bytes - a_bytes);
				written[i] = 1;
				return true;
			}
			++misses;
			a_fallback();
			return false;
		}

	public:
		/** @brief The frame's list: the layout's, every copy no value was written to this commit zeroed. */
		LatchedList Finish()
		{
			// A copy no value was written to this commit copies zeros, never what an older frame left in the slot.
			for (std::size_t i = 0; i < written.size(); ++i)
				if (!written[i] && layout.latch) {
					const auto& copy = layout.copies[i];
					auto region = layout.latch->Slot(slot).subspan(copy.latchOffset, copy.bytes);
					std::memset(region.data(), 0, region.size());
				}
			return layout;
		}

	private:
		CommitUploads& fallback;
		std::uint32_t slot = 0;
		const LatchedList& layout;
		std::vector<std::uint8_t> written;
		std::uint32_t misses = 0;
	};
}
