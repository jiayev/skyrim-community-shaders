// The families a SceneTables holds (see the header): made, copied, cleared, recycled and published by their registrations.

#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

#include <algorithm>
#include <atomic>

namespace DCLF::Extensions
{
	namespace
	{
		// By family: the content epoch its commit hook last reported sent (TableFamilySent; render thread -> scene lane, latest wins).
		constexpr std::uint64_t kNoneSent = ~0ull;
		struct SentSlot
		{
			std::atomic<std::uint64_t> contentEpoch{ kNoneSent };
		};
		std::array<SentSlot, kTableFamilySlots> sentPosted;
	}

	void TableFamilySent(Api::TableFamilyId a_id, std::uint64_t a_contentEpoch)
	{
		const auto registered = RegisteredTableFamilies();
		if (a_id.index >= registered.size() || !registered[a_id.index].journaled)
			return;
		// No wake: the frame's next pass takes it.
		sentPosted[a_id.index].contentEpoch.store(a_contentEpoch, std::memory_order_release);
	}

	std::uint32_t TableFamilyRows(const TableFamilies& a_tables, Api::TableFamilyId a_family)
	{
		const auto registered = RegisteredTableFamilies();
		const auto* family = a_tables.Find(a_family);
		if (!family || a_family.index >= registered.size() || !registered[a_family.index].rows || !family->columns)
			return 0;
		return registered[a_family.index].rows(family->columns.get());
	}

	void TableFamilies::TakeSent()
	{
		const auto registered = RegisteredTableFamilies();
		for (std::size_t f = 0; f < registered.size(); ++f) {
			if (!registered[f].journaled)
				continue;
			const std::uint64_t contentEpoch = sentPosted[f].contentEpoch.exchange(kNoneSent, std::memory_order_acq_rel);
			if (contentEpoch == kNoneSent)
				continue;
			// The live tables hold every family from their first write on (Mutable); none held: nothing was sent from them.
			if (f < entries.size())
				entries[f].journal.BeginBuild(contentEpoch);
		}
	}

	TableFamilies::TableFamilies(const TableFamilies& a_other)
	{
		*this = a_other;
	}

	TableFamilies& TableFamilies::operator=(const TableFamilies& a_other)
	{
		if (this == &a_other)
			return *this;
		if (a_other.entries.empty()) {
			entries.clear();
			return *this;
		}
		for (std::size_t f = 0; f < a_other.entries.size(); ++f)
			Assign(f, a_other.entries[f]);
		entries.resize(a_other.entries.size());
		return *this;
	}

	void TableFamilies::MakeAll()
	{
		const auto registered = RegisteredTableFamilies();
		if (entries.size() >= registered.size())
			return;
		const std::size_t first = entries.size();
		entries.resize(registered.size());
		for (std::size_t f = first; f < registered.size(); ++f) {
			entries[f].columns = registered[f].make();
			entries[f].rowsEnd = registered[f].rowsEnd;
			entries[f].recordWord = registered[f].recordWord;
			entries[f].recordShift = registered[f].recordShift;
			entries[f].recordBias = registered[f].recordBias;
			entries[f].recordTag = registered[f].recordTag;
			entries[f].journaled = registered[f].journaled;
		}
	}

	void TableFamilies::Assign(std::size_t a_family, const TableFamilyColumns& a_from)
	{
		const auto registered = RegisteredTableFamilies();
		if (entries.size() <= a_family)
			MakeAll();
		auto& to = entries[a_family];
		to.stamp = a_from.stamp;
		to.objectRow = a_from.objectRow;
		to.rowTag = a_from.rowTag;
		to.rowsEnd = a_from.rowsEnd;
		to.recordWord = a_from.recordWord;
		to.recordShift = a_from.recordShift;
		to.recordBias = a_from.recordBias;
		to.recordTag = a_from.recordTag;
		to.journaled = a_from.journaled;
		if (a_from.journaled)
			to.journal = a_from.journal;
		if (a_from.columns && a_family < registered.size())
			registered[a_family].assign(to.columns.get(), a_from.columns.get());
	}

	TableFamilyColumns& TableFamilies::Mutable(Api::TableFamilyId a_id)
	{
		MakeAll();
		return entries[a_id.index];
	}

	void TableFamilies::Clear()
	{
		MakeAll();
		const auto registered = RegisteredTableFamilies();
		for (std::size_t f = 0; f < entries.size(); ++f) {
			auto& family = entries[f];
			family.NoteWrite();
			family.objectRow.clear();
			family.rowTag.clear();
			registered[f].clear(family.columns.get());
			// Every holder is sent everything (its content epoch counts on).
			if (family.journaled)
				family.journal.Resync();
		}
	}

	void TableFamilies::Recycle(std::uint32_t a_family, std::uint32_t a_row)
	{
		const auto registered = RegisteredTableFamilies();
		if (a_family >= registered.size())
			return;
		MakeAll();
		auto& family = entries[a_family];
		family.NoteWrite();
		registered[a_family].recycle(family.columns.get(), a_row);
	}

	std::uint64_t TableFamilies::StampSum() const
	{
		std::uint64_t sum = 0;
		for (const auto& family : entries)
			sum += family.stamp;
		return sum;
	}

	bool TableFamilies::SameStamps(const TableFamilies& a_other) const
	{
		for (std::size_t f = 0; f < std::max(entries.size(), a_other.entries.size()); ++f)
			if (StampOf(static_cast<std::uint32_t>(f)) != a_other.StampOf(static_cast<std::uint32_t>(f)))
				return false;
		return true;
	}

	std::uint32_t TableFamilies::Publish(const TableFamilies& a_from, bool a_replayable, std::array<std::uint64_t, kTableFamilySlots>& a_kept)
	{
		const auto registered = RegisteredTableFamilies();
		std::uint32_t kept = 0;
		for (std::size_t f = 0; f < registered.size(); ++f) {
			const auto family = static_cast<std::uint32_t>(f);
			if (a_replayable && StampOf(family) == a_from.StampOf(family)) {
				++a_kept[f];
				kept |= 1u << f;
				// The journal is copied whatever the stamp: a sent epoch trims it without a write.
				if (f < entries.size() && f < a_from.entries.size() && a_from.entries[f].journaled)
					entries[f].journal = a_from.entries[f].journal;
				continue;
			}
			// The live tables hold every family from their first write on, and their stamps only move: one they do not hold yet is one
			// neither side wrote.
			if (f < a_from.entries.size())
				Assign(f, a_from.entries[f]);
		}
		return kept;
	}

	bool TableFamilies::Same(std::uint32_t a_family, const TableFamilies& a_other) const
	{
		const auto registered = RegisteredTableFamilies();
		const auto* left = a_family < entries.size() ? &entries[a_family] : nullptr;
		const auto* right = a_family < a_other.entries.size() ? &a_other.entries[a_family] : nullptr;
		const bool compared = a_family < registered.size() && registered[a_family].same;
		// A family one side does not hold reads as empty (a tables made before the first write, kept since).
		if (!left || !right) {
			const auto* held = left ? left : right;
			if (!held)
				return true;
			if (!held->objectRow.empty() || !held->rowTag.empty())
				return false;
			return !compared || registered[a_family].same(held->columns.get(), registered[a_family].make().get());
		}
		if (left->objectRow != right->objectRow || left->rowTag != right->rowTag)
			return false;
		return !compared || registered[a_family].same(left->columns.get(), right->columns.get());
	}
}
