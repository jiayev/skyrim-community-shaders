// Tree LOD's engine side: the hooks on the refill, block update, AddGroup and RemoveGroup (any thread), the source of the feature's tree
// LOD channel (events onto events through Push, which the render thread's mirror drains: TreeLodMirror.h).

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodMirror.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace DCLF::Features::TreeLod
{
	namespace
	{
		// The family's records (its layout: TreeInstancesLayout.h) and engine shapes.
		namespace Layout = ::DCLF::TreeInstances;

		constexpr std::uintptr_t kRefill = 0x504a40;            // FUN_140504a40(block, group)
		constexpr std::uintptr_t kRefillSites[] = { 0x5037f6,   // the block's attach (FUN_140503630)
			0x503fbf };                                         // its visibility update (FUN_140503f70)
		constexpr std::uintptr_t kBlockUpdate = 0x503f70;       // FUN_140503f70(block): refills the groups not up to date
		constexpr std::uintptr_t kBlockUpdateSite = 0x5108ff;   // its one call, in the node update (FUN_140510730)
		constexpr std::uintptr_t kAddGroup = 0xe1d7d0;          // BSMultiStreamInstanceTriShape::AddGroup, vfunc 0x1E0
		constexpr std::uintptr_t kRemoveGroup = 0xe1e2e0;       // RemoveGroup, vfunc 0x1E8
		constexpr std::size_t kAddGroupSlot = 0x1E0 / 8, kRemoveGroupSlot = 0x1E8 / 8;
		// The tree types (the attach's and the refill's): an array of 0x28-byte entries (width +4, height +8, base shape +0x20)
		// and their count; a group's type past the count takes the first entry.
		constexpr std::uintptr_t kTreeTypes = 0x314d170, kTreeTypeCount = 0x314d180;
		constexpr std::size_t kTreeTypeBytes = 0x28;

		// The block and group the refill on this thread is packing, for AddGroup.
		struct Refilling
		{
			const void* block = nullptr;
			const void* group = nullptr;
		};
		thread_local Refilling refilling;

		struct Refill
		{
			static void thunk(void* a_block, void* a_group)
			{
				const auto saved = std::exchange(refilling, Refilling{ a_block, a_group });
				func(a_block, a_group);
				refilling = saved;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AddGroup
		{
			static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a_shape, std::uint32_t a_count, const std::uint16_t* a_data, std::uint32_t a_shorts, float a_radius)
			{
				const auto index = func(a_shape, a_count, a_data, a_shorts, a_radius);
				if (refilling.group && a_shorts == Layout::kInstanceShorts) {
					Event event{ a_shape, refilling.block, refilling.group, std::vector<Layout::Instance>(a_count), Event::Kind::Add };
					if (a_count)
						std::memcpy(event.instances.data(), a_data, a_count * sizeof(Layout::Instance));
					// The shape's placement (AddGroup's bound uses its local translation), its type's extent, and its mesh, whose
					// buffers are held until the render thread leases them.
					const auto& translate = a_shape->local.translate;
					event.translate = { translate.x, translate.y, translate.z };
					const auto* types = Engine::Global<const std::byte*>(kTreeTypes);
					const auto type = static_cast<std::uint8_t>(static_cast<const RE::BGSDistantTreeBlock::TreeGroup*>(refilling.group)->treeType);
					if (types) {
						const auto* entry = types + (type < Engine::Global<std::uint32_t>(kTreeTypeCount) ? type * kTreeTypeBytes : 0);
						event.extent = std::max(*reinterpret_cast<const float*>(entry + 4), *reinterpret_cast<const float*>(entry + 8));
					}
					if (const auto* data = a_shape->GetGeometryRuntimeData().rendererData) {
						event.mesh.key = data;
						event.mesh.vertexBuffer.copy_from(reinterpret_cast<ID3D11Buffer*>(data->vertexBuffer));
						event.mesh.indexBuffer.copy_from(reinterpret_cast<ID3D11Buffer*>(data->indexBuffer));
						event.mesh.vertexDesc = std::bit_cast<std::uint64_t>(data->vertexDesc);
						event.mesh.indexCount = static_cast<std::uint32_t>(static_cast<RE::BSTriShape*>(a_shape)->GetTrishapeRuntimeData().triangleCount) * 3;
					}
					Push(std::move(event));
				}
				return index;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct RemoveGroup
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* a_shape, std::uint32_t a_index)
			{
				func(a_shape, a_index);
				if (Layout::IsEngineShape(a_shape))
					Push(Event{ a_shape, nullptr, nullptr, {}, Event::Kind::Remove });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/*
		 * The block's update, at its one call (dclf-open-defects.md, "Tree LOD instances the engine leaves hidden after
		 * allVisible"). The node update sets allVisible, or has the block's visibility updated, then calls it; it refills only
		 * the groups whose shaderPropertyUpToDate is clear, so a group whose instances were hidden keeps them at scale 0 after
		 * allVisible is set. DCLF draws by the rule instead: every group of the block packed again with the block's allVisible,
		 * on this thread (the one that writes the groups), as events the drain applies where they change the records.
		 */
		struct BlockUpdate
		{
			static void thunk(RE::BGSDistantTreeBlock* a_block)
			{
				func(a_block);
				// Only a group with a hidden instance packs differently with allVisible set and clear: the engine's records can be either
				// (it refills only groups whose instances changed), and the drain applies the rule's where they differ.
				for (auto* group : a_block->treeGroups) {
					if (!group || !group->geometry)
						continue;
					const auto count = std::min<std::uint32_t>(group->instances.size(), Layout::kMaxGroupInstances);
					bool hidden = false;
					for (std::uint32_t i = 0; i < count && !hidden; ++i)
						hidden = (reinterpret_cast<const std::uint8_t&>(group->instances[i].hidden) & 1) != 0;
					if (!hidden)
						continue;
					Event event{ group->geometry.get(), a_block, group, {}, Event::Kind::Add, true };
					PackGroup(a_block->allVisible, *group, event.instances);
					Push(std::move(event));
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks()
	{
		auto at = [](std::uintptr_t a_offset) { return REL::Offset(a_offset).address(); };
		bool ok = true;
		for (const auto site : kRefillSites)
			ok = ok && Engine::CallsTo(at(site), at(kRefill));
		ok = ok && Engine::CallsTo(at(kBlockUpdateSite), at(kBlockUpdate));
		const auto* vtable = reinterpret_cast<const std::uintptr_t*>(RE::VTABLE_BSMultiStreamInstanceTriShape[0].address());
		ok = ok && vtable[kAddGroupSlot] == at(kAddGroup) && vtable[kRemoveGroupSlot] == at(kRemoveGroup);
		if (!ok) {
			logger::warn("[DCLF] tree LOD: the refill's or the block update's calls, or the instance shape's group slots differ; tree LOD is not mirrored");
			return;
		}
		for (const auto site : kRefillSites)
			stl::write_thunk_call<Refill>(at(site));
		stl::write_thunk_call<BlockUpdate>(at(kBlockUpdateSite));
		stl::write_vfunc<kAddGroupSlot, AddGroup>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
		stl::write_vfunc<kRemoveGroupSlot, RemoveGroup>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
		installed = true;
	}
}
