// Coordinator::UpdateMaterialBindings and its helpers (MaterialBindings.h), scene lane.

#include "Internal.h"

namespace DCLF::Scene
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

	void Coordinator::UpdateMaterialBindings()
	{
		ZoneScopedN("CS.DCLF.Scene.MaterialBindings");
		auto& mb = materialBindings;
		if (lookups.materials.size() < tables.materials.size())
			lookups.materials.resize(tables.materials.size());
		if (mb.held.size() < tables.materials.size())
			mb.held.resize(tables.materials.size(), 0);
		// The answers since the last pass: each view's waiting slots written from it (a rejection too: that texture unresolvable). An
		// answer nobody waits on any more lets its owner go here: the last release of an import, or of its descriptor, runs on the cleanup
		// queue (ResourceCleanupQueue::Make), never on this thread.
		std::vector<std::uint32_t> waiting;
		mb.replies.Drain([&](GpuTextures::Reply&& a_reply) {
			if (a_reply.cookie != mb.cookie) {
				++mb.stats.stale;
				return;
			}
			++mb.stats.answered;
			if (const auto it = mb.inFlight.find(a_reply.view); it != mb.inFlight.end()) {
				for (const std::uint32_t slot : it->second)
					mb.waiting.erase(std::pair{ a_reply.view, slot });
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

	void Coordinator::ResolveMaterialBinding(std::uint32_t a_slot)
	{
		auto& mb = materialBindings;
		// Only a used slot: the used set certifies the views a record holds (Tables, "The three shared tables"). A slot freed keeps its
		// entry until its retirement comes back (RetireMaterialBinding).
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
		// The entry is the lookups', written straight in; its version and log entry come before the next commit or
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
		// The owners held now (the entry's block, immutable); those that change are collected and sealed into a new block after.
		const Lookups::MaterialOwners* owners = entry.bindingBlock.get();
		Lookups::MaterialOwners next;
		std::uint32_t texturesMoved = 0, featuresMoved = 0;
		// One texture: kept while its view and binding stand; held while a resolved entry's new view is asked for; else what is known.
		auto bind = [&](ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t& a_index, bool a_feature, std::uint32_t a_at,
						ID3D11ShaderResourceView*& a_seen) {
			const void* owner = owners ? (a_feature ? owners->features[a_at] : owners->textures[a_at]).get() : nullptr;
			if (sameGeneration && a_seen == a_view && owner && a_index != Lookups::kNone)
				return;
			auto binding = MaterialViewBinding(a_view, a_sourceTag, a_slot);
			if (binding.pending && wasResolved && owner && a_index != Lookups::kNone) {
				held = true;
				return;
			}
			pending |= binding.pending;
			const bool ownerChanged = owner != binding.owner.get();
			changed |= ownerChanged || a_index != binding.index;
			if (ownerChanged) {
				ownersChanged = true;
				(a_feature ? featuresMoved : texturesMoved) |= 1u << a_at;
				(a_feature ? next.features[a_at] : next.textures[a_at]) = std::move(binding.owner);
			}
			a_index = binding.index;
			a_seen = a_view;
		};
		auto clear = [&](std::uint32_t& a_index, bool a_feature, std::uint32_t a_at, ID3D11ShaderResourceView*& a_seen) {
			const bool owner = owners && (a_feature ? owners->features[a_at] : owners->textures[a_at]);
			changed |= a_index != Lookups::kNone || owner;
			if (owner) {
				ownersChanged = true;
				(a_feature ? featuresMoved : texturesMoved) |= 1u << a_at;
			}
			a_index = Lookups::kNone;
			a_seen = nullptr;
		};
		const std::uint32_t textureWritten = material.textureWritten & ~(MaterialSources::FrameCharacterLight(key.second) ? 1u << kCharacterLightTexture : 0u);
		for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
			if ((textureWritten >> t) & 1)
				bind(material.textures[t], t, entry.textureIndex[t], false, t, entry.views[t]);  // a null view: the null descriptor
			else
				clear(entry.textureIndex[t], false, t, entry.views[t]);
		}
		for (std::uint32_t f = 0; f < kFeatureMaterialTextures; ++f) {
			if (material.featureTextures[f])
				bind(material.featureTextures[f], kFeatureSourceTag + f, entry.featureIndex[f], true, f, entry.featureViews[f]);
			else
				clear(entry.featureIndex[f], true, f, entry.featureViews[f]);
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
		if (ownersChanged)
			entry.bindingBlock = SealMaterialOwners(key.first, owners, next, texturesMoved, featuresMoved);
		if (changed)
			mb.MarkChanged(a_slot);
	}

	std::shared_ptr<const Lookups::MaterialOwners> Coordinator::SealMaterialOwners(const RE::BSShaderMaterial* a_material, const Lookups::MaterialOwners* a_held,
		Lookups::MaterialOwners& a_next, std::uint32_t a_texturesMoved, std::uint32_t a_featuresMoved)
	{
		auto& mb = materialBindings;
		// The unmoved owners kept from the held block; none at all holds nothing.
		bool any = false;
		for (std::uint32_t t = 0; t < a_next.textures.size(); ++t) {
			if (!((a_texturesMoved >> t) & 1) && a_held)
				a_next.textures[t] = a_held->textures[t];
			any |= static_cast<bool>(a_next.textures[t]);
		}
		for (std::uint32_t f = 0; f < a_next.features.size(); ++f) {
			if (!((a_featuresMoved >> f) & 1) && a_held)
				a_next.features[f] = a_held->features[f];
			any |= static_cast<bool>(a_next.features[f]);
		}
		if (!any)
			return nullptr;
		// Another slot of the material sealed the same owners (its other passes name the same textures): one block for both.
		auto& shared = mb.sealed[a_material];
		if (auto block = shared.lock(); block && block->textures == a_next.textures && block->features == a_next.features) {
			++mb.work.sealShared;
			return block;
		}
		std::vector<std::shared_ptr<const void>> list;
		list.reserve(a_next.textures.size() + a_next.features.size());
		list.insert(list.end(), a_next.textures.begin(), a_next.textures.end());
		list.insert(list.end(), a_next.features.begin(), a_next.features.end());
		a_next.lease = GpuTextures::Get().Seal(std::move(list));
		auto block = std::make_shared<const Lookups::MaterialOwners>(std::move(a_next));
		shared = block;
		++mb.work.sealed;
		// Amortized: each block sealed looks at two other materials' and drops one nobody holds any more.
		for (std::uint32_t i = 0; i < 2 && !mb.sealed.empty(); ++i) {
			mb.sealedSweep = (mb.sealedSweep + 1) % mb.sealed.size();
			if (const auto swept = mb.sealed.begin() + static_cast<std::ptrdiff_t>(mb.sealedSweep); swept->second.expired())
				mb.sealed.erase(swept);
		}
		return block;
	}

	GpuTextures::Binding Coordinator::MaterialViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t a_slot)
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
		// Once per (view, slot): a slot resolved again while it waits (a held view, a record rewritten meanwhile) is listed already.
		if (mb.waiting.emplace(a_view, a_slot).second)
			waiters->second.push_back(a_slot);
		else
			++mb.work.waitersDeduped;
		return { Lookups::kNone, {}, true };
	}

	void Coordinator::RetireMaterialBinding(std::uint32_t a_slot)
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

	void Coordinator::ResetMaterialBindings()
	{
		// The lookups are made new with the tables (ResetSlotTables), and these entries with them.
		auto& mb = materialBindings;
		mb.held.clear();
		mb.changed.clear();
		mb.changedMark.clear();
		mb.inFlight.clear();
		mb.waiting.clear();
		mb.sealed.clear();
		mb.sealedSweep = 0;
		mb.cache.clear();
		mb.arrived.clear();
		mb.usedSeen.clear();
		mb.cursor = {};
		++mb.cookie;
	}

	void Coordinator::VersionMaterialBindings()
	{
		ZoneScopedN("CS.DCLF.Scene.VersionMaterialBindings");
		// The scene lane, before a commit or a publication (ResolveLookups): the entries written since the last time (resolved, re-keyed,
		// retired) each take a new version and a log entry.
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
