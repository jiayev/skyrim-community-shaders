// The texture transforms' payload product: the rows of each main payload that carry TexcoordOffset (TextureTransformList), built at
// the main build's Finish, counted as a payload carries them, installed for the pool at adoption (TextureTransformsPool.cpp).

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PayloadProducts.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/MainRows.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		// The rows the frame patches TexcoordOffset into (TextureTransformList): every kept row (MainRows::materials, whatever build packed
		// it) packed from its slot's current record that places it, its slot keyed (its material held while the publication lives). All of
		// them, scrolling or not: a backing's bytes are then the frame's value whatever patches its history holds. A slot's liveness is its
		// key (cleared when freed: Slots.cpp): the slot tables' allocation state is the coordinator's alone, never published.
		std::shared_ptr<const void> Build(const Api::PayloadProductBuild& a_build)
		{
			ZoneScopedN("CS.DCLF.BuildMain.TextureTransforms");
			const auto& rows = a_build.rows;
			const auto& tables = a_build.tables;
			auto list = std::make_shared<TextureTransformList>();
			const std::size_t slots = std::min({ rows.materials.size(), tables.materials.size(), tables.materialSlotKey.size(), tables.materialVersion.size() });
			std::uint64_t unpacked = 0, noTexcoord = 0, freeSlot = 0, stale = 0;
			for (std::uint32_t m = 0; m < slots; ++m) {
				const auto& state = rows.materials[m];
				if (!state.written || !state.blocksOk) {
					++unpacked;
					continue;
				}
				if (!state.texcoordMask) {
					++noTexcoord;
					continue;
				}
				if (!tables.materialSlotKey[m].first) {
					++freeSlot;
					continue;
				}
				if (state.key[0] != tables.materialVersion[m]) {
					++stale;
					continue;
				}
				list->entries.push_back({ tables.materialSlotKey[m].first,
					static_cast<std::uint32_t>(std::uint64_t(m) * kMaterialRowBytes + kMaterialRowVS + std::uint64_t(state.texcoordDword) * 4), state.texcoordMask });
			}
			namespace stats = TextureTransformStats;
			stats::built.fetch_add(1, std::memory_order_relaxed);
			stats::builtEntries.fetch_add(list->entries.size(), std::memory_order_relaxed);
			stats::rowsUnpacked.fetch_add(unpacked, std::memory_order_relaxed);
			stats::rowsNoTexcoord.fetch_add(noTexcoord, std::memory_order_relaxed);
			stats::rowsFreeSlot.fetch_add(freeSlot, std::memory_order_relaxed);
			stats::rowsStale.fetch_add(stale, std::memory_order_relaxed);
			return list;
		}

		// A main payload publishes its list (PayloadBuilder::Publish).
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
