#pragma once

// The table family contract's Core side (Api/TableFamilies.h): the families a SceneTables holds (TableFamilies, a member of every
// tables), the typed handle a feature reads its columns by, and the calls the tables' clear, retirement and publication make.

#include "Features/DrawcallLimitFix/Api/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF::Extensions
{
	/**
	 * @brief The most families the scene's tables hold: a SceneFit bounds each one's rows (Draws::SceneFit::familyRows). A family past
	 * them fails the install: grow this first.
	 */
	inline constexpr std::uint32_t kTableFamilySlots = 4;
	/** @brief An object's reference to no row of a family (TableFamilyColumns::objectRow). */
	inline constexpr std::uint32_t kNoFamilyRow = ~0u;

	/** @brief One family of a tables: its stamp, the feature's columns, and what Core keeps of it beside them (Api::TableFamily). */
	struct TableFamilyColumns
	{
		std::uint64_t stamp = 0;  // moved by every write (NoteWrite): the publication copies the family only when it moved
		void NoteWrite() { ++stamp; }
		std::shared_ptr<void> columns;  // the feature's (Api::TableFamily::make), this tables' own: never shared with another tables
		// By object slot: the row each object references (kNoFamilyRow: none; at or past rowsEnd: no row, the feature's meaning).
		std::vector<std::uint32_t> objectRow;
		// By row: its tag (a listing's generation), which the record carries beside the row.
		std::vector<std::uint32_t> rowTag;
		// The registration's, copied with the columns (the per-object readers read no registry).
		std::uint32_t rowsEnd = ~0u, recordWord = ~0u;
		std::uint32_t recordShift = 0, recordBias = 0;
		bool recordTag = true, journaled = false;
		// A journaled family's rows written since what its commit hook's buffer holds (Api::TableFamily::journaled): Mark marks them,
		// TakeSent trims what the hook reported sent. Copied with every publication, kept family or not; unused otherwise.
		ChangeJournal journal;

		/** @brief A journaled family's row a_row written: marked in the journal (once a build), and the stamp moved. */
		void Mark(std::uint32_t a_row)
		{
			journal.Mark(a_row);
			NoteWrite();
		}
		/** @brief The journal a commit hook sends a journaled family's rows by (ForEachRun from what its buffer holds). */
		const ChangeJournal& Journal() const { return journal; }
		/** @brief The journal's content epoch: what a commit hook that sent every run holds (Extensions::TableFamilySent's a_contentEpoch). */
		std::uint64_t ContentEpoch() const { return journal.ContentEpoch(); }

		/** @brief The row a_object references, or kNoFamilyRow. */
		std::uint32_t RowOf(std::uint32_t a_object) const { return a_object < objectRow.size() ? objectRow[a_object] : kNoFamilyRow; }
		/**
		 * @brief The record's reference to a_row (Api::TableFamily::recordShift, recordBias): (row + bias) << shift, 0 when it does not
		 * fit the bits above the shift.
		 */
		std::uint32_t RecordReferenceOf(std::uint32_t a_row) const
		{
			const std::uint64_t biased = std::uint64_t(a_row) + recordBias;
			return biased < (std::uint64_t(1) << (32 - recordShift)) ? static_cast<std::uint32_t>(biased << recordShift) : 0u;
		}
		/**
		 * @brief The two record words a_object's reference is carried by (Api::TableFamily::recordWord): its reference and the row's tag
		 * (0 without recordTag).
		 */
		std::array<std::uint32_t, 2> RecordWordsOf(std::uint32_t a_object) const
		{
			const std::uint32_t row = RowOf(a_object);
			if (row == kNoFamilyRow)
				return { 0u, 0u };
			return { RecordReferenceOf(row), !recordTag ? 0u : row >= rowsEnd ? row : row < rowTag.size() ? rowTag[row] : 0u };
		}
		/**
		 * @brief a_object's reference ORed into a_record's words (recordWord, and the next with a tag): the record's Core bits of the word
		 * stay (below the shift); a word Core zeroed takes the words as they are. Nothing without a record word.
		 */
		void WriteRecordWords(std::uint32_t* a_record, std::uint32_t a_object) const
		{
			if (recordWord == ~0u)
				return;
			const auto words = RecordWordsOf(a_object);
			a_record[recordWord] |= words[0];
			if (recordTag)
				a_record[recordWord + 1] |= words[1];
		}
		/** @brief Whether a_object's reference is within a_rows rows (one past them waits for their growth: ObjectFits). */
		bool RowFits(std::uint32_t a_object, std::uint32_t a_rows) const
		{
			const std::uint32_t row = RowOf(a_object);
			return row >= rowsEnd || row < a_rows;
		}
	};

	/**
	 * @brief The families a SceneTables holds, by registration (Extensions::RegisteredTableFamilies). Empty until the coordinator first
	 * writes one (MutableColumns makes them all; Core's tables may be made before the features are installed); a family a tables does not
	 * hold reads as empty. Copied whole (each family's assign: the dense walk's save), moved and swapped by its pointers (the
	 * publication's held-out members).
	 */
	class TableFamilies
	{
	public:
		TableFamilies() = default;
		TableFamilies(const TableFamilies& a_other);
		TableFamilies& operator=(const TableFamilies& a_other);
		TableFamilies(TableFamilies&&) noexcept = default;
		TableFamilies& operator=(TableFamilies&&) noexcept = default;

		/** @brief Every family this tables holds, by registration (empty before the first write). */
		std::span<const TableFamilyColumns> All() const { return entries; }
		/** @brief a_id's family, or null when this tables holds none. */
		const TableFamilyColumns* Find(Api::TableFamilyId a_id) const { return a_id.index < entries.size() ? &entries[a_id.index] : nullptr; }
		/** @brief a_id's columns as the feature's type, or null (Find). */
		template <class T>
		const T* Columns(Api::TableFamilyId a_id) const
		{
			const auto* family = Find(a_id);
			return family ? static_cast<const T*>(family->columns.get()) : nullptr;
		}
		/** @brief The writer's (the coordinator's live tables, on the scene lane): a_id's family, every family made first if none is. */
		TableFamilyColumns& Mutable(Api::TableFamilyId a_id);
		template <class T>
		T& MutableColumns(Api::TableFamilyId a_id)
		{
			return *static_cast<T*>(Mutable(a_id).columns.get());
		}

		/** @brief SceneTables::Clear: every family's rows, references and tags gone, its stamp moved. */
		void Clear();
		/** @brief Coordinator::RecycleRetired: a_family's retired a_row back on its free list (its stamp moved). */
		void Recycle(std::uint32_t a_family, std::uint32_t a_row);
		/** @brief The stamps' sum (the publication key's: any write moves it). */
		std::uint64_t StampSum() const;
		/** @brief Whether every family's stamp is a_other's (a family neither holds stands at 0). */
		bool SameStamps(const TableFamilies& a_other) const;
		/**
		 * @brief The publication's copy (PublishTables): each family copied from a_from unless a_replayable and its stamp stood since this
		 * snapshot's (then a_kept counts it). Returns the kept families, a bit by registration.
		 */
		std::uint32_t Publish(const TableFamilies& a_from, bool a_replayable, std::array<std::uint64_t, kTableFamilySlots>& a_kept);
		/** @brief Whether a_family holds the same rows, references and tags as a_other's (the replay parity of a kept family). */
		bool Same(std::uint32_t a_family, const TableFamilies& a_other) const;
		/** @brief a_family's stamp (0 when not held). */
		std::uint64_t StampOf(std::uint32_t a_family) const { return a_family < entries.size() ? entries[a_family].stamp : 0; }
		/**
		 * @brief The scene lane, the pass's coordinator inputs (Coordinator::TakeCoordinatorInputs, first there), on the live tables: each
		 * journaled family's sent content epoch posted since the last call (TableFamilySent, latest wins) taken, its journal trimmed to it
		 * (ChangeJournal::BeginBuild). No publication is forced: the hook posts every depth commit, and a trim changes no row, so the
		 * published journal (copied by the next publication that changed something) only lists runs the holder already has, which ForEachRun
		 * skips. A family these tables do not hold yet takes nothing (its post is dropped: no row of it was ever written).
		 */
		void TakeSent();

	private:
		void MakeAll();
		void Assign(std::size_t a_family, const TableFamilyColumns& a_from);

		std::vector<TableFamilyColumns> entries;
	};

	/**
	 * @brief Render thread, a commit hook of a journaled family (Api::TableFamily::journaled), after it sent its rows: its buffer holds
	 * a_id's rows at a_contentEpoch (TableFamilyColumns::ContentEpoch of the tables it sent from). Posted latest-wins, no wake: the next
	 * pass's coordinator inputs take it (TableFamilies::TakeSent). Nothing for a family that is not journaled.
	 */
	void TableFamilySent(Api::TableFamilyId a_id, std::uint64_t a_contentEpoch);
	/**
	 * @brief Render thread, per revision (never per object): the rows the resources sized by a_family must hold for a_tables' columns
	 * (Api::TableFamily::rows); 0 without the callback or the family.
	 */
	std::uint32_t TableFamilyRows(const TableFamilies& a_tables, Api::TableFamilyId a_family);
}
