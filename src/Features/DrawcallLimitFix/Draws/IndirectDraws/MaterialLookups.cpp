#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF::Draws
{
	void RefreshMaterialLookups(SceneStore& a_store, const SceneStore::Tables& a_tables, std::uint32_t a_frame, const SceneStore::ProjectedTextures& a_projected, Lookups& a_lookups)
	{
		ZoneScopedN("CS.DCLF.RefreshMaterialLookups");
		auto& textures = GpuTextures::Get();
		const auto cleanup = org::runtime::GetActiveDescriptorService()->GetResourceCleanupQueue();
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
		if (note(a_lookups.nullTexture, textures.NullIndex()))
			a_lookups.sharedVersion = a_lookups.NextVersion();
		if (!a_lookups.samplersResolved) {
			for (std::uint32_t address = 0; address < 4; ++address)
				for (std::uint32_t filter = 0; filter < 5; ++filter)
					a_lookups.samplers[Lookups::SamplerIndex(address, filter)] = textures.Sampler(address, filter);
			a_lookups.samplersResolved = true;
			a_lookups.sharedVersion = a_lookups.NextVersion();
		}
		for (std::size_t i = 0; i < a_lookups.projectedTextures.size(); ++i) {
			const auto binding = a_projected.valid ? textures.ResolveBinding(a_projected.views[i], static_cast<std::uint32_t>(32 + i)) : GpuTextures::Binding{ Lookups::kNone, {} };
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
		a_lookups.materials.resize(a_tables.materials.size());
		for (std::size_t word = 0; word < a_tables.usedMaterialBits.size(); ++word) {
			for (std::uint64_t remaining = a_tables.usedMaterialBits[word]; remaining; remaining &= remaining - 1) {
				const std::uint32_t slot = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
				if (slot >= a_tables.materialLastUsed.size() || a_tables.materialLastUsed[slot] != a_frame)
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
				const auto& material = a_tables.materials[slot];
				// An unchanged material owns its bindings directly; there is no
				// periodic lifetime restamp of its descriptor indices.
				if (entry.resolved && entry.recordVersion == a_tables.materialVersion[slot] && entry.written == material.textureWritten && entry.texturesGeneration == textures.Generation()) {
					continue;
				}
				if (entry.texturesGeneration != textures.Generation())
					entry.alternateCharacterLight = {};
				for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
					if (!((material.textureWritten >> t) & 1)) {
						if (t == kAlternatingMaterialTextureRegister)
							entry.alternateCharacterLight = {};  // a cleared register releases both incarnations
						if (note(entry.textureIndex[t], Lookups::kNone)) {
							entry.version = a_lookups.NextVersion();
							bindingDirty = true;
						}
						bindingDirty |= static_cast<bool>(entry.textureOwners[t]);
						entry.textureOwners[t].reset();
						entry.views[t] = nullptr;
						continue;
					}
					auto& alternate = entry.alternateCharacterLight;
					const auto* view = material.textures[t];
					const auto binding = t == kAlternatingMaterialTextureRegister && alternate.view == view && alternate.owner ?
						GpuTextures::Binding{ alternate.index, alternate.owner } : textures.ResolveBinding(material.textures[t], t);
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
					if (entry.views[t] != view && t == kAlternatingMaterialTextureRegister)
						alternate = { entry.views[t], entry.textureIndex[t], entry.textureOwners[t] };
					const bool ownerChanged = entry.textureOwners[t].get() != binding.owner.get();
					const bool indexChanged = note(entry.textureIndex[t], binding.index);
					bindingDirty |= ownerChanged || indexChanged;
					if (ownerChanged) ++a_lookups.generation;
					if (indexChanged || ownerChanged)
						entry.version = a_lookups.NextVersion();
					entry.textureOwners[t] = binding.owner;
					entry.views[t] = material.textures[t];
				}
				for (std::uint32_t f = 0; f < kFeatureMaterialTextures; ++f) {
					const auto binding = material.featureTextures[f] ? textures.ResolveBinding(material.featureTextures[f], 16 + f) : GpuTextures::Binding{ Lookups::kNone, {} };
					const std::uint32_t index = binding.index;
					const bool ownerChanged = entry.featureOwners[f].get() != binding.owner.get();
					const bool indexChanged = note(entry.featureIndex[f], index);
					bindingDirty |= ownerChanged || indexChanged;
					if (ownerChanged) ++a_lookups.generation;
					if (indexChanged || ownerChanged)
						entry.version = a_lookups.NextVersion();
					entry.featureOwners[f] = binding.owner;
					entry.featureViews[f] = material.featureTextures[f];
				}
				if (!entry.resolved) {
					++a_lookups.generation;
					entry.version = a_lookups.NextVersion();
				}
				entry.resolved = true;
				entry.written = material.textureWritten;
				entry.texturesGeneration = textures.Generation();
				entry.recordVersion = a_tables.materialVersion[slot];
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
		for (std::size_t word = 0; word < a_tables.usedPipelineBits.size(); ++word) {
			for (std::uint64_t remaining = a_tables.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
				const std::uint32_t p = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
				if (p >= a_tables.pipelines.size())
					continue;
				if (!a_tables.PipelineUsed(p, a_frame))
					continue;
				const auto& technique = a_tables.TechniqueOf(p);
				auto& entry = a_lookups.pipelines[p];
				auto* view = technique.shadowMask ? technique.shadowMaskTexture : nullptr;
				if (entry.shadowMaskView == view && entry.shadowMaskTextureGeneration == textures.Generation() &&
					(view ? static_cast<bool>(entry.shadowMaskOwner) : entry.shadowMaskIndex == Lookups::kNone))
					continue;
				const auto binding = technique.shadowMask ? textures.ResolveBinding(technique.shadowMaskTexture, 48) : GpuTextures::Binding{ Lookups::kNone, {} };
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

	void RefreshKnownMaterialTextures(SceneStore& a_store, Lookups& a_lookups)
	{
		ZoneScopedN("CS.DCLF.RefreshKnownMaterialTextures");
		const auto& a_tables = a_store.GetTables();
		const std::uint32_t a_frame = a_store.GetFrame();
		auto& textures = GpuTextures::Get();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		const auto cleanup = host->ResourceCleanup();
		if (!cleanup)
			return;
		auto refreshSlot = [&](std::size_t slot) {
			if (slot >= a_tables.materialLastUsed.size() || a_tables.materialLastUsed[slot] != a_frame)
				return;
			auto& entry = a_lookups.materials[slot];
			const auto& material = a_tables.materials[slot];
			if (!entry.resolved || entry.key != a_tables.materialSlotKey[slot] || entry.written != material.textureWritten ||
				entry.recordVersion == a_tables.materialVersion[slot])
				return;
			std::array<GpuTextures::Binding, kPixelTextureSlots> bindings{};
			bool changed = false, known = true;
			for (std::uint32_t t = 0; t < kPixelTextureSlots && known; ++t) {
				if (!((material.textureWritten >> t) & 1) || entry.views[t] == material.textures[t])
					continue;
				changed = true;
				const auto& alternate = entry.alternateCharacterLight;
				if (t == kAlternatingMaterialTextureRegister && alternate.view == material.textures[t] && alternate.owner)
					bindings[t] = { alternate.index, alternate.owner };
				else
					known = textures.KnownBinding(material.textures[t], bindings[t]);
			}
			if (!changed || !known)
				return;
			for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
				if (!((material.textureWritten >> t) & 1) || entry.views[t] == material.textures[t])
					continue;
				if (entry.textureIndex[t] != bindings[t].index || entry.textureOwners[t].get() != bindings[t].owner.get()) {
					++a_lookups.generation;
					entry.version = a_lookups.NextVersion();
				}
				if (t == kAlternatingMaterialTextureRegister)
					entry.alternateCharacterLight = { entry.views[t], entry.textureIndex[t], entry.textureOwners[t] };
				entry.textureIndex[t] = bindings[t].index;
				entry.textureOwners[t] = std::move(bindings[t].owner);
				entry.views[t] = material.textures[t];
			}
			std::vector<std::shared_ptr<const void>> owners;
			owners.reserve(entry.textureOwners.size() + entry.featureOwners.size());
			for (const auto& owner : entry.textureOwners)
				owners.push_back(owner);
			for (const auto& owner : entry.featureOwners)
				owners.push_back(owner);
			entry.bindingBlock = org::ExecutionResourceLease::Create(cleanup, std::move(owners)).Owner();
			if (entry.featureViews == material.featureTextures)
				entry.recordVersion = a_tables.materialVersion[slot];
		};
		std::vector<std::uint32_t> changed;
		a_store.TakeMaterialTextureChanges(changed);
		for (const std::uint32_t slot : changed)
			if (slot < a_tables.materials.size() && slot < a_lookups.materials.size())
				refreshSlot(slot);
	}

	void RefreshShadowLookups(SceneStore& a_store, const SceneStore::Tables& a_tables, const std::array<bool, kShadowModeCount>& a_modeUsed,
		const std::array<std::uint32_t, kShadowModeCount>& a_modeRasterStates, DXGI_FORMAT a_dsvFormat, DXGI_FORMAT a_skyFormat, Lookups& a_lookups)
	{
		ZoneScopedN("CS.DCLF.RefreshShadowLookups");
		auto& textures = GpuTextures::Get();
		auto& pipelines = DrawPipelines::Get();
		auto& programs = ShaderPrograms::Get();
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
					if (a_lookups.shadowTextures.erase(srv)) ++a_lookups.generation;
					a_lookups.shadowTextureOwners.erase(srv);
				}
			}
		}
		{
			ZoneScopedN("CS.DCLF.RefreshShadow.RetryTextures");
			const std::vector<ID3D11ShaderResourceView*> pending(a_lookups.pendingShadowTextures.begin(), a_lookups.pendingShadowTextures.end());
			for (auto* srv : pending) {
				const auto binding = textures.ResolveBinding(srv, 192);
				auto [slot, inserted] = a_lookups.shadowTextures.try_emplace(srv, binding.index);
				const auto owner = a_lookups.shadowTextureOwners.find(srv);
				if (inserted || slot->second != binding.index || owner == a_lookups.shadowTextureOwners.end() || owner->second.get() != binding.owner.get())
					++a_lookups.generation;
				slot->second = binding.index;
				if (binding.owner) {
					a_lookups.shadowTextureOwners[srv] = binding.owner;
					a_lookups.pendingShadowTextures.erase(srv);
				}
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
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_modeUsed[m])
				continue;
			// Skylighting's map takes its own keys (complete techniques) and its target's format.
			const auto& keys = m == kSkyMode ? a_tables.skyKeysUsed : a_tables.shadowKeysUsed;
			const DXGI_FORMAT format = m == kSkyMode ? a_skyFormat : a_dsvFormat;
			for (const auto& key : keys) {
				const std::uint32_t modeBits = ModeBitsOf(m);
				const ShadowPipelineKey slotKey{ key.technique | modeBits, key.rasterFlags, key.vertexLayout };
				auto slotIt = a_lookups.shadowSlots.find(slotKey);
				if (slotIt == a_lookups.shadowSlots.end()) {
					if (a_lookups.shadowSlotKeys.size() >= kMaxShadowSlots)
						continue;  // its casters stay native
					slotIt = a_lookups.shadowSlots.emplace(slotKey, static_cast<std::uint32_t>(a_lookups.shadowSlotKeys.size())).first;
					a_lookups.shadowSlotKeys.push_back(slotKey);
					++a_lookups.generation;
				}
				const std::uint32_t slot = slotIt->second;
				const auto* program = [&] {
					ZoneScopedN("CS.DCLF.RefreshShadow.FindProgram");
					bool requested = false;
					const auto* found = programs.FindShadow(slotKey.technique, *utility, mayRequestProgram, &requested);
					mayRequestProgram &= !requested;
					return found;
				}();
				// The key under each rasterizer state its mode's views draw with.
				for (std::uint32_t states = (a_modeRasterStates[m] & 0xFFFFu) | (a_modeRasterStates[m] >> 16); states; states &= states - 1) {
					const auto state = static_cast<std::uint32_t>(std::countr_zero(states));
					const ShadowPipelineKey viewKey{ slotKey.technique, WithShadowState(slotKey.rasterFlags, state), slotKey.vertexLayout };
					const std::uint32_t set = [&] {
						ZoneScopedN("CS.DCLF.RefreshShadow.FindPipeline");
						return program ? pipelines.FindShadow(viewKey, *program, format) : DrawPipelines::kNotReady;
					}();
					const std::uint32_t index = set == DrawPipelines::kNotReady ? Lookups::kNone : set;
					auto [it, inserted] = a_lookups.shadowPipelines.try_emplace(viewKey, index);
					if (inserted || it->second != index) {
						++a_lookups.generation;
						it->second = index;
					}
					auto& row = a_lookups.shadowMapRows[state];
					if (row.size() <= slot)
						row.resize(slot + 1, Lookups::kNone);
					row[slot] = index;
				}
			}
		}
		TracyCZoneEnd(shadowPipelinesZone);
	}
}

#endif
