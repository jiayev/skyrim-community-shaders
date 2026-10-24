// Tree LOD's engine side: the refill, AddGroup and RemoveGroup (SceneWriterFramework's tree LOD calls) and the block update's call (any
// thread), the source of the feature's tree LOD channel (events onto events through Push, which the render thread's mirror drains:
// TreeLodMirror.h).

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
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

		// The refill (FUN_140504a40(block, group), at the block's attach and its visibility update) and the instance shape's AddGroup and
		// RemoveGroup (vfuncs 0x1E0, 0x1E8) are SceneWriterFramework's tree LOD calls.
		constexpr Engine::Offset kBlockUpdate{ { 31659 }, { 30838 } };      // FUN_140503f70(block): refills the groups not up to date
		constexpr Engine::Offset kBlockUpdateSite{ { 31838, 0x1cf }, { 31051, 0x1df } };  // its one call, in the node update (FUN_140510730)
		// The tree types (the attach's and the refill's): an array of 0x28-byte entries (width +4, height +8, base shape +0x20)
		// and their count; a group's type past the count takes the first entry.
		constexpr Engine::Offset kTreeTypes{ { 402201 }, { 516088 } }, kTreeTypeCount{ { 402203 }, { 516090 } };
		constexpr std::size_t kTreeTypeBytes = 0x28;

		// The block and group the refill on this thread is packing, for AddGroup.
		struct Refilling
		{
			const void* block = nullptr;
			const void* group = nullptr;
		};
		thread_local Refilling refilling;

		// The refill: PRE saves the outer refill's block and group (scratch[0], [1]) and sets this one's; POST restores them.
		void OnRefill(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			if (SceneWriter::IsPre(a_event)) {
				a_scratch[0] = reinterpret_cast<std::uintptr_t>(refilling.block);
				a_scratch[1] = reinterpret_cast<std::uintptr_t>(refilling.group);
				refilling = { SceneWriter::Arg<const void*>(a_event, 0), SceneWriter::Arg<const void*>(a_event, 1) };
				return;
			}
			refilling = { reinterpret_cast<const void*>(a_scratch[0]), reinterpret_cast<const void*>(a_scratch[1]) };
		}

		// AddGroup, after the call (its data is the caller's, alive until it returns).
		void OnAddGroup(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* shape = SceneWriter::Arg<RE::BSMultiStreamInstanceTriShape*>(a_event, 0);
			const auto count = SceneWriter::Arg<std::uint32_t>(a_event, 1);
			const auto* data = SceneWriter::Arg<const std::uint16_t*>(a_event, 2);
			const auto shorts = SceneWriter::Arg<std::uint32_t>(a_event, 3);
			if (refilling.group && shorts == Layout::kInstanceShorts) {
				Event event{ shape, refilling.block, refilling.group, std::vector<Layout::Instance>(count), Event::Kind::Add };
				if (count)
					std::memcpy(event.instances.data(), data, count * sizeof(Layout::Instance));
				// The shape's placement (AddGroup's bound uses its local translation), its type's extent, and its mesh, whose
				// buffers are held until the render thread leases them.
				const auto& translate = shape->local.translate;
				event.translate = { translate.x, translate.y, translate.z };
				const auto* types = Engine::Global<const std::byte*>(kTreeTypes);
				const auto type = static_cast<std::uint8_t>(static_cast<const RE::BGSDistantTreeBlock::TreeGroup*>(refilling.group)->treeType);
				if (types) {
					const auto* entry = types + (type < Engine::Global<std::uint32_t>(kTreeTypeCount) ? type * kTreeTypeBytes : 0);
					event.extent = std::max(*reinterpret_cast<const float*>(entry + 4), *reinterpret_cast<const float*>(entry + 8));
				}
				if (const auto* renderer = shape->GetGeometryRuntimeData().rendererData) {
					event.mesh.key = renderer;
					event.mesh.vertexBuffer.copy_from(reinterpret_cast<ID3D11Buffer*>(renderer->vertexBuffer));
					event.mesh.indexBuffer.copy_from(reinterpret_cast<ID3D11Buffer*>(renderer->indexBuffer));
					event.mesh.vertexDesc = std::bit_cast<std::uint64_t>(renderer->vertexDesc);
					event.mesh.indexCount = static_cast<std::uint32_t>(static_cast<RE::BSTriShape*>(shape)->GetTrishapeRuntimeData().triangleCount) * 3;
				}
				Push(std::move(event));
			}
		}

		void OnRemoveGroup(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* shape = SceneWriter::Arg<RE::BSMultiStreamInstanceTriShape*>(a_event, 0);
			if (Layout::IsEngineShape(shape))
				Push(Event{ shape, nullptr, nullptr, {}, Event::Kind::Remove });
		}

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
		if (!SceneWriter::Has(SWF_CAP_TREE_LOD_CALLS) || !Engine::CallsTo(at(kBlockUpdateSite), at(kBlockUpdate))) {
			logger::warn("[DCLF] tree LOD: the scene writer framework has no tree LOD calls, or the block update's call differs; tree LOD is not mirrored");
			return;
		}
		const bool subscribed = SceneWriter::SubscribeCall(SWF_HOOK_TREE_LOD_REFILL, &OnRefill) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_INSTANCE_ADD_GROUP, &OnAddGroup, SWF_PHASE_POST) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_INSTANCE_REMOVE_GROUP, &OnRemoveGroup, SWF_PHASE_POST);
		if (!subscribed)
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused the tree LOD calls after reporting them installed");
		stl::write_thunk_call<BlockUpdate>(at(kBlockUpdateSite));
		installed = true;
	}
}
