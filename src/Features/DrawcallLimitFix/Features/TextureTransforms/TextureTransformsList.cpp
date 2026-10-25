// The texture transforms' payload product: the rows of each main payload that carry TexcoordOffset or a declared frame variable
// (TextureTransformList), built at the main build's Finish from the previous list and the material slots that moved since (the rest kept,
// the same list when none did), counted as a payload carries them, installed for the pool at adoption (TextureTransformsPool.cpp).

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PayloadProducts.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/MainRows.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		// A slot's verdict in a list (TextureTransformList::slotVerdicts): listed, or left out (leftOut's reason, less one).
		enum Verdict : std::uint8_t
		{
			kListed,
			kUnpacked,
			kNoTexcoord,
			kFreeSlot,
			kStale
		};

		// The rows the frame patches TexcoordOffset and the declared frame variables into (TextureTransformList): every kept row
		// (MainRows::materials, whatever build packed it) packed from its slot's current record that places one, its slot keyed (its material
		// held while the publication lives), an entry per variable placed. All of them, moving or not: a backing's bytes are then the frame's
		// value whatever patches its history holds. A declared variable's read is its port's for the row's pass (the slot key's). A slot's
		// liveness is its key (cleared when freed: Slots.cpp): the slot tables' allocation state is the coordinator's alone, never published.
		// Slot m's entries appended to a_list (TexcoordOffset, then the declared variables in declaration order), and its verdict.
		Verdict ListSlot(const Draws::MainRows& a_rows, const SceneTables& a_tables, std::uint32_t m, TextureTransformList& a_list)
		{
			const auto& state = a_rows.materials[m];
			if (!state.written || !state.blocksOk)
				return kUnpacked;
			const bool declared = std::any_of(state.frameVariables.begin(), state.frameVariables.end(), [](const auto& a_place) { return a_place.mask != 0; });
			if (!state.texcoordMask && !declared)
				return kNoTexcoord;
			if (!a_tables.materialSlotKey[m].first)
				return kFreeSlot;
			if (state.key[0] != a_tables.materialVersion[m])
				return kStale;
			const std::uint64_t row = std::uint64_t(m) * kMaterialRowBytes;
			if (state.texcoordMask) {
				a_list.entries.push_back({ a_tables.materialSlotKey[m].first, static_cast<std::uint32_t>(row + kMaterialRowVS + std::uint64_t(state.texcoordDword) * 4),
					state.texcoordMask });
				a_list.entrySlots.push_back(m);
			}
			if (!declared)
				return kListed;
			const auto variables = Extensions::FrameVariablesOf(a_tables.materials[m].kind);
			const std::uint32_t pass = a_tables.materialSlotKey[m].second;
			for (std::size_t i = 0; i < variables.size() && i < state.frameVariables.size(); ++i)
				if (const auto& place = state.frameVariables[i]; place.mask) {
					a_list.entries.push_back({ a_tables.materialSlotKey[m].first, static_cast<std::uint32_t>(row + place.rowByte), place.mask,
						variables[i].ComponentsFor(pass).data() });
					a_list.entrySlots.push_back(m);
					++a_list.variables;
				}
			return kListed;
		}

		// Slot m listed into a_list, its verdict noted and counted.
		void SetSlot(const Draws::MainRows& a_rows, const SceneTables& a_tables, std::uint32_t m, TextureTransformList& a_list)
		{
			const Verdict verdict = ListSlot(a_rows, a_tables, m, a_list);
			a_list.slotVerdicts[m] = verdict;
			if (verdict != kListed)
				++a_list.leftOut[verdict - 1];
		}

		std::shared_ptr<TextureTransformList> BuildWhole(const Draws::MainRows& a_rows, const SceneTables& a_tables, std::size_t a_slots)
		{
			auto list = std::make_shared<TextureTransformList>();
			list->slotVerdicts.resize(a_slots);
			for (std::uint32_t m = 0; m < a_slots; ++m)
				SetSlot(a_rows, a_tables, m, *list);
			return list;
		}

		// a_previous with the slots that moved (a_moved: sorted, unique, below a_slots; the slots past a_previous among them) listed again
		// and the rest of its entries kept in place: the list a whole build makes, entry for entry.
		std::shared_ptr<TextureTransformList> BuildFrom(const TextureTransformList& a_previous, const Draws::MainRows& a_rows, const SceneTables& a_tables, std::size_t a_slots,
			std::span<const std::uint32_t> a_moved)
		{
			auto list = std::make_shared<TextureTransformList>();
			const auto& entries = a_previous.entries;
			const auto& entrySlots = a_previous.entrySlots;
			list->entries.reserve(entries.size() + a_moved.size());
			list->entrySlots.reserve(entries.size() + a_moved.size());
			list->slotVerdicts = a_previous.slotVerdicts;
			list->slotVerdicts.resize(a_slots, kUnpacked);
			list->leftOut = a_previous.leftOut;
			list->variables = a_previous.variables;
			const std::size_t covered = a_previous.slotVerdicts.size();
			std::size_t at = 0;  // a_previous's next entry to keep
			const auto keep = [&](std::size_t a_end) {
				list->entries.insert(list->entries.end(), entries.begin() + static_cast<std::ptrdiff_t>(at), entries.begin() + static_cast<std::ptrdiff_t>(a_end));
				list->entrySlots.insert(list->entrySlots.end(), entrySlots.begin() + static_cast<std::ptrdiff_t>(at), entrySlots.begin() + static_cast<std::ptrdiff_t>(a_end));
				at = a_end;
			};
			for (const std::uint32_t m : a_moved) {
				keep(static_cast<std::size_t>(std::lower_bound(entrySlots.begin() + static_cast<std::ptrdiff_t>(at), entrySlots.end(), m) - entrySlots.begin()));
				// Its old entries and verdict let go.
				for (; at < entries.size() && entrySlots[at] == m; ++at)
					list->variables -= entries[at].components ? 1u : 0u;
				if (m < covered && a_previous.slotVerdicts[m] != kListed)
					--list->leftOut[a_previous.slotVerdicts[m] - 1];
				SetSlot(a_rows, a_tables, m, *list);
			}
			keep(entries.size());
			return list;
		}

		std::shared_ptr<const void> Build(const Api::PayloadProductBuild& a_build)
		{
			ZoneScopedN("CS.DCLF.BuildMain.TextureTransforms");
			const auto& rows = a_build.rows;
			const auto& tables = a_build.tables;
			const std::size_t slots = std::min({ rows.materials.size(), tables.materials.size(), tables.materialSlotKey.size(), tables.materialVersion.size() });
			// From the previous list when there is one to continue: the slots that moved, and those past it (new slots).
			std::shared_ptr<const TextureTransformList> list;
			const auto previous =
				a_build.previous && !a_build.everyMaterial ? std::static_pointer_cast<const TextureTransformList>(*a_build.previous) : std::shared_ptr<const TextureTransformList>{};
			if (previous && previous->slotVerdicts.size() <= slots) {
				// The builds run one at a time (the snapshot builder's): the moved slots' scratch is kept across them.
				static std::vector<std::uint32_t> moved;
				moved.clear();
				for (const std::uint32_t m : a_build.dirtyMaterials)
					if (m < previous->slotVerdicts.size())
						moved.push_back(m);
				for (std::size_t m = previous->slotVerdicts.size(); m < slots; ++m)
					moved.push_back(static_cast<std::uint32_t>(m));
				std::sort(moved.begin(), moved.end());
				list = moved.empty() ? previous : BuildFrom(*previous, rows, tables, slots, moved);
			} else {
				list = BuildWhole(rows, tables, slots);
			}
			// The rows' animated rows as this build left them (MainBuild::AssignAnimatedRows): a list kept whole takes them only when they moved.
			if (list->animatedSlots != rows.animated.slotOf) {
				auto withRows = list == previous ? std::make_shared<TextureTransformList>(*list) : std::const_pointer_cast<TextureTransformList>(list);
				withRows->animatedSlots = rows.animated.slotOf;
				list = std::move(withRows);
			}
			if (a_build.check)
				CheckListParity(*list, *BuildWhole(rows, tables, slots));
			namespace stats = TextureTransformStats;
			stats::built.fetch_add(1, std::memory_order_relaxed);
			stats::builtEntries.fetch_add(list->entries.size(), std::memory_order_relaxed);
			stats::builtVariables.fetch_add(list->variables, std::memory_order_relaxed);
			stats::rowsUnpacked.fetch_add(list->leftOut[kUnpacked - 1], std::memory_order_relaxed);
			stats::rowsNoTexcoord.fetch_add(list->leftOut[kNoTexcoord - 1], std::memory_order_relaxed);
			stats::rowsFreeSlot.fetch_add(list->leftOut[kFreeSlot - 1], std::memory_order_relaxed);
			stats::rowsStale.fetch_add(list->leftOut[kStale - 1], std::memory_order_relaxed);
			return list;
		}

		// A main payload publishes its list (PayloadPools::MakeSource).
		void Carried(const std::shared_ptr<const void>& a_product)
		{
			if (a_product) {
				TextureTransformStats::published.fetch_add(1, std::memory_order_relaxed);
				TextureTransformStats::publishedEntries.fetch_add(static_cast<const TextureTransformList*>(a_product.get())->entries.size(), std::memory_order_relaxed);
			} else {
				TextureTransformStats::publishedNone.fetch_add(1, std::memory_order_relaxed);  // a main payload without its list: a defect
			}
		}
	}

	void RegisterProduct(Api::InstallContext& a_context)
	{
		a_context.RegisterPayloadProduct({ .name = "texture-transforms", .build = &Build, .carried = &Carried, .install = &InstallList });
	}
}
