#include "Sources.h"
#include "../Editor/NifDocument.h"

#include <cctype>
#include <mutex>

namespace SkinSources
{
	namespace
	{
		std::recursive_mutex mutex;
		uint64_t revision = 1;
		struct Conversion
		{
			RE::NiPointer<RE::BSShaderTextureSet> set;
			std::array<std::string, 9> paths;
			RE::FormID record = 0;
		};
		struct Record
		{
			RE::NiPointer<RE::BSShaderProperty> property;
			std::shared_ptr<const Asset> asset;
			RE::NiPointer<RE::BSTextureSet> set;
			RE::NiSourceTexturePtr normal;
			RE::FormID txst = 0;
			RE::FormID headPart = 0;
			RE::FormID npc = 0;
			uint64_t revision = 0;
		};
		std::unordered_map<RE::BSShaderTextureSet*, Conversion> conversions;
		std::unordered_map<RE::BSShaderProperty*, Record> records;

		RE::FormID TextureRecord(RE::BSTextureSet* a_set)
		{
			if (!a_set)
				return 0;
			if (auto* form = skyrim_cast<RE::BGSTextureSet*>(a_set))
				return form->GetFormID();
			auto* native = netimmerse_cast<RE::BSShaderTextureSet*>(a_set);
			const auto found = conversions.find(native);
			if (found == conversions.end())
				return 0;
			if (Paths(a_set) != found->second.paths) {
				conversions.erase(found);
				return 0;
			}
			return found->second.record;
		}

		struct HeadUpdate;
		thread_local HeadUpdate* headUpdate = nullptr;
		struct HeadUpdate
		{
			RE::BGSHeadPart* part;
			RE::TESNPC* npc;
			RE::BSShaderProperty* property = nullptr;
			RE::FormID record = 0;
			HeadUpdate* previous = std::exchange(headUpdate, this);
			~HeadUpdate() { headUpdate = previous; }
		};

		void Capture(RE::BSShaderProperty* a_property)
		{
			if (!SkinMaterials::IsCompatible(a_property))
				return;
			auto& record = records[a_property];
			auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material);
			record.property = RE::NiPointer(a_property);
			record.set = material->textureSet;
			record.normal = material->normalTexture;
			record.txst = TextureRecord(material->textureSet.get());
			if (headUpdate && headUpdate->property == a_property) {
				record.txst = headUpdate->record;
				record.headPart = headUpdate->part ? headUpdate->part->GetFormID() : 0;
				record.npc = headUpdate->npc ? headUpdate->npc->GetFormID() : 0;
			}
			record.revision = ++revision;
		}

		struct PostLink
		{
			static void thunk(RE::BSLightingShaderProperty* a_property, RE::NiStream& a_stream)
			{
				func(a_property, a_stream);
				if (!SkinMaterials::IsCompatible(a_property))
					return;
				auto asset = std::make_shared<Asset>();
				asset->definition = SkinMaterials::ReadProperty(a_property);
				asset->path = a_stream.inputFilePath;
				try {
					asset->fingerprint = SkinEditor::Fingerprint(SkinEditor::ReadResource(asset->path));
				} catch (const std::exception& e) {
					logger::warn("[Advanced Skin] Source snapshot unavailable: {}: {}", asset->path, e.what());
				}
				for (uint32_t i = 0; i < a_stream.objects.size(); ++i) {
					if (a_stream.objects[i].get() == a_property) {
						asset->block = i;
						break;
					}
				}
				auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material);
				asset->textures = Paths(material->textureSet.get());
				if (!asset->definition.diagnostic.empty())
					logger::warn("[Advanced Skin] {} block {}: {}", asset->path, asset->block, asset->definition.diagnostic);
				std::lock_guard lock(mutex);
				auto& record = records[a_property];
				record.property = RE::NiPointer(a_property);
				record.asset = std::move(asset);
				record.revision = ++revision;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ConvertTextureSet
		{
			static RE::BSShaderTextureSet* thunk(RE::BGSTextureSet* a_record)
			{
				auto* result = func(a_record);
				if (result && a_record) {
					std::lock_guard lock(mutex);
					conversions.insert_or_assign(result, Conversion{ RE::NiPointer(result), Paths(result), a_record->GetFormID() });
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
		struct PrepareHead
		{
			static void thunk(RE::BSFaceGenManager* a_manager, RE::BSFaceGenNiNode* a_node, RE::BGSHeadPart* a_part, RE::TESNPC* a_npc)
			{
				HeadUpdate update{ a_part, a_npc };
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
				std::lock_guard lock(mutex);
				Capture(a_property);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct CloneProperty
		{
			static RE::NiObject* thunk(RE::BSLightingShaderProperty* a_property, RE::NiCloningProcess& a_process)
			{
				auto* result = func(a_property, a_process);
				auto* clone = result ? netimmerse_cast<RE::BSLightingShaderProperty*>(result) : nullptr;
				std::lock_guard lock(mutex);
				const auto found = records.find(a_property);
				if (clone && found != records.end()) {
					auto copy = found->second;
					copy.property = RE::NiPointer<RE::BSShaderProperty>(clone);
					copy.revision = ++revision;
					records.insert_or_assign(clone, std::move(copy));
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	std::array<std::string, 9> Paths(RE::BSTextureSet* a_textureSet)
	{
		std::array<std::string, 9> result;
		if (a_textureSet) {
			if (auto* form = skyrim_cast<RE::BGSTextureSet*>(a_textureSet)) {
				constexpr std::array<size_t, 8> textureSlots{ 0, 1, 3, 4, 5, 2, 6, 7 };
				for (size_t i = 0; i < textureSlots.size(); ++i) {
					const auto* name = form->textures[textureSlots[i]].textureName.c_str();
					if (!name || !*name)
						continue;
					std::string path(name);
					std::replace(path.begin(), path.end(), '\\', '/');
					std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (path.starts_with("data/"))
						path.erase(0, 5);
					result[i] = path.starts_with("textures/") ? path : "textures/" + path;
				}
				return result;
			}
			for (size_t i = 0; i < result.size(); ++i) {
				const char* path = a_textureSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
				result[i] = path ? path : "";
			}
		}
		return result;
	}

	void Install()
	{
		stl::write_vfunc<0x1E, PostLink>(RE::VTABLE_BSLightingShaderProperty[0]);
		stl::write_vfunc<0x17, CloneProperty>(RE::VTABLE_BSLightingShaderProperty[0]);
		if (REL::Module::IsVR()) {
			logger::warn("[Advanced Skin] TXST and player source tracking are unavailable on VR.");
			return;
		}
		stl::detour_thunk<ConvertTextureSet>(REL::RelocationID(20905, 21361));
		stl::detour_thunk<PrepareHead>(REL::RelocationID(26259, 26838));
		stl::detour_thunk<UpdateHeadNormal>(REL::RelocationID(26260, 26839));
		stl::write_vfunc<0x27, SetTexturePath>(RE::VTABLE_BSShaderTextureSet[0]);
		stl::write_vfunc<0x3A, PrecacheTextures>(RE::VTABLE_BSLightingShaderProperty[0]);
	}

	void Prune()
	{
		std::lock_guard lock(mutex);
		std::erase_if(records, [](const auto& entry) { return entry.second.property->GetRefCount() == 1; });
		std::erase_if(conversions, [](const auto& entry) { return entry.second.set->GetRefCount() == 1; });
	}

	Snapshot Get(RE::BSShaderProperty* a_property)
	{
		std::lock_guard lock(mutex);
		const auto found = records.find(a_property);
		if (found == records.end() || !SkinMaterials::IsCompatible(a_property))
			return {};
		auto& record = found->second;
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material);
		const bool unchanged = record.set == material->textureSet && record.normal == material->normalTexture;
		const auto txst = unchanged && record.headPart ? record.txst : TextureRecord(material->textureSet.get());
		if (txst != record.txst || !unchanged) {
			record.txst = txst;
			record.set = material->textureSet;
			record.normal = material->normalTexture;
			record.revision = ++revision;
			if (!unchanged) {
				record.headPart = 0;
				record.npc = 0;
			}
		}
		return { record.asset, record.txst, record.headPart, record.npc, record.revision };
	}

	SkinMaterials::Material AssignedMaterial(const Snapshot& a_source)
	{
		auto material = a_source.asset ? a_source.asset->definition.material : SkinMaterials::Material{};
		if (!material.enabled || !a_source.txst)
			return material;
		auto* record = RE::TESForm::LookupByID<RE::BGSTextureSet>(a_source.txst);
		if (!record)
			return material;
		for (size_t i = 0; i < 2; ++i) {
			const auto path = Paths(record)[SkinMaterials::TextureSlots[i]];
			if (!path.empty())
				material.textures[i] = path;
		}
		return material;
	}
}
