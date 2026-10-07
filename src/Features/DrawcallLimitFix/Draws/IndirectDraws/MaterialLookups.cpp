#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF::Draws
{
	void RefreshMaterialLookups(SceneStore& a_store, const SceneStore::Tables& a_tables, bool a_members, const SceneStore::ProjectedTextures& a_projected, Lookups& a_lookups)
	{
		ZoneScopedN("CS.DCLF.RefreshMaterialLookups");
		auto& textures = GpuTextures::Get();
		// Outside an epoch (a main-pass kick) a view never imported is not queued (RequestBinding): its material stays
		// unresolved until the epoch's refresh asks again. The samplers are created only inside one.
		const bool inEpoch = org::runtime::GetActiveDescriptorService() != nullptr;
		auto* host = RenderGraphRuntime::Get().Host();
		const auto cleanup = host ? host->ResourceCleanup() : nullptr;
		if (!cleanup)
			return;
		auto sealOwners = [&](std::vector<std::shared_ptr<const void>> owners) -> std::shared_ptr<const void> {
			ZoneScopedN("CS.DCLF.RefreshMaterial.SealOwners");
			return org::ExecutionResourceLease::Create(cleanup, std::move(owners)).Owner();
		};
		const auto oldSharedVersion = a_lookups.sharedVersion;
		std::vector<std::uint32_t> retired;
		a_store.TakeRetiredMaterialSlots(retired);
		for (const auto slot : retired) {
			if (slot < a_lookups.materials.size()) {
				a_lookups.materials[slot] = {};
				if (slot < a_lookups.materialVersions.size())
					a_lookups.materialVersions[slot] = 0;
				a_lookups.materialLog.Push(static_cast<std::uint32_t>(slot));
				++a_lookups.generation;
			}
		}
		a_store.TakeRetiredPipelineSlots(retired);
		for (const auto slot : retired) {
			if (slot < a_lookups.pipelines.size()) {
				// EarlyPrepass may already have resolved a successor in this
				// recycled slot. Retire only the old texture binding here.
				auto& pipeline = a_lookups.pipelines[slot];
				pipeline.shadowMaskOwner.reset();
				pipeline.shadowMaskIndex = Lookups::kNone;
				pipeline.shadowMaskView = nullptr;
				pipeline.shadowMaskTextureGeneration = 0;
				pipeline.version = a_lookups.NextVersion();
				++a_lookups.generation;
			}
		}
		// Any change a build can observe bumps the generation, including an entry resolved for the first
		// time: a job built before it deferred those draws, and must not stand in for a build made after.
		// Every change is also a new version of what it belongs to (Lookups::NextVersion), which the kept bindings key on.
		auto note = [&](std::uint32_t& a_slot, std::uint32_t a_value) {
			const bool changed = a_slot != a_value;
			if (changed)
				++a_lookups.generation;
			a_slot = a_value;
			return changed;
		};
		// The null texture and the samplers are the shadow builds' too.
		if (note(a_lookups.nullTexture, textures.NullIndex())) {
			a_lookups.sharedVersion = a_lookups.NextVersion();
			++a_lookups.shadowGeneration;
		}
		if (!a_lookups.samplersResolved && inEpoch) {
			for (std::uint32_t address = 0; address < 4; ++address)
				for (std::uint32_t filter = 0; filter < 5; ++filter)
					a_lookups.samplers[Lookups::SamplerIndex(address, filter)] = textures.Sampler(address, filter);
			a_lookups.samplersResolved = true;
			a_lookups.sharedVersion = a_lookups.NextVersion();
			++a_lookups.generation;
			++a_lookups.shadowGeneration;
		}
		for (std::size_t i = 0; i < a_lookups.projectedTextures.size(); ++i) {
			// Imported off this thread (RequestBinding): kInvalid while it is, which defers the draws that read it.
			const auto binding = a_projected.valid ? textures.RequestBinding(a_projected.views[i], static_cast<std::uint32_t>(32 + i)) : GpuTextures::Binding{ Lookups::kNone, {} };
			const bool ownerChanged = a_lookups.projectedOwners[i].get() != binding.owner.get();
			if (ownerChanged) ++a_lookups.generation;
			if (note(a_lookups.projectedTextures[i], binding.index) || ownerChanged)
				a_lookups.sharedVersion = a_lookups.NextVersion();
			a_lookups.projectedOwners[i] = binding.owner;
		}
		if (oldSharedVersion != a_lookups.sharedVersion || !a_lookups.sharedBindingBlock) {
			std::vector<std::shared_ptr<const void>> owners;
			owners.reserve(1 + a_lookups.samplers.size() + a_lookups.projectedOwners.size());
			owners.push_back(textures.NullBinding().owner);
			for (std::uint32_t address = 0; address < 4; ++address)
				for (std::uint32_t filter = 0; filter < 5; ++filter)
					owners.push_back(textures.SamplerBinding(address, filter).owner);
			for (const auto& owner : a_lookups.projectedOwners)
				owners.push_back(owner);
			a_lookups.sharedBindingBlock = sealOwners(std::move(owners));
		}
		TracyCZoneN(materialBindingZone, "CS.DCLF.RefreshMaterial.Materials", true);
		// The records as the frame draws them (FrameTables: the frame's writes on the coordinator's records).
		const auto& frame = a_store.GetFrameTables();
		a_lookups.materials.resize(a_tables.materials.size());
		a_lookups.materialVersions.resize(a_tables.materials.size(), 0);
		// The members' materials and pipelines, once the accumulate phase has settled this frame's (a_members: not the shadow
		// epoch's refresh, ahead of it).
		for (std::size_t word = 0; a_members && word < a_tables.usedMaterialBits.size(); ++word) {
			for (std::uint64_t remaining = a_tables.usedMaterialBits[word]; remaining; remaining &= remaining - 1) {
				const std::uint32_t slot = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
				if (slot >= a_tables.materials.size() || slot >= frame.materials.size())
					continue;
				auto& entry = a_lookups.materials[slot];
				const auto& key = a_tables.materialSlotKey[slot];
				bool bindingDirty = !entry.resolved || entry.key != key;
				if (entry.key != key) {
					entry = {};  // predecessor incarnation cannot contribute an alternate binding
					entry.key = key;
					entry.textureIndex.fill(Lookups::kNone);
					entry.featureIndex.fill(Lookups::kNone);
					entry.version = a_lookups.NextVersion();
				}
				const auto& material = frame.materials[slot];
				// An unchanged material owns its bindings directly; there is no
				// periodic lifetime restamp of its descriptor indices.
				if (entry.resolved && entry.recordVersion == frame.materialVersion[slot] && entry.written == material.textureWritten && entry.texturesGeneration == textures.Generation()) {
					continue;
				}
				// A new record version is most often one texture: a view the entry already holds, resolved, is kept as it is,
				// without asking the registry again.
				const bool sameGeneration = entry.texturesGeneration == textures.Generation();
				// Readiness is held (the DCLF set: a member joined because its material resolved, and the engine no longer draws it):
				// a resolved entry whose view changed keeps drawing with the previous view, whose import its owner still holds, until
				// the new one has arrived. Then the next refresh takes it.
				const bool wasResolved = entry.resolved && sameGeneration;
				bool held = false;
				// A character-light pass's t11 is the frame's (kCharacterLightRegister): the record holds its modes, no view.
				const std::uint32_t textureWritten = material.textureWritten & ~(MaterialSources::FrameCharacterLight(key.second) ? 1u << kCharacterLightMaterialRegister : 0u);
				bool importsPending = false;  // a texture the import thread has not finished (RequestBinding)
				for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
					if (!((textureWritten >> t) & 1)) {
						if (note(entry.textureIndex[t], Lookups::kNone)) {
							entry.version = a_lookups.NextVersion();
							bindingDirty = true;
						}
						bindingDirty |= static_cast<bool>(entry.textureOwners[t]);
						entry.textureOwners[t].reset();
						entry.views[t] = nullptr;
						continue;
					}
					const auto* view = material.textures[t];
					if (sameGeneration && entry.views[t] == view && entry.textureOwners[t] && entry.textureIndex[t] != Lookups::kNone)
						continue;
					const auto binding = textures.RequestBinding(material.textures[t], t);
					if (binding.pending && wasResolved && entry.textureOwners[t] && entry.textureIndex[t] != Lookups::kNone) {
						held = true;
						continue;
					}
					importsPending |= binding.pending;
					const bool tracePaths = !SwitchValue(Switch::TraceTexturePaths).empty();
					if (tracePaths && t < 2 && (entry.views[t] != view || entry.textureOwners[t].get() != binding.owner.get())) {
						TracyPlot("CS.DCLF.Texture.ChangedMaterialSlot", static_cast<std::int64_t>(slot));
						TracyPlot("CS.DCLF.Texture.ChangedMaterialKey", static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(key.first)));
						TracyPlot("CS.DCLF.Texture.ChangedMaterialPass", static_cast<std::int64_t>(key.second));
						TracyPlot("CS.DCLF.Texture.ChangedMaterialRegister", static_cast<std::int64_t>(t));
						TracyPlot("CS.DCLF.Texture.ChangedOldView", static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(entry.views[t])));
						TracyPlot("CS.DCLF.Texture.ChangedNewView", static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(view)));
						if (key.first) {
							static std::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> reports;
							auto& count = reports[key];
							if (reports.size() < 65536 && (++count & 15u) == 1u) {
								const auto type = key.first->GetType();
								const auto textureSet = type == RE::BSShaderMaterial::Type::kLighting ?
									static_cast<const RE::BSLightingShaderMaterialBase*>(key.first)->GetTextureSet() : nullptr;
								const char* diffuse = textureSet ? textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
								const char* normal = textureSet ? textureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal) : nullptr;
								const auto message = fmt::format("DCLF.TexturePath material={:#x} pass={:#x} slot={} type={} diffuse={} normal={}",
									reinterpret_cast<std::uintptr_t>(key.first), key.second, slot, static_cast<int>(type), diffuse ? diffuse : "", normal ? normal : "");
								TracyMessage(message.data(), message.size());
							}
						}
					}
					const bool ownerChanged = entry.textureOwners[t].get() != binding.owner.get();
					const bool indexChanged = note(entry.textureIndex[t], binding.index);
					if (ownerChanged) ++a_lookups.generation;
					if (indexChanged || ownerChanged)
						entry.version = a_lookups.NextVersion();
					entry.textureOwners[t] = binding.owner;
					entry.views[t] = material.textures[t];
					bindingDirty |= ownerChanged;
				}
				for (std::uint32_t f = 0; f < kFeatureMaterialTextures; ++f) {
					if (sameGeneration && entry.featureViews[f] == material.featureTextures[f] && (!material.featureTextures[f] || (entry.featureOwners[f] && entry.featureIndex[f] != Lookups::kNone)))
						continue;
					const auto binding = material.featureTextures[f] ? textures.RequestBinding(material.featureTextures[f], 16 + f) : GpuTextures::Binding{ Lookups::kNone, {} };
					if (binding.pending && wasResolved && entry.featureOwners[f] && entry.featureIndex[f] != Lookups::kNone) {
						held = true;
						continue;
					}
					importsPending |= binding.pending;
					const std::uint32_t index = binding.index;
					const bool ownerChanged = entry.featureOwners[f].get() != binding.owner.get();
					const bool indexChanged = note(entry.featureIndex[f], index);
					bindingDirty |= ownerChanged;
					if (ownerChanged) ++a_lookups.generation;
					if (indexChanged || ownerChanged)
						entry.version = a_lookups.NextVersion();
					entry.featureOwners[f] = binding.owner;
					entry.featureViews[f] = material.featureTextures[f];
				}
				// A texture still being imported leaves the material unresolved: its draws defer (they stay the engine's), and the
				// next refresh asks again. It turns resolved, a new version, when the last one arrives. Nothing else changes
				// while it waits, so the builds made meanwhile stay current.
				if (!importsPending && !entry.resolved) {
					++a_lookups.generation;
					entry.version = a_lookups.NextVersion();
				}
				entry.resolved = !importsPending;
				if (a_lookups.materialVersions[slot] != entry.version)
					a_lookups.materialLog.Push(static_cast<std::uint32_t>(slot));
				a_lookups.materialVersions[slot] = entry.version;
				entry.written = material.textureWritten;  // the record's, so a character-light pass's is not resolved every refresh
				entry.texturesGeneration = textures.Generation();
				// A held view is asked for again by the next refresh (no record version is 0).
				entry.recordVersion = held ? 0 : frame.materialVersion[slot];
				if (bindingDirty) {
					std::vector<std::shared_ptr<const void>> owners;
					owners.reserve(entry.textureOwners.size() + entry.featureOwners.size());
					for (const auto& owner : entry.textureOwners)
						owners.push_back(owner);
					for (const auto& owner : entry.featureOwners)
						owners.push_back(owner);
					entry.bindingBlock = sealOwners(std::move(owners));
				}
			}
		}
		TracyCZoneEnd(materialBindingZone);
		// The technique's shadow mask, per used pipeline: the frame's view, so it is refreshed every epoch.
		a_lookups.pipelines.resize(std::max(a_lookups.pipelines.size(), a_tables.pipelines.size()));
		for (std::size_t word = 0; a_members && word < a_tables.usedPipelineBits.size(); ++word) {
			for (std::uint64_t remaining = a_tables.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
				const std::uint32_t p = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
				if (p >= a_tables.pipelines.size())
					continue;
				const auto& technique = a_store.GetFrameTables().techniques[a_tables.pipelineTechnique[p]].value;
				auto& entry = a_lookups.pipelines[p];
				auto* view = technique.shadowMask ? technique.shadowMaskTexture : nullptr;
				// A view still being imported holds its owner with no index yet: asked again until it has one.
				if (entry.shadowMaskView == view && entry.shadowMaskTextureGeneration == textures.Generation() &&
					(view ? entry.shadowMaskOwner && entry.shadowMaskIndex != Lookups::kNone : entry.shadowMaskIndex == Lookups::kNone))
					continue;
				const auto binding = technique.shadowMask ? textures.RequestBinding(technique.shadowMaskTexture, 48) : GpuTextures::Binding{ Lookups::kNone, {} };
				// Held while the new view imports, as a material's textures are (the set's members draw with it).
				if (binding.pending && entry.shadowMaskOwner && entry.shadowMaskIndex != Lookups::kNone &&
					entry.shadowMaskTextureGeneration == textures.Generation())
					continue;
				const bool ownerChanged = entry.shadowMaskOwner.get() != binding.owner.get();
				if (ownerChanged) ++a_lookups.generation;
				if (note(entry.shadowMaskIndex, binding.index) || ownerChanged)
					entry.version = a_lookups.NextVersion();
				entry.shadowMaskOwner = binding.owner;
				entry.shadowMaskView = view;
				entry.shadowMaskTextureGeneration = textures.Generation();
			}
		}
	}

	void RefreshShadowLookups(SceneStore& a_store, const SceneStore::Tables& a_tables, const std::array<bool, kShadowModeCount>& a_modeUsed,
		const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates, DXGI_FORMAT a_dsvFormat, const std::array<DXGI_FORMAT, kOcclusionViews>& a_occlusionFormats,
		Lookups& a_lookups)
	{
		ZoneScopedN("CS.DCLF.RefreshShadowLookups");
		auto& textures = GpuTextures::Get();
		auto* utility = globals::game::utilityShader;
		TracyCZoneN(shadowTexturesZone, "CS.DCLF.RefreshShadow.Textures", true);
		std::vector<std::pair<ID3D11ShaderResourceView*, bool>> textureChanges;
		a_store.TakeShadowTextureChanges(textureChanges);
		{
			ZoneScopedN("CS.DCLF.RefreshShadow.ApplyTextureChanges");
			for (const auto& [srv, present] : textureChanges) {
				if (present) {
					a_lookups.pendingShadowTextures.insert(srv);
				} else {
					a_lookups.pendingShadowTextures.erase(srv);
					if (a_lookups.shadowTextures.erase(srv)) ++a_lookups.shadowGeneration;
					a_lookups.shadowTextureOwners.erase(srv);
				}
			}
		}
		{
			ZoneScopedN("CS.DCLF.RefreshShadow.RetryTextures");
			const std::vector<ID3D11ShaderResourceView*> pending(a_lookups.pendingShadowTextures.begin(), a_lookups.pendingShadowTextures.end());
			for (auto* srv : pending) {
				// Still imported (RequestBinding): not in the table yet, so its casters wait (deferredTextures). The import's
				// owner is held meanwhile.
				const auto binding = textures.RequestBinding(srv, 192);
				if (binding.pending) {
					a_lookups.shadowTextureOwners[srv] = binding.owner;
					continue;
				}
				auto [slot, inserted] = a_lookups.shadowTextures.try_emplace(srv, binding.index);
				const auto owner = a_lookups.shadowTextureOwners.find(srv);
				const void* heldOwner = owner == a_lookups.shadowTextureOwners.end() ? nullptr : owner->second.get();
				if (inserted || slot->second != binding.index || heldOwner != binding.owner.get())
					++a_lookups.shadowGeneration;
				slot->second = binding.index;
				if (binding.owner)
					a_lookups.shadowTextureOwners[srv] = binding.owner;
				else if (owner != a_lookups.shadowTextureOwners.end())
					a_lookups.shadowTextureOwners.erase(owner);
				// Imported or rejected (kInvalid, which its casters skip): a rejection is permanent, so neither is asked again.
				a_lookups.pendingShadowTextures.erase(srv);
			}
		}
		TracyCZoneValue(shadowTexturesZone, (static_cast<std::uint64_t>(textureChanges.size()) << 32) | a_lookups.pendingShadowTextures.size());
		TracyCZoneEnd(shadowTexturesZone);
		TracyPlot("CS.DCLF.ShadowTextureChanges", static_cast<std::int64_t>(textureChanges.size()));
		TracyPlot("CS.DCLF.ShadowTexturePending", static_cast<std::int64_t>(a_lookups.pendingShadowTextures.size()));
		TracyPlot("CS.DCLF.ShadowTextureOwners", static_cast<std::int64_t>(a_lookups.shadowTextureOwners.size()));
		if (!utility)
			return;
		// Request setup hashes the shader dependency tree and may take 10+ ms. Missing pipelines leave their
		// casters on the native path, so introduce at most one new Utility technique per frame instead of
		// multiplying a render-thread hitch when a scene exposes several techniques at once.
		bool mayRequestProgram = true;
		TracyCZoneN(shadowPipelinesZone, "CS.DCLF.RefreshShadow.Pipelines", true);
		// Every state of each mode's views, either caster class.
		std::array<std::vector<std::uint32_t>, kShadowModeCount> modeStates;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			modeStates[m] = a_modeRasterStates[m].All();
		// What the pipelines are found for: the pipeline set, each used mode's keys, states and format. A refresh for the same
		// as the last one, which found every pipeline, would find the same ones again (a found pipeline stays in the set until
		// the set is recreated, a new generation): it is skipped, and a frame pays only when a key, a state or the set changes.
		std::vector<std::uint64_t> resolvedFor;
		resolvedFor.push_back(DrawPipelines::Get().Generation());
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_modeUsed[m])
				continue;
			const auto& keys = IsOcclusionMode(m) ? a_tables.occlusionKeysUsed[OcclusionOfMode(m)] : a_tables.shadowKeysUsed;
			resolvedFor.push_back((std::uint64_t(m) << 32) | (IsOcclusionMode(m) ? a_occlusionFormats[OcclusionOfMode(m)] : a_dsvFormat));
			resolvedFor.push_back((std::uint64_t(keys.size()) << 32) | modeStates[m].size());
			for (const auto& key : keys) {
				resolvedFor.push_back((std::uint64_t(key.technique) << 32) | key.rasterFlags);
				resolvedFor.push_back(key.vertexLayout);
			}
			for (const std::uint32_t state : modeStates[m])
				resolvedFor.push_back(state);
		}
		if (resolvedFor == a_lookups.shadowPipelinesResolvedFor) {
			TracyCZoneEnd(shadowPipelinesZone);
			a_lookups.shadowRefreshDue = false;
			return;
		}
		bool resolved = true;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_modeUsed[m])
				continue;
			// An occlusion view takes its own keys (complete techniques) and its target's format.
			const auto& keys = IsOcclusionMode(m) ? a_tables.occlusionKeysUsed[OcclusionOfMode(m)] : a_tables.shadowKeysUsed;
			const DXGI_FORMAT format = IsOcclusionMode(m) ? a_occlusionFormats[OcclusionOfMode(m)] : a_dsvFormat;
			for (const auto& key : keys) {
				const std::uint32_t modeBits = ModeBitsOf(m);
				const ShadowPipelineKey slotKey{ key.technique | modeBits, key.rasterFlags, key.vertexLayout };
				auto slotIt = a_lookups.shadowSlots.find(slotKey);
				if (slotIt == a_lookups.shadowSlots.end()) {
					// The latch's map rows hold every slot this refresh can add (ReserveShadowLatch, before the epoch).
					slotIt = a_lookups.shadowSlots.emplace(slotKey, static_cast<std::uint32_t>(a_lookups.shadowSlotKeys.size())).first;
					a_lookups.shadowSlotKeys.push_back(slotKey);
					++a_lookups.shadowGeneration;
				}
				const std::uint32_t slot = slotIt->second;
				// Everything requested here is built at runtime: each build a request starts is logged (RequestShadowProgram,
				// RequestShadowPipeline).
				const std::uint32_t occlusion = IsOcclusionMode(m) ? OcclusionOfMode(m) : ~0u;
				const auto* program = [&] {
					ZoneScopedN("CS.DCLF.RefreshShadow.FindProgram");
					bool requested = false;
					const auto* found = RequestShadowProgram(slotKey.technique, key, occlusion, *utility, mayRequestProgram, &requested);
					mayRequestProgram &= !requested;
					return found;
				}();
				// The key under each rasterizer state its mode's views draw with.
				for (const std::uint32_t state : modeStates[m]) {
					const ShadowPipelineKey viewKey{ slotKey.technique, slotKey.rasterFlags, slotKey.vertexLayout, state };
					const std::uint32_t set = [&] {
						ZoneScopedN("CS.DCLF.RefreshShadow.FindPipeline");
						return program ? RequestShadowPipeline(viewKey, *program, format, key, occlusion) : DrawPipelines::kNotReady;
					}();
					const std::uint32_t index = set == DrawPipelines::kNotReady ? Lookups::kNone : set;
					resolved &= set != DrawPipelines::kNotReady;
					auto [it, inserted] = a_lookups.shadowPipelines.try_emplace(viewKey, index);
					if (inserted || it->second != index) {
						++a_lookups.shadowGeneration;
						it->second = index;
					}
					if (a_lookups.shadowMapRows.size() <= state)
						a_lookups.shadowMapRows.resize(std::size_t(state) + 1);
					auto& row = a_lookups.shadowMapRows[state];
					if (row.size() <= slot)
						row.resize(slot + 1, Lookups::kNone);
					row[slot] = index;
				}
			}
		}
		TracyCZoneEnd(shadowPipelinesZone);
		// A pipeline not found yet (its program or its build still pending, or rejected) is asked for again next refresh.
		a_lookups.shadowPipelinesResolvedFor = resolved ? std::move(resolvedFor) : std::vector<std::uint64_t>{};
		a_lookups.shadowRefreshDue = false;
	}
}

#endif
