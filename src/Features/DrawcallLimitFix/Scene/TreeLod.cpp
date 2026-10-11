#include "TreeLod.h"

#include "Features/DrawcallLimitFix/Common/KeptState.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Draws/GpuResources.h"
#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"

#include <cstring>

namespace DCLF::TreeLod
{
	namespace
	{
		constexpr std::uintptr_t kRefill = 0x504a40;            // FUN_140504a40(block, group)
		constexpr std::uintptr_t kRefillSites[] = { 0x5037f6,   // the block's attach (FUN_140503630)
			0x503fbf };                                         // its visibility update (FUN_140503f70)
		constexpr std::uintptr_t kBlockUpdate = 0x503f70;       // FUN_140503f70(block): refills the groups not up to date
		constexpr std::uintptr_t kBlockUpdateSite = 0x5108ff;   // its one call, in the node update (FUN_140510730)
		constexpr std::uintptr_t kAddGroup = 0xe1d7d0;          // BSMultiStreamInstanceTriShape::AddGroup, vfunc 0x1E0
		constexpr std::uintptr_t kRemoveGroup = 0xe1e2e0;       // RemoveGroup, vfunc 0x1E8
		constexpr std::size_t kAddGroupSlot = 0x1E0 / 8, kRemoveGroupSlot = 0x1E8 / 8;
		constexpr std::uintptr_t kLodTreesRoot = 0x315b880;     // NiNode*: the shapes' parent
		constexpr std::uintptr_t kHalfToFloat = 0xe19410, kFloatToHalf = 0xe19360;
		constexpr std::uintptr_t kCos = 0x153d032, kSin = 0x153d038;  // the CRT's, as the refill calls them
		constexpr std::uintptr_t kOne = 0x1769578;              // the float the refill packs into [7]
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

		// The render thread's id, from its first drain; the hooks count the events pushed from another thread.
		std::atomic<std::uint32_t> drainThread{ 0 };
		std::atomic<std::uint64_t> offThread{ 0 };

		void CountThread()
		{
			if (const auto drain = drainThread.load(std::memory_order_relaxed); drain && drain != GetCurrentThreadId())
				offThread.fetch_add(1, std::memory_order_relaxed);
		}


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
				if (refilling.group && a_shorts == kInstanceShorts) {
					Event event{ a_shape, refilling.block, refilling.group, std::vector<Instance>(a_count), Event::Kind::Add };
					if (a_count)
						std::memcpy(event.instances.data(), a_data, a_count * sizeof(Instance));
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
					CountThread();
					events.Push(std::move(event));
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
				if (IsTreeLodShape(a_shape)) {
					CountThread();
					events.Push(Event{ a_shape, nullptr, nullptr, {}, Event::Kind::Remove });
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The group's records by the refill's rule (FUN_140504a40), with the engine's own conversions. */
		void Pack(bool a_allVisible, const RE::BGSDistantTreeBlock::TreeGroup& a_group, std::vector<Instance>& a_out)
		{
			using HalfToFloat = float (*)(std::uint16_t);
			using FloatToHalf = std::uint16_t (*)(float);
			using Trig = float (*)(float);
			static const auto toFloat = reinterpret_cast<HalfToFloat>(REL::Offset(kHalfToFloat).address());
			static const auto toHalf = reinterpret_cast<FloatToHalf>(REL::Offset(kFloatToHalf).address());
			static const auto cosine = reinterpret_cast<Trig>(REL::Offset(kCos).address());
			static const auto sine = reinterpret_cast<Trig>(REL::Offset(kSin).address());
			const std::uint32_t count = std::min<std::uint32_t>(a_group.instances.size(), kMaxGroupInstances);
			const std::uint16_t one = toHalf(Engine::Global<float>(kOne));
			a_out.assign(count, {});
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& data = a_group.instances[i];
				// The hidden byte's bit 0 (dclf-lod.md, "Tree instance hiding"); the block's allVisible ignores it.
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
					const auto count = std::min<std::uint32_t>(group->instances.size(), kMaxGroupInstances);
					bool hidden = false;
					for (std::uint32_t i = 0; i < count && !hidden; ++i)
						hidden = (reinterpret_cast<const std::uint8_t&>(group->instances[i].hidden) & 1) != 0;
					if (!hidden)
						continue;
					Event event{ group->geometry.get(), a_block, group, {}, Event::Kind::Add, true };
					Pack(a_block->allVisible, *group, event.instances);
					CountThread();
					events.Push(std::move(event));
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool IsTreeLodShape(const RE::BSGeometry* a_shape)
	{
		const auto* property = a_shape ? a_shape->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		return property && *reinterpret_cast<const std::uintptr_t*>(property) == static_cast<std::uintptr_t>(RE::VTABLE_BSDistantTreeShaderProperty[0].address());
	}

	bool Install()
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
			return false;
		}
		for (const auto site : kRefillSites)
			stl::write_thunk_call<Refill>(at(site));
		stl::write_thunk_call<BlockUpdate>(at(kBlockUpdateSite));
		stl::write_vfunc<kAddGroupSlot, AddGroup>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
		stl::write_vfunc<kRemoveGroupSlot, RemoveGroup>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
		installed = true;
		return true;
	}

	void Mirror::Drain(std::uint32_t a_frame)
	{
		drainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		const bool judging = !suspects.empty();
		events.Drain([&](Event&& a_event) {
			if (judging && suspects.erase(a_event.shape))
				++stats.late;
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
		// Suspects with no event since the check: the mirror missed a refill.
		for (auto& [shape, text] : suspects) {
			++stats.differ;
			if (stats.first.empty())
				stats.first = std::move(text);
		}
		suspects.clear();
		if (SwitchEnabled(Switch::PersistentParity) && ParityDue(a_frame) && std::exchange(parityFrame, a_frame) != a_frame)
			CheckParity();
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

	const ShapeRow& Mirror::SlotRow(std::uint32_t a_slot) const
	{
		static const ShapeRow kFree{};
		const auto* shape = a_slot < slotGroups.size() ? slotGroups[a_slot] : nullptr;
		const auto it = shape ? groups.find(shape) : groups.end();
		return it != groups.end() ? it->second.row : kFree;
	}

	const std::vector<Instance>* Mirror::SlotInstances(std::uint32_t a_slot) const
	{
		const auto* shape = a_slot < slotGroups.size() ? slotGroups[a_slot] : nullptr;
		const auto it = shape ? groups.find(shape) : groups.end();
		return it != groups.end() ? &it->second.instances : nullptr;
	}

	const MeshRow& Mirror::MeshSlotRow(std::uint32_t a_slot) const
	{
		static const MeshRow kNone{};
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

	void Mirror::CheckParity()
	{
		++stats.checks;
		auto* root = Engine::Global<RE::NiNode*>(kLodTreesRoot);
		ankerl::unordered_dense::set<const void*> live;
		std::vector<Instance> packed;
		auto note = [&](std::string&& a_text) {
			if (stats.first.empty())
				stats.first = std::move(a_text);
		};
		if (root) {
			for (const auto& child : root->GetChildren()) {
				auto* shape = child ? child->AsGeometry() : nullptr;
				if (!IsTreeLodShape(shape))
					continue;
				live.insert(shape);
				++stats.shapes;
				const auto it = groups.find(shape);
				if (it == groups.end()) {
					++stats.missing;
					note(fmt::format("'{}' under the root, not mirrored", shape->name.c_str() ? shape->name.c_str() : ""));
					continue;
				}
				const auto& block = *static_cast<const RE::BGSDistantTreeBlock*>(it->second.block);
				const auto& group = *static_cast<const RE::BGSDistantTreeBlock::TreeGroup*>(it->second.group);
				if (!group.shaderPropertyUpToDate) {
					++stats.unsettled;
					continue;
				}
				Pack(block.allVisible, group, packed);
				if (packed == it->second.instances)
					continue;
				std::size_t at = 0;
				while (at < packed.size() && at < it->second.instances.size() && packed[at] == it->second.instances[at])
					++at;
				std::string fields;
				if (at < packed.size() && at < it->second.instances.size())
					for (std::size_t f = 0; f < kInstanceShorts; ++f)
						if (packed[at].shorts[f] != it->second.instances[at].shorts[f])
							fields += fmt::format(" [{}] {:#06x} mirrored, {:#06x} packed", f, it->second.instances[at].shorts[f], packed[at].shorts[f]);
				suspects[shape] = fmt::format("{} records mirrored, {} packed now; record {}:{}", it->second.instances.size(), packed.size(), at, fields);
			}
		}
		// A mirrored shape no longer under the root: its removal was missed.
		for (const auto& [shape, group] : groups)
			if (!live.contains(shape)) {
				++stats.stale;
				note(fmt::format("a mirrored shape ({} records) is not under the root", group.instances.size()));
			}
	}

	std::string Mirror::Report()
	{
		const auto& s = stats;
		auto text = fmt::format("[DCLF] tree LOD mirror: {} shapes, {} instances{}; {} adds, {} removes ({} off the render thread); parity {} checks, {} shapes compared, {} not settled, {} missing, {} stale, {} refilled after the drain, {} differ{}{}; {} groups drawn by the rule where the engine keeps hidden records\n",
			groups.size(), InstanceCount(), installed ? "" : ", events not installed", s.adds, s.removes, offThread.exchange(0, std::memory_order_relaxed), s.checks, s.shapes, s.unsettled,
			s.missing, s.stale, s.late, s.differ, s.checks ? (s.missing || s.stale || s.differ ? " <- TREE LOD MIRROR; first: " : " <- OK") : "", s.first, s.intended);
		stats = {};
		return text;
	}
}
