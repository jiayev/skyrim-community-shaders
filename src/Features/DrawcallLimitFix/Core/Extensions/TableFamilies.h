#pragma once

// The table family contract's Core side (Api/TableFamilies.h): the families a SceneTables holds (TableFamilies, a member of every
// tables), the typed handle a feature reads its columns by, and the calls the tables' clear, retirement and publication make.

#include "Features/DrawcallLimitFix/Api/TableFamilies.h"

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

		/** @brief The row a_object references, or kNoFamilyRow. */
		std::uint32_t RowOf(std::uint32_t a_object) const { return a_object < objectRow.size() ? objectRow[a_object] : kNoFamilyRow; }
		/** @brief The two record words a_object's reference is carried by (Api::TableFamily::recordWord): its row and the row's tag. */
		std::array<std::uint32_t, 2> RecordWordsOf(std::uint32_t a_object) const
		{
			const std::uint32_t row = RowOf(a_object);
			if (row == kNoFamilyRow)
				return { 0u, 0u };
			return { row, row >= rowsEnd ? row : row < rowTag.size() ? rowTag[row] : 0u };
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

	private:
		void MakeAll();
		void Assign(std::size_t a_family, const TableFamilyColumns& a_from);

		std::vector<TableFamilyColumns> entries;
	};
}
