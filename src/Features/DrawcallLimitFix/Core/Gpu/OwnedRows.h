#pragma once

// A GPU row's descriptor fields and their owners (dclf-architecture.md, "Descriptor ownership"): a descriptor index in GPU-visible
// bytes lives as long as those bytes' content. A row with descriptor fields declares them DescriptorIndex, which only OwnedRow::Bind
// sets, together with the binding's owner; the upload classes (Uploads.h) record an OwnedRow's owners as its target range's content
// (ContentOwners), which every execution holds while that content is current (ExecutionOwners).

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace DCLF
{
	template <class Row>
	class OwnedRow;

	/**
	 * @brief A descriptor heap index in a GPU row (HLSL: uint). Readable; set only by OwnedRow::Bind (which keeps the binding's owner with
	 * the row) or as a sentinel the row's reader takes for "none" (never a heap index).
	 */
	class DescriptorIndex
	{
	public:
		constexpr DescriptorIndex() = default;
		/** @brief A value that names no descriptor (the reader's "none", e.g. ~0u). */
		static constexpr DescriptorIndex Sentinel(std::uint32_t a_value)
		{
			DescriptorIndex index;
			index.value = a_value;
			return index;
		}
		constexpr std::uint32_t Value() const { return value; }

	private:
		template <class Row>
		friend class OwnedRow;
		std::uint32_t value = 0;
	};
	static_assert(sizeof(DescriptorIndex) == 4 && std::is_standard_layout_v<DescriptorIndex> && std::is_trivially_copyable_v<DescriptorIndex>);

	/**
	 * @brief A row and the owners of the descriptors its DescriptorIndex fields name. Uploaded whole (LatchedUploads, CommitUploads), its
	 * owners become the target range's content owners. Its plain fields are Plain()'s; a copy of the plain row carries no owners, so it
	 * is never uploaded on its own once a field is bound.
	 */
	template <class Row>
	class OwnedRow
	{
		static_assert(std::is_trivially_copyable_v<Row>, "a GPU row is bytes");

	public:
		OwnedRow() = default;
		explicit OwnedRow(const Row& a_plain) :
			row(a_plain) {}

		Row& Plain() { return row; }
		const Row& Plain() const { return row; }

		/** @brief a_binding's index into a_field, its owner kept with the row; false (nothing written) without one. */
		bool Bind(DescriptorIndex Row::*a_field, GpuTextures::Binding a_binding)
		{
			if (a_binding.index == GpuTextures::kInvalid)
				return false;
			(row.*a_field).value = a_binding.index;
			if (a_binding.owner)
				owners.push_back(std::move(a_binding.owner));
			return true;
		}
		/** @brief a_field set to a sentinel (default 0); an owner bound to it before stays held until the row goes. */
		void Unbind(DescriptorIndex Row::*a_field, DescriptorIndex a_sentinel = {}) { row.*a_field = a_sentinel; }

		std::span<const std::shared_ptr<const void>> Owners() const { return owners; }
		const void* Bytes() const { return &row; }
		static constexpr std::size_t kBytes = sizeof(Row);

	private:
		Row row{};
		std::vector<std::shared_ptr<const void>> owners;
	};
}
