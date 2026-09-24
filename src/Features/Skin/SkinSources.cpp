#include "SkinSources.h"

#include <mutex>

namespace SkinSources
{
	namespace
	{
		constexpr size_t CacheLimit = 4096;
		std::recursive_mutex mutex;

		struct Conversion
		{
			RE::NiPointer<RE::BSShaderTextureSet> textureSet;
			std::array<RE::BSFixedString, RE::BSTextureSet::Texture::kTotal> paths;
			RE::FormID record = 0;
		};

		struct Receipt
		{
			RE::NiPointer<RE::BSShaderProperty> property;
			RE::BSTSmartPointer<RE::BSShaderMaterial> material;
			RE::NiPointer<RE::BSTextureSet> textureSet;
			RE::NiSourceTexturePtr normal;
			RE::BSShaderMaterial::Feature feature;
			RE::FormID record = 0;
			bool head = false;
		};

		struct HeadUpdate;
		thread_local HeadUpdate* headUpdate = nullptr;
		struct HeadUpdate
		{
			RE::BSShaderProperty* property = nullptr;
			RE::FormID record = 0;
			HeadUpdate* previous = std::exchange(headUpdate, this);
			~HeadUpdate() { headUpdate = previous; }
		};
		std::unordered_map<RE::BSTextureSet*, Conversion> conversions;
		std::unordered_map<RE::BSShaderProperty*, Receipt> receipts;

		RE::BSLightingShaderMaterialBase* SkinMaterial(RE::BSShaderProperty* a_property)
		{
			auto* material = a_property ? a_property->GetBaseMaterial() : nullptr;
			if (!material || material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
				(material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGen &&
					material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGenRGBTint))
				return nullptr;
			return static_cast<RE::BSLightingShaderMaterialBase*>(material);
		}

		RE::FormID TextureRecord(RE::BSTextureSet* a_set)
		{
			if (!a_set)
				return 0;
			if (auto* native = skyrim_cast<RE::BGSTextureSet*>(a_set))
				return native->GetFormID();
			auto it = conversions.find(a_set);
			if (it == conversions.end())
				return 0;
			for (size_t i = 0; i < it->second.paths.size(); ++i) {
				// The snapshot invalidates a receipt; paths never establish record identity.
				if (it->second.paths[i] != it->second.textureSet->textures[i]) {
					conversions.erase(it);
					return 0;
				}
			}
			return it->second.record;
		}

		void Capture(RE::BSShaderProperty* a_property, RE::FormID a_record, bool a_head)
		{
			auto* material = SkinMaterial(a_property);
			if (!material) {
				receipts.erase(a_property);
				return;
			}
			if (receipts.size() >= CacheLimit && !receipts.contains(a_property))
				receipts.clear();
			receipts.insert_or_assign(a_property, Receipt{ RE::NiPointer(a_property),
													  RE::BSTSmartPointer<RE::BSShaderMaterial>(material), material->textureSet, material->normalTexture,
													  material->GetFeature(), a_record, a_head });
		}

		const Receipt* FindReceipt(RE::BSShaderProperty* a_property, RE::BSLightingShaderMaterialBase* a_material)
		{
			auto it = receipts.find(a_property);
			if (it == receipts.end())
				return nullptr;
			const auto& receipt = it->second;
			if (receipt.material.get() != a_material || receipt.feature != a_material->GetFeature() ||
				receipt.textureSet != a_material->textureSet || receipt.normal != a_material->normalTexture ||
				(!receipt.head && TextureRecord(a_material->textureSet.get()) != receipt.record)) {
				receipts.erase(it);
				return nullptr;
			}
			return &receipt;
		}

		struct ConvertTextureSet
		{
			static RE::BSShaderTextureSet* thunk(RE::BGSTextureSet* a_record)
			{
				auto* result = func(a_record);
				if (result && a_record) {
					std::lock_guard lock(mutex);
					if (conversions.size() >= CacheLimit && !conversions.contains(result))
						conversions.clear();
					Conversion conversion;
					conversion.textureSet = RE::NiPointer(result);
					conversion.record = a_record->GetFormID();
					for (size_t i = 0; i < conversion.paths.size(); ++i)
						conversion.paths[i] = result->textures[i];
					conversions.insert_or_assign(result, std::move(conversion));
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SetTexturePath
		{
			static void thunk(RE::BSShaderTextureSet* a_set, RE::BSTextureSet::Texture a_slot, const char* a_path)
			{
				{
					std::lock_guard lock(mutex);
					conversions.erase(a_set);
				}
				func(a_set, a_slot, a_path);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LoadTextureSet
		{
			static void thunk(RE::BSLightingShaderProperty* a_property, RE::BSTextureSet* a_set)
			{
				func(a_property, a_set);
				std::lock_guard lock(mutex);
				auto* material = SkinMaterial(a_property);
				if (material && material->textureSet.get() == a_set)
					Capture(a_property, TextureRecord(a_set), false);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct PrepareHead
		{
			static void thunk(RE::BSFaceGenManager* a_manager, RE::BSFaceGenNiNode* a_node, RE::BGSHeadPart* a_part, RE::TESNPC* a_npc)
			{
				HeadUpdate update;
				func(a_manager, a_node, a_part, a_npc);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct UpdateHeadNormal
		{
			static void thunk(RE::BSFaceGenManager* a_manager, RE::BGSTextureSet* a_record, RE::BSShaderProperty* a_property)
			{
				func(a_manager, a_record, a_property);
				if (headUpdate) {
					headUpdate->property = a_property;
					headUpdate->record = a_record ? a_record->GetFormID() : 0;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct PrecacheTextures
		{
			static void thunk(RE::BSLightingShaderProperty* a_property)
			{
				func(a_property);
				if (headUpdate) {
					std::lock_guard lock(mutex);
					Capture(a_property, headUpdate->property == a_property ? headUpdate->record : 0, true);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CloneProperty
		{
			static RE::NiObject* thunk(RE::BSLightingShaderProperty* a_property, RE::NiCloningProcess& a_process)
			{
				auto* result = func(a_property, a_process);
				std::lock_guard lock(mutex);
				auto* material = SkinMaterial(a_property);
				auto* clone = result ? netimmerse_cast<RE::BSLightingShaderProperty*>(result) : nullptr;
				if (clone && material) {
					if (const auto* receipt = FindReceipt(a_property, material)) {
						auto* copied = SkinMaterial(clone);
						if (copied && copied->GetFeature() == material->GetFeature() &&
							copied->textureSet == material->textureSet && copied->normalTexture == material->normalTexture)
							Capture(clone, receipt->record, receipt->head);
					}
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (REL::Module::IsVR()) {
			logger::warn("[Advanced Skin] TXST source hooks are not verified for VR; using NIF and direct body records");
			return;
		}
		stl::detour_thunk<ConvertTextureSet>(REL::RelocationID(20905, 21361));
		stl::detour_thunk<LoadTextureSet>(REL::RelocationID(99865, 106510));
		stl::detour_thunk<PrepareHead>(REL::RelocationID(26259, 26838));
		stl::detour_thunk<UpdateHeadNormal>(REL::RelocationID(26260, 26839));
		stl::write_vfunc<0x27, SetTexturePath>(RE::VTABLE_BSShaderTextureSet[0]);
		stl::write_vfunc<0x3A, PrecacheTextures>(RE::VTABLE_BSLightingShaderProperty[0]);
		stl::write_vfunc<0x17, CloneProperty>(RE::VTABLE_BSLightingShaderProperty[0]);
	}

	void Prune()
	{
		std::lock_guard lock(mutex);
		std::erase_if(receipts, [](const auto& entry) { return entry.second.property->GetRefCount() == 1; });
		std::erase_if(conversions, [](const auto& entry) { return entry.second.textureSet->GetRefCount() == 1; });
	}

	RE::FormID Resolve(RE::BSShaderProperty* a_property, bool a_head)
	{
		std::lock_guard lock(mutex);
		auto* material = SkinMaterial(a_property);
		if (!material)
			return 0;
		if (const auto* receipt = FindReceipt(a_property, material))
			return receipt->record;
		if (a_head || material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGenRGBTint)
			return 0;
		return TextureRecord(material->textureSet.get());
	}
}
