// Per-mesh skin materials, sampled at frame start. Characters may share a material yet override
// its profile and textures independently, so these belong to the object shading row.
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"
#include "Features/Skin.h"
#include "Globals.h"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace DCLF::Integrations::AdvancedSkin
{
	namespace
	{
		struct MaterialRow
		{
			float flags[4]{};
			float profile[28]{};
			std::uint32_t textures[4]{};
		};
		static_assert(sizeof(MaterialRow) == 144);
		struct Captured
		{
			MaterialRow row{};
			std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 3> views;
			std::shared_ptr<const void> resources;
			std::vector<std::shared_ptr<const void>> bindings;
		};
		std::unordered_map<std::uint32_t, Captured> captured;
		std::unordered_set<std::uint32_t> reset;
		std::unordered_set<std::uint32_t> candidates;
		GpuTextures::Binding sampler;
		winrt::com_ptr<ID3D11SamplerState> samplerSource;
		std::uint32_t generation = ~0u;

		void Membership(const Api::ShadingFieldMembership& a_membership)
		{
			for (const auto& change : a_membership.changes) {
				if (change.clear) {
					for (const auto& [slot, value] : captured)
						reset.insert(slot);
				} else {
					reset.insert(change.slot);
					candidates.insert(change.slot);
				}
			}
		}

		void Capture(Api::ShadingFieldCapture& a_capture)
		{
			auto& skin = globals::features::skin;
			auto& textures = GpuTextures::Get();
			const bool changedDevice = generation != textures.Generation();
			generation = textures.Generation();
			if (changedDevice || sampler.index == GpuTextures::kInvalid || samplerSource.get() != skin.GetDetailSampler()) {
				samplerSource.copy_from(skin.GetDetailSampler());
				sampler = textures.ResolveSamplerBinding(samplerSource.get());
			}
			// Discover meshes from publication changes, then visit only skin candidates each frame.
			// Static world geometry must not acquire a per-frame CPU walk just to support actor previews.
			for (const auto slot : a_capture.changedObjects)
				candidates.insert(slot);
			std::vector<std::uint32_t> slots;
			std::vector<RE::BSGeometry*> meshes;
			for (auto it = candidates.begin(); it != candidates.end();) {
				auto* geometry = *it < a_capture.objectGeometry.size() ? a_capture.objectGeometry[*it] : nullptr;
				auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				if (!SkinMaterials::IsCompatible(property)) {
					it = candidates.erase(it);
					continue;
				}
				slots.push_back(*it++);
				meshes.push_back(geometry);
			}
			std::unordered_set<std::uint32_t> seen;
			if (skin.loaded) {
				skin.PrepareCapture(meshes);
				for (std::size_t m = 0; m < slots.size(); ++m) {
					const auto slot = slots[m];
					auto* geometry = meshes[m];
					auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
					seen.insert(slot);
					auto snapshot = skin.CaptureGeometry(geometry, property);
					// Preview wetness is per mesh; it can differ from the owning actor's cached value.
					Api::ShadingFieldValue wetness{ .slot = slot, .field = Api::ShadingRowField::SkinPerGeometry };
					if (skin.settings.EnableSkin)
						std::memcpy(wetness.value.data(), &snapshot.data.skinPerGeometry, sizeof(snapshot.data.skinPerGeometry));
					a_capture.values.push_back(std::move(wetness));
					MaterialRow row{};
					std::memcpy(row.flags, &snapshot.data.materialFlags, sizeof(row.flags));
					std::memcpy(row.profile, &snapshot.data.profile, sizeof(row.profile));
					auto [it, inserted] = captured.try_emplace(slot);
					auto& previous = it->second;
					std::vector<std::shared_ptr<const void>> owners;
					owners.reserve(4);
					for (std::size_t i = 0; i < snapshot.textures.size(); ++i) {
						auto binding = !inserted && !changedDevice && previous.views[i] == snapshot.textures[i] &&
						                       previous.row.textures[i] != GpuTextures::kInvalid && (!snapshot.textures[i] || !GpuTextures::Superseded(previous.bindings[i].get())) ?
						                   GpuTextures::Binding{ previous.row.textures[i], previous.bindings[i] } :
						                   textures.ResolveBinding(snapshot.textures[i].get(), 64 + kMaterialTextureRegisters[i]);
						row.textures[i] = binding.index;
						owners.push_back(std::move(binding.owner));
						if (binding.index == GpuTextures::kInvalid) {
							if (i < 2)
								row.flags[i] = 0;
							else
								row.profile[11] = 0;
						}
					}
					row.textures[3] = sampler.index;
					owners.push_back(sampler.owner);
					if (sampler.index == GpuTextures::kInvalid)
						row.profile[11] = 0;
					if (inserted || changedDevice || reset.contains(slot) || std::memcmp(&previous.row, &row, sizeof(row)) != 0 || previous.views != snapshot.textures || previous.bindings != owners) {
						auto lease = textures.Seal(owners);
						previous = { row, std::move(snapshot.textures), std::move(lease), std::move(owners) };
						std::array<float, Api::kShadingFieldValueFloats> value{};
						std::memcpy(value.data(), &row, sizeof(row));
						a_capture.Put(slot, value, previous.resources);
					}
				}
			}
			for (auto it = captured.begin(); it != captured.end();) {
				if (!seen.contains(it->first)) {
					a_capture.Put(it->first, {});
					Api::ShadingFieldValue wetness{ .slot = it->first, .field = Api::ShadingRowField::SkinPerGeometry };
					auto* geometry = it->first < a_capture.objectGeometry.size() ? a_capture.objectGeometry[it->first] : nullptr;
					const auto value = skin.loaded && skin.settings.EnableSkin && geometry ? skin.GetWetness(geometry) : float4{};
					std::memcpy(wetness.value.data(), &value, sizeof(value));
					a_capture.values.push_back(std::move(wetness));
					it = captured.erase(it);
				} else {
					++it;
				}
			}
			reset.clear();
		}
	}

	std::array<ID3D11ShaderResourceView*, 3> CapturedTextures(std::uint32_t a_slot)
	{
		if (const auto it = captured.find(a_slot); it != captured.end())
			return { it->second.views[0].get(), it->second.views[1].get(), it->second.views[2].get() };
		return {};
	}

	void InstallMaterialTextures(Api::InstallContext& a_context)
	{
		a_context.RegisterShadingField({ .name = "skin material", .field = Api::ShadingRowField::SkinMaterial, .members = Api::EntryClass::ActorOwned, .membership = &Membership, .capture = &Capture });
		a_context.DeclarePerDrawBuffer({ .name = "SkinPerGeometry", .stage = Api::ShaderStage::Pixel, .constantRegister = kSkinPerGeometryRegister });
	}
}
