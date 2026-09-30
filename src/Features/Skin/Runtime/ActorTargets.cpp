#include "ActorTargets.h"

#include "../Editor/NifDocument.h"

#include <cmath>

namespace SkinActors
{
	namespace
	{
		bool Within(RE::NiAVObject* object, const RE::NiAVObject* root)
		{
			if (!root)
				return false;
			for (auto* current = object; current; current = current->parent)
				if (current == root)
					return true;
			return false;
		}
		std::string Layout(RE::BSGeometry* geometry)
		{
			auto* shape = geometry->AsTriShape();
			if (!shape)
				return {};
			std::vector<uint8_t> data;
			const auto append = [&](const void* pointer, size_t size) {
				const auto* bytes = static_cast<const uint8_t*>(pointer);
				if (size > 2 * 1024 * 1024 - data.size())
					throw std::runtime_error(T("feature.skin.layout_too_large", "The surface layout exceeds the supported size."));
				data.insert(data.end(), bytes, bytes + size);
			};
			const auto vertices = [&](RE::BSGraphics::TriShape* buffer, uint32_t count) {
				if (!buffer || !buffer->rawVertexData || !count || count > 65535 ||
					!buffer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV))
					return false;
				uint64_t description;
				std::memcpy(&description, &buffer->vertexDesc, sizeof(description));
				const auto stride = static_cast<uint32_t>((description & 15) * 4);
				const auto offset = buffer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
				if (!stride || offset + 4 > stride)
					return false;
				append(&count, sizeof(count));
				for (uint32_t i = 0; i < count; ++i)
					append(buffer->rawVertexData + static_cast<size_t>(i) * stride + offset, 4);
				return true;
			};
			const auto& runtime = geometry->GetGeometryRuntimeData();
			const auto& counts = shape->GetTrishapeRuntimeData();
			if (auto* buffer = runtime.rendererData; buffer && buffer->rawIndexData && vertices(buffer, counts.vertexCount)) {
				append(&counts.triangleCount, sizeof(counts.triangleCount));
				append(buffer->rawIndexData, static_cast<size_t>(counts.triangleCount) * 3 * sizeof(uint16_t));
			} else if (runtime.skinInstance && runtime.skinInstance->skinPartition) {
				data.clear();
				const auto& skin = *runtime.skinInstance->skinPartition;
				if (!skin.numPartitions || skin.numPartitions > 256)
					return {};
				for (uint32_t i = 0; i < skin.numPartitions; ++i) {
					const auto& partition = skin.partitions[i];
					if (partition.strips || !partition.buffData || !partition.buffData->rawIndexData || !vertices(partition.buffData, skin.vertexCount))
						return {};
					append(&partition.triangles, sizeof(partition.triangles));
					append(partition.buffData->rawIndexData, static_cast<size_t>(partition.triangles) * 3 * sizeof(uint16_t));
				}
			} else {
				return {};
			}
			const auto* property = runtime.shaderProperty.get();
			const uint8_t modelSpace = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals);
			append(&modelSpace, sizeof(modelSpace));
			std::string result;
			result.reserve(data.size() * 2);
			constexpr char digits[] = "0123456789abcdef";
			for (const auto byte : data) {
				result.push_back(digits[byte >> 4]);
				result.push_back(digits[byte & 15]);
			}
			return result;
		}
	}

	bool Belongs(RE::BSGeometry* geometry, RE::Actor* actor)
	{
		return geometry && actor && (Within(geometry, actor->Get3D(false)) || (actor == RE::PlayerCharacter::GetSingleton() && Within(geometry, actor->Get3D(true))));
	}
	bool Current(const Target& target, RE::BSGeometry* geometry)
	{
		const auto actor = target.actor.get();
		if (!Belongs(geometry, actor.get()))
			return false;
		if (target.part == Part::Face || target.part >= Part::Count)
			return true;
		const auto& biped = actor->GetBiped(target.guard.firstPerson);
		if (!biped)
			return false;
		constexpr std::array<uint32_t, 3> slots{ RE::BIPED_OBJECTS::kBody, RE::BIPED_OBJECTS::kHands, RE::BIPED_OBJECTS::kFeet };
		const auto& object = biped->objects[slots[static_cast<size_t>(target.part) - 1]];
		return object.item && object.addon && object.item->GetFormID() == target.armor &&
		       object.addon->GetFormID() == target.addon && Within(geometry, object.partClone.get());
	}
	RE::Actor* Owner(RE::BSGeometry* geometry)
	{
		for (auto* object = static_cast<RE::NiAVObject*>(geometry); object; object = object->parent)
			if (auto* owner = object->GetUserData())
				if (auto* actor = owner->As<RE::Actor>(); Belongs(geometry, actor))
					return actor;
		auto* player = RE::PlayerCharacter::GetSingleton();
		return Belongs(geometry, player) ? player : nullptr;
	}

	Target Describe(RE::BSGeometry* a_geometry, const SkinSources::Snapshot& a_source,
		const std::function<uint64_t(const std::string&)>& a_resourceRevision, RE::Actor* a_actor)
	{
		Target target;
		auto* actor = a_actor ? a_actor : Owner(a_geometry);
		if (!Belongs(a_geometry, actor))
			return target;
		auto* third = actor->Get3D(false);
		auto* first = actor == RE::PlayerCharacter::GetSingleton() ? actor->Get3D(true) : nullptr;
		target.guard.firstPerson = first && first != third && Within(a_geometry, first);
		target.character = true;
		target.actor = actor->GetHandle();
		target.key = ActorKey(actor);
		target.name = actor->GetName() ? actor->GetName() : "";
		target.persistent = target.key == "player" || PersistentKey(target.key);
		try {
			if (REL::Module::IsVR())
				throw std::runtime_error(T("feature.skin.character_targets_unverified_vr", "Character targets are not verified on VR."));
			if (!a_source.asset || a_source.asset->block == UINT32_MAX)
				throw std::runtime_error(T("feature.skin.source_material_is_unknown_preview_only", "Source material is unknown; preview only."));
			auto* npc = actor->GetActorBase();
			auto* race = actor->GetRace();
			if (!npc || !race)
				throw std::runtime_error(T("feature.skin.character_appearance_not_ready", "Character appearance is not ready."));
			target.guard.race = FormKey(race);
			target.guard.sex = static_cast<uint32_t>(npc->GetSex());
			target.guard.materialBlock = a_source.asset->block;
			target.guard.source = a_source.asset->path;
			bool located = false;
			if (a_source.headPart && a_source.npc == npc->GetFormID()) {
				auto* part = RE::TESForm::LookupByID<RE::BGSHeadPart>(a_source.headPart);
				if (part && part->type == RE::BGSHeadPart::HeadPartType::kFace) {
					target.part = Part::Face;
					target.guard.headPart = FormKey(part);
					located = true;
				}
			}
			if (!located) {
				const auto& biped = actor->GetBiped(target.guard.firstPerson);
				if (biped) {
					constexpr std::array<uint32_t, 3> slots{ RE::BIPED_OBJECTS::kBody, RE::BIPED_OBJECTS::kHands, RE::BIPED_OBJECTS::kFeet };
					for (size_t i = 0; i < slots.size(); ++i) {
						const auto& object = biped->objects[slots[i]];
						if (!object.item || !object.addon || !Within(a_geometry, object.partClone.get()))
							continue;
						target.part = static_cast<Part>(i + 1);
						target.armor = object.item->GetFormID();
						target.addon = object.addon->GetFormID();
						target.guard.armor = FormKey(object.item);
						target.guard.addon = FormKey(object.addon);
						located = true;
						break;
					}
				}
			}
			if (target.guard.race.empty() || (located && (target.part == Part::Face ? target.guard.headPart.empty() : target.guard.armor.empty() || target.guard.addon.empty())))
				throw std::runtime_error(T("feature.skin.unstable_surface_records", "This surface uses records without a stable plugin identity; preview only."));
			if (!located)
				throw std::runtime_error(T("feature.skin.this_surface_has_no_verified_face_or_body_part_assignment", "This surface has no verified face or body-part assignment; preview only."));
			target.guard.layout = Layout(a_geometry);
			if (target.guard.layout.empty())
				throw std::runtime_error(T("feature.skin.uv_and_topology_data_are_unavailable_preview_only", "UV and topology data are unavailable; preview only."));
			auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!SkinMaterials::IsCompatible(property))
				throw std::runtime_error(T("feature.skin.incompatible_material_configuration", "Incompatible material configuration."));
			auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
			auto paths = a_source.asset->textures;
			if (auto* txst = RE::TESForm::LookupByID<RE::BGSTextureSet>(a_source.txst))
				paths = SkinSources::Paths(txst);
			if (material->normalTexture && material->normalTexture->name.c_str() && *material->normalTexture->name.c_str())
				paths[1] = material->normalTexture->name.c_str();
			if (target.part != Part::Face && material->diffuseTexture && material->diffuseTexture->name.c_str() && *material->diffuseTexture->name.c_str())
				paths[0] = material->diffuseTexture->name.c_str();
			for (size_t i = 0; i < target.guard.baseTextures.size(); ++i) {
				target.guard.baseTextures[i] = SkinMaterials::NormalizeTexturePath(paths[i]);
				target.guard.resourceRevisions[i] = a_resourceRevision(target.guard.baseTextures[i]);
			}
			target.guard.uvTransform = { material->texCoordOffset[0].x, material->texCoordOffset[0].y,
				material->texCoordScale[0].x, material->texCoordScale[0].y };
			if (std::any_of(target.guard.uvTransform.begin(), target.guard.uvTransform.end(), [](float value) { return !std::isfinite(value); }))
				throw std::runtime_error(T("feature.skin.invalid_material_uv_transform", "Invalid material UV transform."));
		} catch (const std::exception& e) {
			target.guard.resolved = false;
			target.error = e.what();
		}
		return target;
	}
}
