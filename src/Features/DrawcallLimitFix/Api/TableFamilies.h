#pragma once

// The table family contract (Api::InstallContext::RegisterTableFamily): a feature's columns held in the scene's tables (SceneTables)
// beside Core's - slots of its own (a row per engine node its members draw under, say), refcounted by the objects that reference them -
// written by the coordinator's components, published with the tables, and read by the frame and the builds.

#include <cstdint>
#include <memory>
#include <string_view>

namespace DCLF::Api
{
	/** @brief A registered family (RegisterTableFamily's result): Extensions::TableFamilies finds its columns in any tables by it. */
	struct TableFamilyId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief A table family.
	 *
	 * Every SceneTables holds one of each family: the coordinator's live tables (written on the scene lane by its components:
	 * Api/CoordinatorComponents.h) and each publication's immutable copy (read by the render thread and the builds). Core keeps beside
	 * the feature's columns:
	 * - its write stamp, which every write moves (Extensions::TableFamilyColumns::NoteWrite): the publication copies the family only when
	 *   its stamp moved since the copy it reuses ("<name> kept" in the tables' report counts the others);
	 * - the objects' references into it (objectRow, by object slot: kNoFamilyRow for none, a row, or a value at or past rowsEnd naming
	 *   no row), which bound the objects a build draws (ObjectFits: an object whose row is past what the feature's scene resources hold
	 *   waits, Api::SceneResources::fit) and which the object's record carries (recordWord);
	 * - each row's tag (rowTag: a listing's generation), carried by the record beside the row, so the GPU tells a row's listing from the
	 *   one before it;
	 * - its freed rows' retirement (SceneTables::Retire with kRetiredFamilyRow and the family's index): a row is back on the feature's
	 *   free list (recycle) once no publication names it.
	 *
	 * The callbacks run on the scene lane (the live tables' copies, clears and recycles), or wherever a publication's replay parity runs
	 * (same); none is called per object.
	 *
	 * - make: an empty family's columns.
	 * - assign: a_to's columns made equal to a_from's (the publication's copy of a moved family, the dense walk's save and restore).
	 * - assignRow (optional, a journaled family's): a_to's row a_row made equal to a_from's - its entry in each per-row column, and any
	 *   value its writes move beside them (a generation counter) - where a_to differs from a_from only in rows marked since
	 *   (Extensions::TableFamilyColumns::Mark, which a write of a row's own columns alone takes; any other write is NoteWrite's). The
	 *   publication's copy of a family only marks moved replays the marked rows by it instead of assign.
	 * - same (optional): whether two families' columns hold the same rows (CS_DCLF_PERSISTENT_PARITY's replay check of a kept family).
	 * - clear: every row gone (SceneTables::Clear), the family's content epochs moved on.
	 * - recycle: a retired row back on the family's free list.
	 *
	 * The defaults (no shift, bias or journal, a tag word, no rows) are a family's whose record carries its row and tag as they are
	 * (Features/TreeWind's trees).
	 */
	struct TableFamily
	{
		std::string_view name;  // plural: the report's "<name> kept" and the replay parity's "the <name> were kept"
		std::shared_ptr<void> (*make)() = nullptr;
		void (*assign)(void* a_to, const void* a_from) = nullptr;
		void (*assignRow)(void* a_to, const void* a_from, std::uint32_t a_row) = nullptr;
		bool (*same)(const void* a_left, const void* a_right) = nullptr;
		void (*clear)(void* a_columns) = nullptr;
		void (*recycle)(void* a_columns, std::uint32_t a_row) = nullptr;
		// The objects' references from this value up (to kNoFamilyRow, none) name no row: the feature's own meanings (Features/TreeWind's
		// nodeless trees). A record carries such a value as its row and its tag.
		std::uint32_t rowsEnd = ~0u;
		// The object record's 32-bit word (DCLFObjectRecord, Common/DCLFObjects.hlsli) the reference's row is written to, its tag to the
		// next (0 and 0 without a reference); ~0u: the record carries none. Checked by Features/Manifest.cpp.
		std::uint32_t recordWord = ~0u;
		// The reference as recordWord carries it: (row + recordBias) << recordShift, ORed into the word (Core's own bits of the word stay
		// below the shift), and 0 when it does not fit the word's bits above the shift (a root slot past 24 bits rides as none: the
		// record's root reference, bits 9-31 = root + 1). 0 and 0 keep the row as is.
		std::uint32_t recordShift = 0, recordBias = 0;
		// Whether the record carries the row's tag in the word after recordWord; false: one word, no tag (a family whose rows carry their
		// generation themselves).
		bool recordTag = true;
		// Core keeps a change journal of the rows beside the stamp (Extensions::TableFamilyColumns::Mark marks a row and moves the stamp),
		// copied with every publication, kept family or not. A commit hook sends the rows the journal names since what its buffer holds
		// (TableFamilyColumns::Journal) and reports the content epoch it sent (Extensions::TableFamilySent; render thread, latest wins); the
		// scene lane trims the journal to it at the pass's coordinator inputs (TableFamilies::TakeSent). The trim forces no publication: the
		// published journal only lists more than the holder lacks until the next publication that changed something carries it.
		bool journaled = false;
		// Optional, per revision (render thread, never per object): the rows the resources sized by the family must hold for a_columns
		// (Extensions::TableFamilyRows: Core's buffers sized by the family's rows, its visibility rows, besides the feature's own).
		std::uint32_t (*rows)(const void* a_columns) = nullptr;
	};
}
