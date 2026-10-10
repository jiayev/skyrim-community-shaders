#include "Internal.h"

namespace DCLF
{
	namespace
	{
		// A character-light pass's t11 is the frame's (MaterialSources::FrameCharacterLight): its record holds the modes, no view.
		constexpr std::uint32_t kCharacterLightTexture = 11;
		// GpuTextures' diagnostic source tags: a material's t0-t15 as themselves, its feature textures from 16.
		constexpr std::uint32_t kFeatureSourceTag = 16;
		// The lookups' material log's bound (VersionMaterialBindings): a publication's changes fit many times over.
		constexpr std::size_t kMaterialLogEntries = std::size_t(1) << 15;
	}

	void SceneStore::UpdateMaterialBindings()
	{
		ZoneScopedN("CS.DCLF.Scene.MaterialBindings");
		auto& mb = materialBindings;
		if (lookups.materials.size() < tables.materials.size())
			lookups.materials.resize(tables.materials.size());
		if (mb.held.size() < tables.materials.size())
			mb.held.resize(tables.materials.size(), 0);
		// The answers since the last pass: each view's waiting slots written from it (a rejection too: that texture unresolvable, as the
		// render thread's lookups had it). An answer nobody waits on any more lets its owner go here: the last release of an import, or
		// of its descriptor, runs on the cleanup queue (ResourceCleanupQueue::Make), never on this thread.
		std::vector<std::uint32_t> waiting;
		mb.replies.Drain([&](GpuTextures::Reply&& a_reply) {
			if (a_reply.cookie != mb.cookie) {
				++mb.stats.stale;
				return;
			}
			++mb.stats.answered;
			if (const auto it = mb.inFlight.find(a_reply.view); it != mb.inFlight.end()) {
				waiting.insert(waiting.end(), it->second.begin(), it->second.end());
				mb.inFlight.erase(it);
			}
			if (a_reply.binding.owner) {
				auto& cached = mb.cache[a_reply.view];
				cached.owner = a_reply.binding.owner;
				cached.index = a_reply.binding.index;
				// Amortized: each answer cached looks at two other entries and drops one whose import nobody holds any more.
				for (std::uint32_t i = 0; i < 2 && !mb.cache.empty(); ++i) {
					mb.cacheSweep = (mb.cacheSweep + 1) % mb.cache.size();
					if (const auto swept = mb.cache.begin() + static_cast<std::ptrdiff_t>(mb.cacheSweep); swept->second.owner.expired())
						mb.cache.erase(swept);
				}
			} else {
				++mb.stats.rejected;
			}
			mb.arrived[a_reply.view] = std::move(a_reply.binding);
		});
		for (const std::uint32_t slot : waiting)
			ResolveMaterialBinding(slot);
		mb.arrived.clear();
		// The records written since the last pass: the joins' new slots and the posts' records (the tables' material log; a cursor
		// behind it resolves every used slot).
		const auto& log = tables.materialLog;
		if (!mb.cursor.Continues(log, 0)) {
			Tables::ForEachBit(tables.usedMaterialBits, [&](std::uint32_t a_slot) { ResolveMaterialBinding(a_slot); });
			mb.cursor.Restart(0);
		} else {
			for (const std::uint32_t slot : mb.cursor.Unread(log))
				ResolveMaterialBinding(slot);
		}
		mb.cursor.Advance(log);
		// The slots used since the last pass (a member joined on a slot whose record is older, KeepResidentsAlive marking it again).
		const auto& used = tables.usedMaterialBits;
		mb.usedSeen.resize(used.size(), 0);
		for (std::size_t word = 0; word < used.size(); ++word) {
			for (std::uint64_t added = used[word] & ~mb.usedSeen[word]; added; added &= added - 1)
				ResolveMaterialBinding(static_cast<std::uint32_t>(word * 64 + std::countr_zero(added)));
			mb.usedSeen[word] = used[word];
		}
	}

	void SceneStore::ResolveMaterialBinding(std::uint32_t a_slot)
	{
		auto& mb = materialBindings;
		// Only a used slot: the used set certifies the views a record holds (Tables, "The three shared tables"), as the render thread's
		// refresh took only the used ones. A slot freed keeps its entry until its retirement comes back (RetireMaterialBinding).
		if (a_slot >= tables.materials.size() || a_slot >= tables.materialSlotKey.size() || a_slot >= tables.materialVersion.size() ||
			!tables.materialSlots.Alive(a_slot) || !tables.MaterialUsed(a_slot))
			return;
		const auto& key = tables.materialSlotKey[a_slot];
		if (!key.first)
			return;
		if (lookups.materials.size() < tables.materials.size())
			lookups.materials.resize(tables.materials.size());
		if (mb.held.size() < tables.materials.size())
			mb.held.resize(tables.materials.size(), 0);
		const auto& material = tables.materials[a_slot];
		const std::uint32_t textureGeneration = GpuTextures::Get().Generation();
		// An unchanged record keeps its bindings (no periodic restamp); a held view waits for its answer, which resolves the slot again.
		// Read before the entry is written: a write copies its chunk when a publication shares it (SharedChunks).
		if (const auto& current = lookups.materials[a_slot]; current.key == key && current.resolved && !mb.held[a_slot] &&
															 current.recordVersion == tables.materialVersion[a_slot] && current.written == material.textureWritten &&
															 current.texturesGeneration == textureGeneration)
			return;
		// The entry is the lookups' (T6b2c step 5), written straight in; its version and log entry come before the next commit or
		// publication (VersionMaterialBindings).
		auto& entry = lookups.materials.Mutable(a_slot);
		bool changed = false;
		if (entry.key != key) {
			entry = {};  // a predecessor incarnation contributes no binding
			entry.key = key;
			entry.textureIndex.fill(Lookups::kNone);
			entry.featureIndex.fill(Lookups::kNone);
			mb.held[a_slot] = 0;
			changed = true;
		}
		const bool sameGeneration = entry.texturesGeneration == textureGeneration;
		// Readiness is held (the DCLF set: a member joined because its material resolved, and the engine no longer draws it): a resolved
		// entry whose view changed keeps drawing with the previous view, whose import its owner still holds, until the new one is answered.
		const bool wasResolved = entry.resolved && sameGeneration;
		bool held = false, pending = false, ownersChanged = false;
		// One texture: kept while its view and binding stand; held while a resolved entry's new view is asked for; else what is known.
		auto bind = [&](ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t& a_index, std::shared_ptr<const void>& a_owner,
						ID3D11ShaderResourceView*& a_seen) {
			if (sameGeneration && a_seen == a_view && a_owner && a_index != Lookups::kNone)
				return;
			auto binding = MaterialViewBinding(a_view, a_sourceTag, a_slot);
			if (binding.pending && wasResolved && a_owner && a_index != Lookups::kNone) {
				held = true;
				return;
			}
			pending |= binding.pending;
			const bool ownerChanged = a_owner.get() != binding.owner.get();
			changed |= ownerChanged || a_index != binding.index;
			ownersChanged |= ownerChanged;
			a_index = binding.index;
			a_owner = std::move(binding.owner);
			a_seen = a_view;
		};
		auto clear = [&](std::uint32_t& a_index, std::shared_ptr<const void>& a_owner, ID3D11ShaderResourceView*& a_seen) {
			changed |= a_index != Lookups::kNone || a_owner;
			ownersChanged |= static_cast<bool>(a_owner);
			a_index = Lookups::kNone;
			a_owner.reset();
			a_seen = nullptr;
		};
		const std::uint32_t textureWritten = material.textureWritten & ~(MaterialSources::FrameCharacterLight(key.second) ? 1u << kCharacterLightTexture : 0u);
		for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
			if ((textureWritten >> t) & 1)
				bind(material.textures[t], t, entry.textureIndex[t], entry.textureOwners[t], entry.views[t]);  // a null view: the null descriptor
			else
				clear(entry.textureIndex[t], entry.textureOwners[t], entry.views[t]);
		}
		for (std::uint32_t f = 0; f < kFeatureMaterialTextures; ++f) {
			if (material.featureTextures[f])
				bind(material.featureTextures[f], kFeatureSourceTag + f, entry.featureIndex[f], entry.featureOwners[f], entry.featureViews[f]);
			else
				clear(entry.featureIndex[f], entry.featureOwners[f], entry.featureViews[f]);
		}
		// A texture still asked for leaves the slot unresolved: its draws defer (they stay the engine's) until the answer resolves it.
		if (entry.resolved == pending) {
			changed = true;
			if (!pending)
				++mb.stats.resolved;
		}
		entry.resolved = !pending;
		entry.written = material.textureWritten;  // the record's, so a character-light pass's is not resolved again every pass
		entry.texturesGeneration = textureGeneration;
		entry.recordVersion = tables.materialVersion[a_slot];
		mb.held[a_slot] = held ? 1 : 0;
		if (ownersChanged) {
			std::vector<std::shared_ptr<const void>> owners;
			owners.reserve(entry.textureOwners.size() + entry.featureOwners.size());
			owners.insert(owners.end(), entry.textureOwners.begin(), entry.textureOwners.end());
			owners.insert(owners.end(), entry.featureOwners.begin(), entry.featureOwners.end());
			entry.bindingBlock = GpuTextures::Get().Seal(std::move(owners));
		}
		if (changed)
			mb.MarkChanged(a_slot);
	}

	GpuTextures::Binding SceneStore::MaterialViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t a_slot)
	{
		auto& mb = materialBindings;
		if (const auto it = mb.arrived.find(a_view); it != mb.arrived.end())
			return it->second;
		if (const auto it = mb.cache.find(a_view); it != mb.cache.end()) {
			if (auto owner = it->second.owner.lock()) {
				++mb.stats.cached;
				return { it->second.index, std::move(owner) };
			}
			mb.cache.erase(it);
		}
		// Asked once per view in flight: the record holds the view, alive (its material's slot holds the material, which holds its
		// textures), and the request takes its own reference on it at once.
		auto [waiters, asked] = mb.inFlight.try_emplace(a_view);
		if (asked) {
			GpuTextures::Get().Request(a_view, a_sourceTag, mb.replies, mb.cookie);
			++mb.stats.requested;
		}
		waiters->second.push_back(a_slot);
		return { Lookups::kNone, {}, true };
	}

	void SceneStore::RetireMaterialBinding(std::uint32_t a_slot)
	{
		auto& mb = materialBindings;
		if (a_slot >= lookups.materials.size())
			return;
		// The lookups' entry emptied: its owners let go here, their last release on the cleanup queue (whichever holder of a publication
		// naming them lets go last). Versioned with the other changes (VersionMaterialBindings).
		lookups.materials.Mutable(a_slot) = {};
		if (a_slot < mb.held.size())
			mb.held[a_slot] = 0;
		mb.MarkChanged(a_slot);
		++mb.stats.retired;
	}

	void SceneStore::ResetMaterialBindings()
	{
		// The lookups are made new with the tables (ResetSlotTables), and these entries with them.
		auto& mb = materialBindings;
		mb.held.clear();
		mb.changed.clear();
		mb.changedMark.clear();
		mb.inFlight.clear();
		mb.cache.clear();
		mb.arrived.clear();
		mb.usedSeen.clear();
		mb.cursor = {};
		++mb.cookie;
	}

	void SceneStore::VersionMaterialBindings()
	{
		ZoneScopedN("CS.DCLF.Scene.VersionMaterialBindings");
		// The scene lane, before a commit or a publication (ResolveLookups): the entries written since the last time (resolved, re-keyed,
		// retired) each take a new version and a log entry, as the frame's start gave them when it copied them in.
		auto& mb = materialBindings;
		if (lookups.materialVersions.size() < lookups.materials.size())
			lookups.materialVersions.resize(lookups.materials.size(), 0);
		if (mb.changed.empty())
			return;
		for (const std::uint32_t slot : mb.changed) {
			mb.changedMark[slot] = 0;
			if (slot >= lookups.materials.size())
				continue;
			// A new version for every change (the kept bindings key on it); a retired slot's empty entry has none, as before it resolved.
			const std::uint32_t version = lookups.materials[slot].key.first ? lookups.NextVersion() : 0u;
			lookups.materials.Mutable(slot).version = version;
			lookups.materialVersions[slot] = version;
			lookups.materialLog.Push(slot);
		}
		mb.stats.versioned += mb.changed.size();
		mb.changed.clear();
		// Each publication copies the log (Lookups::Snapshot): its older half goes past this, and a reader behind it reads every pair again.
		lookups.materialLog.Trim(kMaterialLogEntries);
		// A build made against the entries before may have deferred draws they now resolve, or read an index they let go of.
		++lookups.generation;
	}
}
