#pragma once

#include "KeptState.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace DCLF
{
	// A kept record is an immutable template. Its dynamic texture registers are
	// resolved into a separate upload copy when their index changed, or when a
	// base-record upload overwrote the GPU's previous frame patch. The emitter
	// must consume/copy the temporary record before returning.
	template <class Record, std::size_t Registers, std::size_t Words, class Emit>
	std::uint32_t EmitFrameRecordPatches(const KeptView<Record>& a_records, std::uint64_t a_uploadedBase,
		std::span<const std::array<std::uint64_t, Words>> a_masks, const std::array<std::uint64_t, Words>& a_changed,
		const std::array<std::uint32_t, Registers>& a_indices, Emit&& a_emit)
	{
		static_assert(Words * 64 >= Registers);
		if (!a_records.elements)
			return 0;
		const auto& records = *a_records.elements;
		std::vector<std::uint8_t> baseRewritten(a_masks.size(), 0);
		a_records.changes.ForEachRun(a_uploadedBase, records.size(), [&](std::uint64_t first, std::uint64_t count) {
			const auto end = std::min<std::uint64_t>(first + count, baseRewritten.size());
			for (auto slot = first; slot < end; ++slot)
				baseRewritten[slot] = 1;
		});
		std::uint32_t patchedCount = 0;
		for (std::size_t slot = 0; slot < a_masks.size() && slot < records.size(); ++slot) {
			const auto& mask = a_masks[slot];
			bool used = false, changed = false;
			for (std::size_t word = 0; word < Words; ++word) {
				used |= mask[word] != 0;
				changed |= (mask[word] & a_changed[word]) != 0;
			}
			if (!used || (!changed && !baseRewritten[slot]))
				continue;
			Record patched = records[slot];
			for (std::size_t word = 0; word < Words; ++word)
				for (std::uint64_t bits = mask[word]; bits; bits &= bits - 1) {
					const auto index = word * 64 + std::countr_zero(bits);
					if (index < Registers)
						patched.textures[index] = a_indices[index];
				}
			a_emit(slot, patched);
			++patchedCount;
		}
		return patchedCount;
	}
}
