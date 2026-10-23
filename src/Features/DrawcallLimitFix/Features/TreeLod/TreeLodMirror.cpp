// Tree LOD: the hooks' push (any thread; events onto events, from the feature's hooks on the refill, block update, AddGroup and
// RemoveGroup), the refill's packing rule, and the render thread's Mirror (Drain, slots, mesh leases) the depth commit uploads from.

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodMirror.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/Diagnostics/TreeLodEvents.h"

#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstring>
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Features::TreeLod
{
	namespace
	{
		constexpr Engine::Offset kLodTreesRoot{ 0x315b880, 0x2f23e90 };  // NiNode*: the shapes' parent
		// SE's two are jmp thunks to the conversions, as its refill calls them.
		constexpr Engine::Offset kHalfToFloat{ 0xe19410, 0xd42700 }, kFloatToHalf{ 0xe19360, 0xd426f0 };
		constexpr Engine::Offset kCos{ 0x153d032, 0x134beca }, kSin{ 0x153d038, 0x134bed0 };  // the CRT's, as the refill calls them

		// The render thread's id, from its first drain; the hooks count the events pushed from another thread.
		std::atomic<std::uint32_t> drainThread{ 0 };
		std::atomic<std::uint64_t> offThread{ 0 };

		void CountThread()
		{
			if (const auto drain = drainThread.load(std::memory_order_relaxed); drain && drain != GetCurrentThreadId())
				offThread.fetch_add(1, std::memory_order_relaxed);
		}

		/** @brief The group's records by the refill's rule (FUN_140504a40), with the engine's own conversions. */
		void Pack(bool a_allVisible, const RE::BGSDistantTreeBlock::TreeGroup& a_group, std::vector<TreeInstances::Instance>& a_out)
		{
			using HalfToFloat = float (*)(std::uint16_t);
			using FloatToHalf = std::uint16_t (*)(float);
			using Trig = float (*)(float);
			static const auto toFloat = reinterpret_cast<HalfToFloat>(REL::Offset(kHalfToFloat).address());
			static const auto toHalf = reinterpret_cast<FloatToHalf>(REL::Offset(kFloatToHalf).address());
			static const auto cosine = reinterpret_cast<Trig>(REL::Offset(kCos).address());
			static const auto sine = reinterpret_cast<Trig>(REL::Offset(kSin).address());
			const std::uint32_t count = std::min<std::uint32_t>(a_group.instances.size(), TreeInstances::kMaxGroupInstances);
			const std::uint16_t one = toHalf(Engine::Global<float>(Engine::Address::kFadeSpecialB));
			a_out.assign(count, {});
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& data = a_group.instances[i];
				// The hidden byte's bit 0 (dclf-architecture.md, "Tree instance hiding"); the block's allVisible ignores it.
				const bool hidden = !a_allVisible && (reinterpret_cast<const std::uint8_t&>(data.hidden) & 1);
				auto& s = a_out[i].shorts;
				s[0] = data.x;
				s[1] = data.y;
				s[2] = data.z;
				s[3] = hidden ? 0 : data.scale;
				s[4] = toHalf(cosine(toFloat(data.rotZ)));
				s[5] = toHalf(sine(toFloat(data.rotZ)));
				s[6] = data.alpha;
				s[7] = one;
			}
		}
	}

	void Push(Event&& a_event)
	{
		CountThread();
		events.Push(std::move(a_event));
	}

	std::uint64_t TakeOffThread()
	{
		return offThread.exchange(0, std::memory_order_relaxed);
	}

	void PackGroup(bool a_allVisible, const RE::BGSDistantTreeBlock::TreeGroup& a_group, std::vector<TreeInstances::Instance>& a_out)
	{
		Pack(a_allVisible, a_group, a_out);
	}

	RE::NiNode* LodTreesRoot()
	{
		return Engine::Global<RE::NiNode*>(kLodTreesRoot);
	}

	Mirror& RenderMirror()
	{
		static Mirror mirror;
		return mirror;
	}

	void Mirror::Drain(std::uint32_t a_frame)
	{
		drainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		events.Drain([&](Event&& a_event) {
			Diagnostics::Notify(TreeLodEventDrained{ a_event.shape });
			if (a_event.kind == Event::Kind::Remove) {
				++stats.removes;
				if (const auto it = groups.find(a_event.shape); it != groups.end()) {
					ReleaseSlot(it->second.slot);
					RemoveMeshUser(it->second.mesh);
					groups.erase(it);
				}
				return;
			}
			if (a_event.intended) {
				// The rule's records after a block update: applied where they differ from the engine's (the shape is mirrored: its
				// attach's refill was pushed before).
				const auto it = groups.find(a_event.shape);
				if (it == groups.end() || it->second.instances == a_event.instances)
					return;
				++stats.intended;
				it->second.instances = std::move(a_event.instances);
				it->second.row.count = static_cast<std::uint32_t>(it->second.instances.size());
				MarkSlot(it->second.slot);
				return;
			}
			++stats.adds;
			auto [it, inserted] = groups.try_emplace(a_event.shape);
			auto& group = it->second;
			if (inserted) {
				group.slot = AcquireSlot();
				slotGroups[group.slot] = a_event.shape;
			}
			group.block = a_event.block;
			group.group = a_event.group;
			group.instances = std::move(a_event.instances);
			if (a_event.mesh.key != group.mesh) {
				const void* previous = group.mesh;
				group.mesh = AddMeshUser(std::move(a_event.mesh));
				RemoveMeshUser(previous);
			}
			auto& row = group.row;
			std::copy_n(a_event.translate.data(), 3, row.translate);
			row.extent = a_event.extent;
			row.count = static_cast<std::uint32_t>(group.instances.size());
			const auto mesh = meshes.find(group.mesh);
			row.mesh = mesh != meshes.end() ? mesh->second.slot : ~0u;
			MarkSlot(group.slot);
		});
		Diagnostics::Notify(TreeLodDrained{ *this, a_frame });
	}

	std::uint32_t Mirror::AcquireSlot()
	{
		if (!freeSlots.empty()) {
			const std::uint32_t slot = freeSlots.back();
			freeSlots.pop_back();
			return slot;
		}
		slotGroups.push_back(nullptr);
		slotChanged.push_back(0);
		return static_cast<std::uint32_t>(slotGroups.size() - 1);
	}

	void Mirror::ReleaseSlot(std::uint32_t a_slot)
	{
		slotGroups[a_slot] = nullptr;
		freeSlots.push_back(a_slot);
		MarkSlot(a_slot);  // its row's count 0: the cull skips it
	}

	void Mirror::MarkSlot(std::uint32_t a_slot)
	{
		if (!std::exchange(slotChanged[a_slot], std::uint8_t(1)))
			changedSlots.push_back(a_slot);
	}

	const void* Mirror::AddMeshUser(MeshSource&& a_source)
	{
		if (!a_source.key)
			return nullptr;
		auto [it, inserted] = meshes.try_emplace(a_source.key);
		auto& mesh = it->second;
		if (inserted) {
			if (!freeMeshSlots.empty()) {
				mesh.slot = freeMeshSlots.back();
				freeMeshSlots.pop_back();
				meshSlots[mesh.slot] = a_source.key;
			} else {
				mesh.slot = static_cast<std::uint32_t>(meshSlots.size());
				meshSlots.push_back(a_source.key);
			}
			mesh.source = std::move(a_source);
			changedMeshes.push_back(mesh.slot);
		}
		++mesh.users;
		return it->first;
	}

	void Mirror::RemoveMeshUser(const void* a_key)
	{
		const auto it = a_key ? meshes.find(a_key) : meshes.end();
		if (it == meshes.end() || --it->second.users)
			return;
		// Its leases outlive the frames in flight (TakeRetired), and its slot's row is not drawn by any shape until reused.
		for (auto* owner : { &it->second.vertexOwner, &it->second.indexOwner })
			if (*owner)
				retired.push_back(std::move(*owner));
		meshSlots[it->second.slot] = nullptr;
		freeMeshSlots.push_back(it->second.slot);
		meshes.erase(it);
	}

	void Mirror::MarkAllChanged()
	{
		for (std::uint32_t slot = 0; slot < slotGroups.size(); ++slot)
			MarkSlot(slot);
		changedMeshes.clear();
		for (std::uint32_t slot = 0; slot < meshSlots.size(); ++slot)
			changedMeshes.push_back(slot);
	}

	void Mirror::TakeChanges(std::vector<std::uint32_t>& a_slots, std::vector<std::uint32_t>& a_meshes)
	{
		// The render thread's registry (GpuResources::Frame): the scene work leases the geometry slots' buffers from its own, on its thread.
		auto& gpu = GpuResources::Frame();
		gpu.BeginFrame();
		// The new meshes' buffers, leased (they stay pinned while the mesh lives); a mesh that cannot be leased has no indices.
		std::vector<std::uint32_t> pending;
		for (const std::uint32_t slot : changedMeshes) {
			const auto* key = slot < meshSlots.size() ? meshSlots[slot] : nullptr;
			const auto it = key ? meshes.find(key) : meshes.end();
			if (it == meshes.end())
				continue;
			auto& mesh = it->second;
			if (!mesh.resolved && !mesh.failed) {
				if (!gpu.Enabled()) {
					pending.push_back(slot);
					continue;
				}
				auto vertex = gpu.Acquire(mesh.source.vertexBuffer.get());
				auto index = vertex ? gpu.Acquire(mesh.source.indexBuffer.get()) : std::nullopt;
				if (vertex && index) {
					const std::uint64_t desc = mesh.source.vertexDesc;
					mesh.row.vertexAddress = vertex->buffer.address;
					mesh.row.indexAddress = index->buffer.address;
					mesh.row.vertexStride = static_cast<std::uint32_t>(desc & 0xF) * 4;
					mesh.row.texcoordOffset = static_cast<std::uint32_t>((desc >> 8) & 0xF) * 4;
					mesh.row.indexCount = mesh.source.indexCount;
					mesh.vertexOwner = std::move(vertex->owner);
					mesh.indexOwner = std::move(index->owner);
					mesh.resolved = true;
					maxIndices = std::max(maxIndices, mesh.row.indexCount);
				} else {
					mesh.failed = true;
				}
				mesh.source.vertexBuffer = nullptr;
				mesh.source.indexBuffer = nullptr;
			}
			a_meshes.push_back(slot);
		}
		changedMeshes = std::move(pending);
		for (const std::uint32_t slot : changedSlots) {
			slotChanged[slot] = 0;
			a_slots.push_back(slot);
		}
		changedSlots.clear();
	}

	const TreeInstances::ShapeRow& Mirror::SlotRow(std::uint32_t a_slot) const
	{
		static const TreeInstances::ShapeRow kFree{};
		const auto* shape = a_slot < slotGroups.size() ? slotGroups[a_slot] : nullptr;
		const auto it = shape ? groups.find(shape) : groups.end();
		return it != groups.end() ? it->second.row : kFree;
	}

	const std::vector<TreeInstances::Instance>* Mirror::SlotInstances(std::uint32_t a_slot) const
	{
		const auto* shape = a_slot < slotGroups.size() ? slotGroups[a_slot] : nullptr;
		const auto it = shape ? groups.find(shape) : groups.end();
		return it != groups.end() ? &it->second.instances : nullptr;
	}

	const TreeInstances::MeshRow& Mirror::MeshSlotRow(std::uint32_t a_slot) const
	{
		static const TreeInstances::MeshRow kNone{};
		const auto* key = a_slot < meshSlots.size() ? meshSlots[a_slot] : nullptr;
		const auto it = key ? meshes.find(key) : meshes.end();
		return it != meshes.end() ? it->second.row : kNone;
	}

	std::size_t Mirror::InstanceCount() const
	{
		std::size_t count = 0;
		for (const auto& [shape, group] : groups)
			count += group.instances.size();
		return count;
	}
}
