#pragma once

// The main rows (MainRows): written by the main build (DCLF's pool), read by the commit on the render thread and by the payload
// products (Api::PayloadProductBuild::rows). Self-contained (features include it); MainPayload.h includes it.

#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PayloadProducts.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace DCLF::Draws
{
	/**
	 * @brief A main-pass row's bytes (DrawPipelines.h: kMaterialRowBytes, kPipelineRowBytes). It holds no addresses (the draws'
	 * sequences carry their blocks', BuildDrawsCS), so a backing takes it as it is (EmitMainRows).
	 */
	template <std::size_t Bytes>
	struct RowBytes
	{
		std::array<std::byte, Bytes> bytes{};
	};
	using MaterialRow = RowBytes<kMaterialRowBytes>;
	using PipelineRow = RowBytes<kPipelineRowBytes>;
	inline MaterialRowHeader& HeaderOf(MaterialRow& a_row) { return *reinterpret_cast<MaterialRowHeader*>(a_row.bytes.data() + kMaterialRowHeader); }
	inline PipelineRowHeader& HeaderOf(PipelineRow& a_row) { return *reinterpret_cast<PipelineRowHeader*>(a_row.bytes.data() + kPipelineRowHeader); }

	/**
	 * @brief The main pass's rows, kept across frames and shared by the Z-prepass and colour segments (one build makes both):
	 * a material row per material slot and a pipeline row per pipeline slot, each
	 * written again only when what it is written from changed (its key), and sent as what changed since the content epoch the
	 * table holds. What a pair of them can draw is checked per build (MainBuild::AssembleRecord), from what each row holds.
	 */
	struct MainRows
	{
		KeptArray<MaterialRow> material;
		KeptArray<PipelineRow> pipeline;
		/** @brief Where a row's block placed a frame variable (PlaceFrameVariables): its first component's byte in the row, the components (bit c). */
		struct FramePlacement
		{
			std::uint16_t rowByte = 0;
			std::uint8_t mask = 0;  // 0: not placed
			bool operator==(const FramePlacement&) const = default;
		};
		struct MaterialState
		{
			std::array<std::uint32_t, 8> key{};
			bool written = false;
			bool texturesOk = false;  // every texture the material sets resolved (else deferred: waiting for them)
			bool deferred = false;
			std::uint32_t missingTexture = 0;
			std::uint32_t textures = 0;  // t0-t15 the row gives a descriptor (bit per register)
			std::uint32_t samplers = 0;  // s0-s15 likewise
			std::uint32_t features = 0;  // the feature texture slots likewise (Extensions::FeatureTextureRegister)
			std::uint64_t tables = 0;    // the constant tables its blocks were packed with (a hash)
			bool blocksOk = false;       // both blocks fit their places
			// Where its VS block placed TexcoordOffset (the frame's, which a feature patches on the GPU from a payload product): the packed
			// dword, ~0 none, and the components the record wrote.
			std::uint32_t texcoordDword = ~0u;
			std::uint8_t texcoordMask = 0;
			// Where its blocks placed its kind's declared frame variables (Api::MaterialFrameVariable, by declaration), which the same feature
			// patches likewise.
			std::array<FramePlacement, Api::kMaxMaterialFrameVariables> frameVariables{};
		};
		struct PipelineState
		{
			std::array<std::uint32_t, 8> key{};
			bool written = false;
			bool blocksOk = false;
			bool shadowMask = false, shadowMaskSampler = false;  // t14, s14 given
		};
		std::vector<MaterialState> materials;
		std::vector<PipelineState> pipelines;
		/**
		 * @brief The material rows a frame patch may write (Animated: TexcoordOffset or a declared frame variable placed), each given a row of
		 * the frame's copy (MaterialRowHeader::animatedRow; Features/TextureTransforms copies them there and patches the copy, the installed
		 * rows stay as built). A row keeps its index while it stays animated; a freed index is the lowest one taken next, and the count is the
		 * highest held plus one (the trailing free ones dropped), so an index moves only with its own row. Assigned on the builder's thread at
		 * MainBuild::Finish from the slots the build wrote; WriteMaterialRow reads rowOf (pool chunks too: Finish alone writes it).
		 */
		struct AnimatedRows
		{
			std::vector<std::uint32_t> slotOf;  // per animated row: its material slot, ~0u free
			std::vector<std::uint32_t> rowOf;   // per material slot: its animated row, ~0u none
			std::vector<std::uint32_t> free;    // free rows below slotOf.size(), a min-heap (stale entries skipped as taken)

			static bool Animated(const MaterialState& a_state)
			{
				return a_state.written && a_state.blocksOk &&
				       (a_state.texcoordMask || std::any_of(a_state.frameVariables.begin(), a_state.frameVariables.end(), [](const auto& a_place) { return a_place.mask != 0; }));
			}
			std::uint32_t RowOf(std::uint32_t a_slot) const { return a_slot < rowOf.size() ? rowOf[a_slot] : ~0u; }
			std::uint32_t Count() const { return static_cast<std::uint32_t>(slotOf.size()); }
			/** @brief a_slot's row, taken when it has none. */
			std::uint32_t Assign(std::uint32_t a_slot)
			{
				if (const std::uint32_t held = RowOf(a_slot); held != ~0u)
					return held;
				std::uint32_t row = ~0u;
				while (!free.empty() && row == ~0u) {
					std::pop_heap(free.begin(), free.end(), std::greater<>{});
					const std::uint32_t candidate = free.back();
					free.pop_back();
					if (candidate < slotOf.size() && slotOf[candidate] == ~0u)
						row = candidate;
				}
				if (row == ~0u) {
					row = static_cast<std::uint32_t>(slotOf.size());
					slotOf.push_back(~0u);
				}
				slotOf[row] = a_slot;
				if (rowOf.size() <= a_slot)
					rowOf.resize(std::size_t(a_slot) + 1, ~0u);
				rowOf[a_slot] = row;
				return row;
			}
			/** @brief a_slot's row let go (none: nothing). */
			void Release(std::uint32_t a_slot)
			{
				const std::uint32_t row = RowOf(a_slot);
				if (row == ~0u)
					return;
				rowOf[a_slot] = ~0u;
				slotOf[row] = ~0u;
				if (row + 1 == slotOf.size()) {
					while (!slotOf.empty() && slotOf.back() == ~0u)
						slotOf.pop_back();
				} else {
					free.push_back(row);
					std::push_heap(free.begin(), free.end(), std::greater<>{});
				}
			}
			void Clear()
			{
				slotOf.clear();
				rowOf.clear();
				free.clear();
			}
		} animated;
		const void* identity = nullptr;
		std::uint32_t generation = 0;  // the tables generation its rows were written for
		bool active = false;
		// Since the last report.
		std::uint64_t builds = 0, materialsWritten = 0, pipelinesWritten = 0, resyncs = 0;
		// Material rows written again, by the key part that moved (WriteMaterialRow): record, frame values, lookup, shared lookups,
		// technique bindings, projected, constant tables (two words).
		std::array<std::uint64_t, 8> keyMoved{};
		std::atomic<std::uint32_t> busy{ 0 };
		// Per pipeline slot: its constant tables' hash (TablesHash) and the lookup entry it was taken from (the lookups' instance and the
		// entry's version, which moves with the tables), so a build hashes only the entries that changed.
		struct TablesHashOf
		{
			std::uint64_t instance = 0;
			std::uint32_t version = 0;
			std::uint64_t hash = 0;
		};
		std::vector<TablesHashOf> tablesHashes;
		/**
		 * @brief The material slots whose payload products may have moved since their last build (Api::PayloadProductBuild: the dirty
		 * slots, the previous products): a row's state written again (WriteMaterialRow; a pool chunk's through MergeWork), a slot the tables'
		 * material log names (a record written, a slot keyed or let go), every slot after a resync or a log the cursor cannot continue.
		 * The builder's thread alone; cleared when the products are built (MainBuild::Finish).
		 */
		struct ProductDirty
		{
			std::vector<std::uint32_t> slots;  // unique, in marking order
			std::vector<std::uint8_t> marked;  // per material slot: in slots
			bool every = true;
			LogCursor cursor;  // the tables' material log
			void Mark(std::uint32_t a_slot)
			{
				if (every)
					return;
				if (marked.size() <= a_slot)
					marked.resize(std::size_t(a_slot) + 1, 0);
				if (!marked[a_slot]) {
					marked[a_slot] = 1;
					slots.push_back(a_slot);
				}
			}
			void MarkEvery()
			{
				Clear();
				every = true;
			}
			void Clear()
			{
				for (const std::uint32_t slot : slots)
					marked[slot] = 0;
				slots.clear();
				every = false;
			}
		} productDirty;
		// The products the last build made (MainBody::products): what the next build's are made from (Api::PayloadProductBuild::previous).
		Extensions::PayloadProducts products;
		// The region's pair work and the builder's stages (MainBuild::UpdateRegionPairs, RunAhead), since the last report (MainPairsReport).
		struct PairStats
		{
			std::uint64_t builds = 0;          // builds that updated the region's pairs
			std::uint64_t checked = 0;         // pairs the events named (the check list)
			std::uint64_t resolved = 0;        // of those, resolved again (their witness moved)
			std::uint64_t everyPair = 0;       // builds that checked every pair (frame witness moved, a log lost)
			std::uint64_t pipelinesMoved = 0;  // pipelines whose witness moved (their pairs checked)
			std::uint64_t fannedOut = 0, chunks = 0;  // builds that resolved on the pool, and their chunks
			std::uint64_t entriesRewritten = 0;       // region entries Pairs.Inputs rewrote
			std::uint64_t bundles = 0;                // owner bundles made again
			std::uint64_t resyncs = 0, resyncsFannedOut = 0;
			std::uint64_t loopListed = 0;             // the region's loop list (RunObjectLoop's slots), summed over its builds
			double resolveMs = 0, resolveMaxMs = 0, inputsMs = 0, bundleMs = 0;
			// RunAhead: the stages, and the builds whose stream views and main build overlapped on the pool.
			std::uint64_t aheads = 0, overlapped = 0;
			double streamsMs = 0, mainMs = 0, stagesMs = 0, shadowMs = 0, exclusionsMs = 0;
			// Parity observers (Policy::CheckBuilderPairs, Policy::CheckRowPacking).
			std::uint64_t parityChecks = 0, parityMismatches = 0, packingChecks = 0, packingConflicts = 0;
		} pairStats;
	};
	/** @brief The `[DCLF] main pairs` report line from a_rows.pairStats, which it resets (the snapshot builder's, with StoresReport). */
	std::string MainPairsReport(MainRows& a_rows);

	/** @brief One stage of a material row's packing (MainBuild::PackMaterialBlocks'): the record's space, the pipeline's table, the group. */
	struct MaterialRowStage
	{
		const StageLayout& layout;
		std::span<const std::uint8_t> table;
		std::uint64_t group = 0;
		std::uint32_t first = 0;
	};
	/**
	 * @brief Where a material row's packed blocks placed the variables the frame owns, which a feature patches on the GPU from a payload
	 * product (Features/TextureTransforms): its kind's TexcoordOffset (rules.frameVSVariable: a_state.texcoordDword, texcoordMask) and its
	 * declared frame variables (a_state.frameVariables). Each takes the components the record wrote that sit at consecutive packed dwords
	 * from the first inside the block. MainBuild::PackMaterialBlocks, the blocks packed (they fit, the record's kind a_kind the key's).
	 */
	void PlaceFrameVariables(const MaterialRecord& a_material, std::uint8_t a_kind, const MaterialRowStage& a_vs, const MaterialRowStage& a_ps,
		MainRows::MaterialState& a_state);
}
