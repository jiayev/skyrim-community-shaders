#pragma once

// A commit's uploads on the render thread: staged (CommitUploads) or latched into the epoch's latch block (LatchedUploads). Every write
// states its content's descriptor owners (dclf-architecture.md, "Descriptor ownership"): an OwnedRow's, or none for plain bytes.
// Self-contained (features include it); Internal.h includes it in its order.

#include "Features/DrawcallLimitFix/Core/Gpu/FrameData.h"
#include "Features/DrawcallLimitFix/Core/Gpu/OwnedRows.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/ContentOwners.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"

#include <Render/LatchBlock.h>
#include <Render/Runtime/IUploadService.h>
#include <Render/Runtime/StagedUploadBatch.h>
#include <Render/Runtime/UploadServiceAccess.h>
#include <Resources/Buffers/Buffer.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Draws
{
	/**
	 * @brief Where a write at a_offset into the frame-constants buffer goes, named for FrameData: a stage's register slot, the
	 * shared light block, the lighting slot's parts (the frame lighting, the LOD fades' inputs, the foliage record's) or the record.
	 * Resources/FrameSlots.cpp.
	 */
	std::string FrameConstantsPart(std::uint64_t a_offset);
	/** @brief FrameData's name of a write to a_target at a_offset, as a_route ("latched", "staged"). */
	template <class Target>
	std::string FrameDataWhere(std::string_view a_route, const Target& a_target, std::uint64_t a_offset)
	{
		std::string where(a_route);
		where += ": ";
		if constexpr (requires { a_target->GetName(); }) {
			const auto& name = a_target ? a_target->GetName() : std::string();
			if (name == "cs.dclf.frame-constants")
				return where + FrameConstantsPart(a_offset);
			where += name.empty() ? std::string("unnamed") : name;
		} else {
			where += "other";
		}
		return where;
	}

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
	// the epoch's recordings resolve. Each write replaces its range's content owners (a_content: the scene's; null for a commit that
	// writes no scene content, which then takes no OwnedRow).
	class CommitUploads
	{
	public:
		CommitUploads(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool, const NamedVersions& a_adopted, ContentOwners* a_content = nullptr) :
			batch(AcquireStagedBatch(a_pool, std::size_t{ 1 } << 20)), adopted(a_adopted), content(a_content)
		{}
		~CommitUploads()
		{
			if (!batch->Entries().empty())
				org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(batch));
		}
		CommitUploads(const CommitUploads&) = delete;
		CommitUploads& operator=(const CommitUploads&) = delete;

		// Plain bytes: no descriptor of theirs needs an owner.
		template <class Target>
		void operator()(const Target& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (!a_data || !a_bytes)
				return;
			NoteContent(ContentKey(a_target), a_offset, a_bytes, {});
			Stage(a_target, a_data, a_bytes, a_offset);
		}
		// A row naming descriptors: its owners are the range's content owners from now on.
		template <class Target, class Row>
		void operator()(const Target& a_target, const OwnedRow<Row>& a_row, std::uint64_t a_offset)
		{
			Stage(a_target, a_row.Bytes(), OwnedRow<Row>::kBytes, a_offset);
			NoteContent(ContentKey(a_target), a_offset, OwnedRow<Row>::kBytes, a_row.Owners());
		}
		/** @brief The buffer a write into a_target goes to: the version the adopted set names. */
		const std::shared_ptr<org::Buffer>& TargetOf(const Versioned& a_target) const { return adopted.Of(a_target).version->buffer; }

	private:
		friend class LatchedUploads;
		// A target as the content owners key it: a plain buffer's address, a versioned buffer's key (every version of it).
		static const void* ContentKey(const Versioned& a_target) { return a_target->Key(); }
		template <class T>
		static const void* ContentKey(const std::shared_ptr<T>& a_target)
		{
			return a_target.get();
		}
		template <class Target>
		void Stage(const Target& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			batch->Stage(org::runtime::UploadTarget::FromShared(a_target), static_cast<std::size_t>(a_offset), a_data, a_bytes);
			FrameData::Note(FrameDataWhere("staged", a_target, a_offset), a_bytes);
		}
		// A versioned buffer: the version the adopted set names (every target is one of its buffers: a buffer of the set is named by
		// every set sealed after its registration, and a commit writes only what its revision's shapes name).
		void Stage(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset);
		void NoteContent(const void* a_key, std::uint64_t a_offset, std::uint64_t a_bytes, std::span<const std::shared_ptr<const void>> a_owners)
		{
			if (content)
				content->Set(a_key, a_offset, a_bytes, a_owners);
			else if (!a_owners.empty())
				stl::report_and_fail("Drawcall Limit Fix: a row naming descriptors was uploaded by a commit with no content owners (its descriptors would outlive their owners)");
		}

		std::shared_ptr<org::runtime::StagedUploadBatch> batch;
		const NamedVersions& adopted;
		ContentOwners* content = nullptr;
	};

	/**
	 * @brief A commit's uploads of per-frame values (a block written whole, a counter zeroed) into its shape's latched copies (a
	 * scene revision's): each value at its copy's place in the list's block (the one the revision's set names), the rest of the copy
	 * zeroed, and the epoch's first pass copies them to their targets with the ticket, on ORG's host thread. A value the list has no
	 * copy for is staged (a_fallback), as a target no pass declared is. Finish zeroes the copies no value was written to, and returns
	 * the list. Content owners as the fallback's.
	 */
	class LatchedUploads
	{
	public:
		LatchedUploads(const LatchedList& a_layout, CommitUploads& a_fallback, std::uint32_t a_slot) :
			fallback(a_fallback), slot(a_slot), layout(a_layout), written(a_layout.copies.size(), 0)
		{}
		/** @brief Values the layout had no copy for (staged instead). */
		std::uint32_t Misses() const { return misses; }

		// Plain bytes (no descriptor of theirs needs an owner). A latched value is counted here (FrameData), a staged one by the fallback.
		void operator()(const std::shared_ptr<org::Buffer>& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (!a_data || !a_bytes)
				return;
			fallback.NoteContent(a_target.get(), a_offset, a_bytes, {});
			Write(a_target, a_data, a_bytes, a_offset);
		}
		// A versioned buffer: latched by its key, copied into the version the epoch's preparation resolved (the adopted set's).
		void operator()(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (!a_data || !a_bytes)
				return;
			fallback.NoteContent(a_target->Key(), a_offset, a_bytes, {});
			Write(a_target, a_data, a_bytes, a_offset);
		}
		// A row naming descriptors: its owners are the range's content owners from now on.
		template <class Target, class Row>
		void operator()(const Target& a_target, const OwnedRow<Row>& a_row, std::uint64_t a_offset)
		{
			Write(a_target, a_row.Bytes(), OwnedRow<Row>::kBytes, a_offset);
			fallback.NoteContent(CommitUploads::ContentKey(a_target), a_offset, OwnedRow<Row>::kBytes, a_row.Owners());
		}

	private:
		void Write(const std::shared_ptr<org::Buffer>& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (Latch(a_target.get(), a_data, a_bytes, a_offset, [&] { fallback.Stage(a_target, a_data, a_bytes, a_offset); }))
				FrameData::Note(FrameDataWhere("latched", a_target, a_offset), a_bytes);
		}
		void Write(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
		{
			if (Latch(a_target->Key(), a_data, a_bytes, a_offset, [&] { fallback.Stage(a_target, a_data, a_bytes, a_offset); }))
				FrameData::Note(FrameDataWhere("latched", fallback.TargetOf(a_target), a_offset), a_bytes);
		}
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
