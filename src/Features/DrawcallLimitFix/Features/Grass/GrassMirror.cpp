// Grass: the sink's channel's render-thread end - the Mirror (Drain in seq order, shape slots, the instance table's ranges, buckets
// and their visible bases, mesh leases) the depth commit uploads from.

#include "Features/DrawcallLimitFix/Features/Grass/GrassMirror.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassEvents.h"

#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include "GrassShared/GrassFrame.h"
#include "GrassShared/GrassMeshLibrary.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace DCLF::Features::Grass
{
	namespace
	{
		using GrassInstances::BucketRow;
		using GrassInstances::kNoMesh;
		using GrassInstances::kTiers;

		std::uint64_t Mix(std::uint64_t a_hash, std::uint64_t a_value)
		{
			return ankerl::unordered_dense::hash<std::uint64_t>{}((a_hash * 0x9E3779B97F4A7C15ull) ^ a_value);
		}

		// A bucket's row's fixed part, from its key and material (the mesh slots, visible bases, count and bindings are the commit's).
		void FillBucketRow(BucketRow& a_row, const BucketKey& a_key, const BucketSource& a_source)
		{
			a_row.flags = (a_key.pbr ? GrassInstances::kBucketTruePbr : 0u) | (a_key.sphereNormal ? GrassInstances::kBucketSphereNormal : 0u);
			a_row.alphaRef = a_key.alphaRef;
			std::copy_n(a_source.boundCenter, 3, a_row.boundCenter);
			a_row.modelRadius = a_source.modelRadius;
			a_row.wavePeriod = a_source.wavePeriod;
			// Grass Optimizations' mesh cost (GrassBucketStore::CacheBucketTypeParams): a mesh of more triangles is drawn less far and
			// larger, as the cost bias ramps in.
			const float weight = std::sqrt(std::max(1.0f, static_cast<float>(a_source.triangleCount) / 8.0f));
			a_row.distScale = 1.0f / weight;
			a_row.minPixelScale = weight;
			a_row.pbrFlags = a_source.pbrFlags;
			// The vertex stage's ScaleMask (BSGrassShader::SetupGeometry): every axis with kUniformScale, else the height alone.
			const float uniform = a_key.uniformScale ? 1.0f : 0.0f;
			a_row.scaleMask[0] = uniform;
			a_row.scaleMask[1] = uniform;
			a_row.scaleMask[2] = 1.0f;
			a_row.pbrParams1[0] = a_source.roughnessScale;
			a_row.pbrParams1[1] = a_source.specularLevel;
			std::copy_n(a_source.subsurface, 4, a_row.pbrParams2);
		}
	}

	std::uint64_t BucketKeyHash::operator()(const BucketKey& a_key) const noexcept
	{
		std::uint64_t hash = reinterpret_cast<std::uintptr_t>(a_key.mesh);
		hash = Mix(hash, reinterpret_cast<std::uintptr_t>(a_key.material));
		hash = Mix(hash, reinterpret_cast<std::uintptr_t>(a_key.diffuse));
		hash = Mix(hash, a_key.vertexDesc);
		const std::uint64_t bits = (std::uint64_t(a_key.clampMode) << 3) | (a_key.pbr ? 1u : 0u) | (a_key.sphereNormal ? 2u : 0u) | (a_key.uniformScale ? 4u : 0u);
		return Mix(hash, bits | (std::uint64_t(std::bit_cast<std::uint32_t>(a_key.alphaRef)) << 32));
	}

	Mirror& RenderMirror()
	{
		static Mirror mirror;
		return mirror;
	}

	void Mirror::Drain(std::uint32_t a_frame)
	{
		++drains;
		// The spawn fade's clock: the engine's frame time, summed every frame (Grass Optimizations' timeAccum).
		if (const auto* state = globals::game::smState)
			fadeClock += state->timerValues[1];
		events.Drain([&](Event&& a_event) {
			Diagnostics::Notify(GrassEventDrained{ a_event.shape });
			if (Stale(a_event.shape, a_event.seq)) {
				++stats.stale;
				return;
			}
			Apply(std::move(a_event));
		});
		PruneTombstones();
		Diagnostics::Notify(GrassDrained{ *this, a_frame });
	}

	bool Mirror::Stale(const void* a_shape, std::uint64_t a_seq)
	{
		if (const auto it = shapes.find(a_shape); it != shapes.end() && a_seq < it->second.seq)
			return true;
		const auto tomb = tombstones.find(a_shape);
		return tomb != tombstones.end() && a_seq < tomb->second.first;
	}

	void Mirror::Apply(Event&& a_event)
	{
		if (a_event.kind != Event::Kind::Add) {
			// A removal or a rejection: the mirror's records of the shape (if any) are not what the engine draws any more. A rejected shape
			// keeps the family the engine's while it lives (RejectedLive).
			if (a_event.kind == Event::Kind::Rejected) {
				++stats.rejected;
				rejected.insert(a_event.shape);
			} else if (rejected.erase(a_event.shape) || shapes.contains(a_event.shape)) {
				++stats.removes;
			}
			Remove(a_event.shape);
			tombstones[a_event.shape] = { a_event.seq, drains };
			return;
		}
		++stats.adds;
		rejected.erase(a_event.shape);
		tombstones.erase(a_event.shape);
		// Each add is one more group of the shape (its own slot and range); its removal takes them all.
		auto& shape = shapes[a_event.shape];
		const std::uint32_t slot = AcquireSlot();
		slotShapes[slot] = a_event.shape;
		auto& group = groups[slot];
		const auto count = static_cast<std::uint32_t>(a_event.instances.size());
		shape.seq = a_event.seq;
		shape.slots.push_back(slot);
		shape.records += count;
		group.first = AllocateRange(count);
		group.bucket = AddBucketUser(a_event).slot;
		group.instances = std::move(a_event.instances);
		instanceCount += count;
		auto& row = group.row;
		std::copy_n(a_event.origin.data(), 3, row.origin);
		row.bucket = group.bucket;
		row.firstInstance = group.first;
		row.count = count;
		row.spawnStart = fadeClock;
		row.flags = 0;
		MarkSlot(slot);
	}

	void Mirror::Remove(const void* a_shape)
	{
		const auto it = shapes.find(a_shape);
		if (it == shapes.end())
			return;
		for (const std::uint32_t slot : it->second.slots) {
			ReleaseGroup(groups[slot]);
			slotShapes[slot] = nullptr;
			freeSlots.push_back(slot);
			MarkSlot(slot);  // its row's count 0
		}
		shapes.erase(it);
	}

	void Mirror::ReleaseGroup(Group& a_group)
	{
		const auto count = static_cast<std::uint32_t>(a_group.instances.size());
		FreeRange(a_group.first, count);
		RemoveBucketUser(a_group.bucket, count);
		instanceCount -= count;
		a_group = Group{};
	}

	std::uint32_t Mirror::AcquireSlot()
	{
		if (!freeSlots.empty()) {
			const std::uint32_t slot = freeSlots.back();
			freeSlots.pop_back();
			return slot;
		}
		slotShapes.push_back(nullptr);
		groups.emplace_back();
		slotChanged.push_back(0);
		return static_cast<std::uint32_t>(slotShapes.size() - 1);
	}

	std::uint32_t Mirror::AllocateRange(std::uint32_t a_count)
	{
		if (!a_count)
			return 0;
		// First fit over the free ranges, else past the high-water mark.
		for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it) {
			if (it->second < a_count)
				continue;
			const auto [first, size] = *it;
			freeRanges.erase(it);
			if (size > a_count)
				freeRanges.emplace(first + a_count, size - a_count);
			return first;
		}
		const std::uint32_t first = instanceSlots;
		instanceSlots += a_count;
		return first;
	}

	void Mirror::FreeRange(std::uint32_t a_first, std::uint32_t a_count)
	{
		if (!a_count)
			return;
		// Its owners go ~0u (the cull skips it) at the next commit.
		freedRanges.emplace_back(a_first, a_count);
		std::uint32_t first = a_first, end = a_first + a_count;
		if (auto next = freeRanges.find(end); next != freeRanges.end()) {
			end += next->second;
			freeRanges.erase(next);
		}
		if (auto previous = freeRanges.lower_bound(first); previous != freeRanges.begin()) {
			--previous;
			if (previous->first + previous->second == first) {
				first = previous->first;
				freeRanges.erase(previous);
			}
		}
		// A range that reaches the high-water mark lowers it.
		if (end == instanceSlots)
			instanceSlots = first;
		else
			freeRanges.emplace(first, end - first);
	}

	void Mirror::MarkSlot(std::uint32_t a_slot)
	{
		if (!std::exchange(slotChanged[a_slot], std::uint8_t(1)))
			changedSlots.push_back(a_slot);
	}

	void Mirror::MarkBucket(std::uint32_t a_slot)
	{
		if (a_slot < bucketChanged.size() && !std::exchange(bucketChanged[a_slot], std::uint8_t(1)))
			changedBuckets.push_back(a_slot);
	}

	Bucket& Mirror::AddBucketUser(Event& a_event)
	{
		std::uint32_t slot;
		if (const auto it = bucketIndex.find(a_event.key); it != bucketIndex.end()) {
			slot = it->second;
		} else {
			if (!freeBuckets.empty()) {
				slot = freeBuckets.back();
				freeBuckets.pop_back();
			} else {
				slot = static_cast<std::uint32_t>(buckets.size());
				buckets.emplace_back();
				bucketChanged.push_back(0);
			}
			bucketIndex.emplace(a_event.key, slot);
			auto& bucket = buckets[slot];
			bucket = Bucket{ .key = a_event.key, .source = std::move(a_event.source), .slot = slot, .live = true };
			FillBucketRow(bucket.row, bucket.key, bucket.source);
			ResolveTiers(bucket, a_event.shape, std::move(a_event.mesh));
		}
		auto& bucket = buckets[slot];
		++bucket.shapes;
		bucket.total += static_cast<std::uint32_t>(a_event.instances.size());
		totalsChanged = true;
		MarkBucket(slot);
		return bucket;
	}

	void Mirror::RemoveBucketUser(std::uint32_t a_slot, std::uint32_t a_instances)
	{
		auto& bucket = buckets[a_slot];
		bucket.total -= a_instances;
		totalsChanged = true;
		MarkBucket(a_slot);
		if (--bucket.shapes)
			return;
		// Its meshes' users and its bindings go (the bindings to the next execution, as the meshes' leases); its slot's row is zero.
		for (const void* mesh : bucket.meshes)
			RemoveMeshUser(mesh);
		for (auto& owner : bucket.owners)
			retired.push_back(std::move(owner));
		bucketIndex.erase(bucket.key);
		bucket = Bucket{ .slot = a_slot };
		freeBuckets.push_back(a_slot);
	}

	void Mirror::ResolveTiers(Bucket& a_bucket, const void* a_shape, MeshSource&& a_mesh)
	{
		a_bucket.meshes[0] = AddMeshUser(std::move(a_mesh));
		// Grass Optimizations' LOD meshes (meshes\LOD\Grass\<stem>_LOD0.nif and _LOD1.nif), by the shape's source mesh: the shape is a
		// key only (it may be dead, then no LOD). The far tier falls back to the middle mesh (GetLODMesh).
		if (!GrassShared::Settings().EnableMeshLOD)
			return;
		auto& library = GrassShared::MeshLibrary();
		const auto meshId = library.ResolveMeshId(static_cast<RE::BSMultiStreamInstanceTriShape*>(const_cast<void*>(a_shape)));
		if (!meshId)
			return;
		library.EnsureLODMeshes(meshId);
		using Tier = GrassShared::GrassMeshLibrary::LODTier;
		for (std::uint32_t tier = 1; tier < kTiers; ++tier) {
			const auto* lod = library.GetLODMesh(meshId, tier == 1 ? Tier::kMiddle : Tier::kFar);
			if (!lod)
				continue;
			MeshSource source{ .key = lod, .vertexDesc = lod->descVal, .indexCount = lod->indexCount };
			source.vertexBuffer.copy_from(lod->vertexBuffer);
			source.indexBuffer.copy_from(lod->indexBuffer);
			a_bucket.meshes[tier] = AddMeshUser(std::move(source));
		}
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
		// Its leases outlive the frames in flight (TakeRetired); no bucket names its slot until it is reused.
		for (auto* owner : { &it->second.vertexOwner, &it->second.indexOwner })
			if (*owner)
				retired.push_back(std::move(*owner));
		meshSlots[it->second.slot] = nullptr;
		freeMeshSlots.push_back(it->second.slot);
		meshes.erase(it);
	}

	void Mirror::LayOutVisible()
	{
		// Every live bucket's tiers with a resolved mesh, in slot order: each the bucket's instance count long.
		std::uint32_t base = 0;
		for (auto& bucket : buckets) {
			if (!bucket.live)
				continue;
			auto& row = bucket.row;
			row.flags &= ~(GrassInstances::kBucketTierOne | GrassInstances::kBucketTierTwo);
			for (std::uint32_t tier = 0; tier < kTiers; ++tier) {
				const auto it = bucket.meshes[tier] ? meshes.find(bucket.meshes[tier]) : meshes.end();
				const bool drawn = it != meshes.end() && it->second.resolved;
				row.mesh[tier] = drawn ? it->second.slot : kNoMesh;
				row.visibleBase[tier] = drawn ? base : 0;
				if (drawn) {
					base += bucket.total;
					row.flags |= tier == 1 ? GrassInstances::kBucketTierOne : tier == 2 ? GrassInstances::kBucketTierTwo : 0u;
				}
			}
			row.instanceCount = row.mesh[0] != kNoMesh ? bucket.total : 0;
			MarkBucket(bucket.slot);
		}
	}

	void Mirror::PruneTombstones()
	{
		if (drains % kTombstoneDrains)
			return;
		for (auto it = tombstones.begin(); it != tombstones.end();) {
			if (drains - it->second.second >= kTombstoneDrains)
				it = tombstones.erase(it);
			else
				++it;
		}
	}

	void Mirror::MarkAllChanged()
	{
		for (std::uint32_t slot = 0; slot < slotShapes.size(); ++slot)
			MarkSlot(slot);
		for (std::uint32_t slot = 0; slot < buckets.size(); ++slot)
			MarkBucket(slot);
		changedMeshes.clear();
		for (std::uint32_t slot = 0; slot < meshSlots.size(); ++slot)
			changedMeshes.push_back(slot);
		// A new backing of the owners holds nothing: the free ranges' owners go ~0u again.
		for (const auto& [first, count] : freeRanges)
			freedRanges.emplace_back(first, count);
	}

	void Mirror::TakeChanges(Changes& a_changes)
	{
		// The render thread's registry (GpuResources::Frame): the scene work leases the geometry slots' buffers from its own, on its thread.
		auto& gpu = GpuResources::Frame();
		gpu.BeginFrame();
		// The new meshes' buffers, leased (they stay pinned while the mesh lives); a mesh that cannot be leased is drawn by no tier.
		std::vector<std::uint32_t> pending;
		bool settled = false;
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
					mesh.row = { .vertexAddress = vertex->buffer.address, .indexAddress = index->buffer.address, .vertexDesc = mesh.source.vertexDesc,
						.indexCount = mesh.source.indexCount, .vertexStride = static_cast<std::uint32_t>(mesh.source.vertexDesc & 0xF) * 4 };
					mesh.vertexOwner = std::move(vertex->owner);
					mesh.indexOwner = std::move(index->owner);
					mesh.resolved = true;
				} else {
					mesh.failed = true;
				}
				mesh.source.vertexBuffer = nullptr;
				mesh.source.indexBuffer = nullptr;
				settled = true;
			}
			a_changes.meshes.push_back(slot);
		}
		changedMeshes = std::move(pending);
		// A mesh settled or a bucket's total moved: the tiers' visible bases again, every bucket's row with them.
		if (settled || totalsChanged) {
			LayOutVisible();
			totalsChanged = false;
		}
		for (const std::uint32_t slot : changedSlots) {
			slotChanged[slot] = 0;
			a_changes.shapes.push_back(slot);
		}
		changedSlots.clear();
		for (const std::uint32_t slot : changedBuckets) {
			bucketChanged[slot] = 0;
			a_changes.buckets.push_back(slot);
		}
		changedBuckets.clear();
		a_changes.freedRanges = std::exchange(freedRanges, {});
	}

	const GrassInstances::ShapeRow& Mirror::SlotRow(std::uint32_t a_slot) const
	{
		static const GrassInstances::ShapeRow kFree{};
		return a_slot < slotShapes.size() && slotShapes[a_slot] ? groups[a_slot].row : kFree;
	}

	const std::vector<GrassInstances::Instance>* Mirror::SlotInstances(std::uint32_t a_slot) const
	{
		return a_slot < slotShapes.size() && slotShapes[a_slot] ? &groups[a_slot].instances : nullptr;
	}

	const GrassInstances::MeshRow& Mirror::MeshSlotRow(std::uint32_t a_slot) const
	{
		static const GrassInstances::MeshRow kNone{};
		const auto* key = a_slot < meshSlots.size() ? meshSlots[a_slot] : nullptr;
		const auto it = key ? meshes.find(key) : meshes.end();
		return it != meshes.end() ? it->second.row : kNone;
	}

	Bucket* Mirror::SlotBucket(std::uint32_t a_slot)
	{
		return a_slot < buckets.size() && buckets[a_slot].live ? &buckets[a_slot] : nullptr;
	}

	Mirror::Readiness Mirror::ReadinessOf() const
	{
		Readiness readiness;
		for (const auto& [key, mesh] : meshes)
			++(mesh.resolved ? readiness.meshesResolved : mesh.failed ? readiness.meshesFailed : readiness.meshesPending);
		for (const auto& bucket : buckets)
			readiness.bucketsDrawable += bucket.live && bucket.row.instanceCount ? 1u : 0u;
		return readiness;
	}
}
