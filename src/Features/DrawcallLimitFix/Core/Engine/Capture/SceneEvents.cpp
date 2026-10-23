// The scene's event hooks (installed on the render thread at PostPostLoad): detours on engine writers, and the subscriptions to
// SceneWriterFramework's patched stores (SceneWriter.h), each pushing the mirror's update and the scene pass's event on the writer's
// thread (SceneEvents.h).

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValueMoves.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Diagnostics/FadeProbe.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"

namespace DCLF::Scene
{
	struct FadeOnVisible
	{
		static void thunk(RE::BSFadeNode* a_this, RE::NiCullingProcess* a_process, std::int32_t a_alphaGroup)
		{
			const float before = CurrentFade(a_this);
			const std::uint8_t door = a_this->GetRuntimeData().unk154;
			func(a_this, a_process, a_alphaGroup);
			// The values for the mirror.
			if (CurrentFade(a_this) != before || a_this->GetRuntimeData().unk154 != door)
				PushNodeUpdate(a_this, SceneCapture::NodeRecord::kFadeCurrent | SceneCapture::NodeRecord::kFadeDoor);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct FadeUpdate
	{
		static std::uint64_t thunk(RE::BSFadeNode* a_this, float a_fadeAmount, void* a_camera)
		{
			const float before = CurrentFade(a_this);
			const std::uint8_t door = a_this->GetRuntimeData().unk154;
			const auto result = func(a_this, a_fadeAmount, a_camera);
			Diagnostics::FadeProbe::NoteFade(before, CurrentFade(a_this), a_this->GetRuntimeData().unk154);  // TEMP
			if (CurrentFade(a_this) != before || a_this->GetRuntimeData().unk154 != door)
				PushNodeUpdate(a_this, SceneCapture::NodeRecord::kFadeCurrent | SceneCapture::NodeRecord::kFadeDoor);
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Actor::SetAlpha's fadeAmount writes are Features/Actors' (its early hooks; the fade amounts channel's push, Features/Fades).

	/**
	 * @brief FUN_14147a430 (node, level): the only writer of the fade node's LOD level (+0x152 & 0xF, the skin partitions' row);
	 * called by the fade update (FUN_14147a160), BSLeafAnimNode::OnVisible and the feedback's fade service.
	 */
	struct LodLevelUpdate
	{
		static void thunk(RE::BSFadeNode* a_this, float a_level)
		{
			const std::uint8_t before = a_this->GetRuntimeData().unk152 & 0xF;
			func(a_this, a_level);
			if ((a_this->GetRuntimeData().unk152 & 0xF) != before)
				PushNodeUpdate(a_this, SceneCapture::NodeRecord::kFadeLevel);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Sky::SetColor (0x14040d970): a weather-blend colour written in place; external emittance may point at it.
	struct SkySetColor
	{
		static void thunk(RE::Sky* a_sky, RE::NiColor* a_colour, void* a_blend, float a_flash)
		{
			const RE::NiColor before = a_colour ? *a_colour : RE::NiColor{};
			func(a_sky, a_colour, a_blend, a_flash);
			if (a_colour && std::memcmp(&before, a_colour, sizeof(before)) != 0)
				emittanceEvents.Push(a_colour);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_141480fc0 (object, &colour): points the external emittance of the Lighting and material port properties under the object
	// at a colour. The colour is a classification input (ListDependents), so a re-point is a property event.
	struct SetExternalEmittance
	{
		static std::uint64_t thunk(RE::NiAVObject* a_object, RE::NiColor** a_colour)
		{
			auto* geometry = a_object ? a_object->AsGeometry() : nullptr;
			auto* shaderProperty = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			auto* property = shaderProperty ? netimmerse_cast<RE::BSLightingShaderProperty*>(shaderProperty) : nullptr;
			// A port property's emissive (the Effect port's emittanceColor, its PropertyColor: the mirror's emissive): a re-point is an
			// event too. The port's (its exact RTTI, as the capture's), read through its emissiveOf.
			const std::uint8_t port = shaderProperty && !property ? Extensions::PortOfRtti(shaderProperty->GetRTTI()) : std::uint8_t{ 0 };
			const auto emissiveOf = port ? Extensions::MaterialPorts()[port].emissiveOf : nullptr;
			const void* before = property ? property->emissiveColor : emissiveOf ? emissiveOf(*shaderProperty) : nullptr;
			const auto result = func(a_object, a_colour);
			if (property && property->emissiveColor != before) {
				PushProperty(property);
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kEmissive);
			} else if (emissiveOf && emissiveOf(*shaderProperty) != before) {
				PushProperty(shaderProperty);
				PushPropertyUpdate(shaderProperty, SceneCapture::PropertyRecord::kEmissive);
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_1402d1280 (cell, reference) and FUN_1402d5090 (cell, reference, flag): a reference's 3D placed in its cell. They write
	// the LOD type inline (`and`/`or` at +0x153: 10 if flagged, 6 if distant) around the range (FUN_14021f200) and snap only on
	// one branch (save-load and sky cell skip it): the statics after the call.
	inline void PushReferenceFadeStatics(RE::TESObjectREFR* a_reference)
	{
		auto* root = a_reference ? a_reference->Get3D() : nullptr;
		if (auto* fade = root ? root->AsFadeNode() : nullptr) {
			PushNodeUpdate(fade, kFadeStatics | kFadeState);
			Diagnostics::Notify(Diagnostics::FadeStaticsPlaced{ fade });
		}
	}

	struct CellPlaceReference
	{
		static void thunk(RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_reference)
		{
			func(a_cell, a_reference);
			PushReferenceFadeStatics(a_reference);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct CellPlaceReferenceAt
	{
		static void* thunk(RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_reference, bool a_snap)
		{
			auto* result = func(a_cell, a_reference, a_snap);
			PushReferenceFadeStatics(a_reference);
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_140b44490 (rigid body, motion type, ...): the motion rebuilt in place (from hkpRigidBody::setMotionType, FUN_140b30700,
	// directly or deferred while the world is locked). Every motion type path ends here (SetMotionType, animation graph, ragdolls,
	// Havok). The owning node (TESHavokUtilities::FindCollidableObject) after the call.
	struct RigidBodyMotionType
	{
		static void thunk(RE::hkpRigidBody* a_body, std::uint32_t a_type, std::uint32_t a_arg2, std::uint32_t a_arg3)
		{
			func(a_body, a_type, a_arg2, a_arg3);
			if (a_body)
				PushNodeUpdate(RE::TESHavokUtilities::FindCollidableObject(*a_body->GetCollidable()), SceneCapture::NodeRecord::kBody);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// NiObjectNET::RemoveController (0x140d269a0): PrependController's other half.
	struct RemoveController
	{
		static void thunk(RE::NiObjectNET* a_target, RE::NiTimeController* a_controller)
		{
			func(a_target, a_controller);
			if (!a_target)
				return;
			if (auto* object = netimmerse_cast<RE::NiAVObject*>(a_target))
				PushNodeUpdate(object, SceneCapture::NodeRecord::kControllers);
			else
				PushPropertyUpdate(netimmerse_cast<RE::BSShaderProperty*>(a_target), SceneCapture::PropertyRecord::kControllers);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// BSDismemberSkinInstance::UpdateDismemberPartion (0x14021a530): a partition's editorVisible, its only writer after load. The
	// skin instance knows no geometry: the mirror finds it by the skin.
	inline void PushSkinShown(const RE::BSDismemberSkinInstance* a_skin)
	{
		if (!a_skin)
			return;
		SceneCapture::GeometryRecord record;
		record.skin = a_skin;
		const auto& data = a_skin->GetRuntimeData();
		if (data.partitions)
			for (std::int32_t i = 0; i < data.numPartitions; ++i)
				record.shown.push_back(data.partitions[i].editorVisible ? 1 : 0);
		SceneTracker::Get().PushUpdate(SceneCapture::Update{ SceneCapture::GeometryRecord::kDismember, std::move(record) });
	}

	struct DismemberPartition
	{
		static void thunk(RE::BSDismemberSkinInstance* a_this, std::uint16_t a_slot, bool a_visible)
		{
			func(a_this, a_slot, a_visible);
			PushSkinShown(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_140218200 (biped, ?, skin, slot, flag): an armor's partitions shown for a biped slot, stored inline (job thread, the
	// actor's 3D update). The skin's partitions after the call.
	struct BipedDismemberPartitions
	{
		static void thunk(void* a_biped, void* a_object, RE::BSDismemberSkinInstance* a_skin, std::int32_t a_slot, bool a_flag)
		{
			func(a_biped, a_object, a_skin, a_slot, a_flag);
			PushSkinShown(a_skin);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_1402ad800 (land): a TESObjectLAND's landscape properties stored into its four quads' geometry (+0x128) after their
	// attach. Each quad's leaf after the call (a swap, with the new property's record).
	struct LandSetupProperties
	{
		static std::uint64_t thunk(RE::TESObjectLAND* a_land)
		{
			const auto result = func(a_land);
			const auto* loaded = a_land ? Engine::At<const RE::NiNode* const*>(a_land, 0x40) : nullptr;
			if (!loaded)
				return result;
			for (std::uint32_t quad = 0; quad < 4; ++quad) {
				const auto* node = loaded[quad];
				if (!node || node->GetChildren().empty() || !node->GetChildren()[0])
					continue;
				if (auto* geometry = node->GetChildren()[0]->AsGeometry())
					PushLeafUpdate(*geometry);
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_1402ad0e0 (land): after the cell's attach, each quad's landscape material takes LOD blend textures (+0xF8, +0x100) and
	// landBlendParams (+0x108). Each quad's material (a write: its record takes the LOD blend textures, MTLandLODBlend's t13 and t15,
	// which it otherwise holds null from the attach's capture) and property after the call.
	struct LandBlendParams
	{
		static void thunk(RE::TESObjectLAND* a_land)
		{
			func(a_land);
			const auto* loaded = a_land ? Engine::At<const RE::NiNode* const*>(a_land, 0x40) : nullptr;
			if (!loaded)
				return;
			for (std::uint32_t quad = 0; quad < 4; ++quad) {
				const auto* node = loaded[quad];
				if (!node || node->GetChildren().empty() || !node->GetChildren()[0])
					continue;
				if (auto* geometry = node->GetChildren()[0]->AsGeometry()) {
					const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
					if (property)
						MaterialWrites::NoteWritten(property->material);
					PushPropertyUpdate(property, SceneCapture::PropertyRecord::kLandBlend);
				}
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_1414abd10 (object, visitor {params, colour, snow}): projected UV on every Lighting or grass property under the object
	// (recursing through the entry): kProjectedUV and kSnow via SetFlags, projectedUVParams and projectedUVColor inline. The
	// property after the call.
	struct PropertiesProjectedUV
	{
		static std::uint64_t thunk(RE::NiAVObject* a_object, void* a_visitor)
		{
			const auto result = func(a_object, a_visitor);
			if (auto* geometry = a_object ? a_object->AsGeometry() : nullptr)
				PushPropertyUpdate(geometry->GetGeometryRuntimeData().shaderProperty.get(), SceneCapture::PropertyRecord::kProjected);
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace DCLF::Extensions
{
	// A contributed node's list edit, from the feature's own hooks (Features/Decals' decal array edits): Core/Extensions/NodeCaptures.h.
	void PushNodeUpdate(const RE::NiAVObject* a_node, std::uint32_t a_fields)
	{
		Scene::PushNodeUpdate(a_node, a_fields);
	}
}

namespace DCLF::Scene
{
	// BSGeometry::AttachProperty (0x140267cf0, geometry, alpha): the alpha property (+0x120), set after attach (object LOD's
	// blocks). The leaf after the call.
	struct GeometryAttachAlpha
	{
		static void thunk(RE::BSGeometry* a_geometry, RE::NiAlphaProperty* a_alpha)
		{
			func(a_geometry, a_alpha);
			if (a_geometry)
				PushLeafUpdate(*a_geometry);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_1414ab770 (object, visitor {predicate, threshold}): the alpha test threshold (+0x32) of admitted geometries under the
	// object, inline: an actor's or effect's fade (job threads, every frame of it). Per geometry after the call.
	struct AlphasSetThreshold
	{
		static std::uint64_t thunk(RE::NiAVObject* a_object, void* a_visitor)
		{
			const auto result = func(a_object, a_visitor);
			if (auto* geometry = a_object ? a_object->AsGeometry() : nullptr)
				PushAlphaUpdate(geometry->GetGeometryRuntimeData().alphaProperty.get(), SceneCapture::AlphaRecord::kThreshold);
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_1414ab580 (object, visitor {flag, hide}): weapon blood on flagged geometries under the object: hidden bit, kWeaponBlood
	// (SetFlags), alpha blend bits cleared inline (& 0xFE01). Alpha flags and hidden bit after the call, per geometry.
	struct WeaponBloodGeometry
	{
		static std::uint64_t thunk(RE::NiAVObject* a_object, void* a_visitor)
		{
			const auto result = func(a_object, a_visitor);
			if (auto* geometry = a_object ? a_object->AsGeometry() : nullptr) {
				PushAlphaUpdate(geometry->GetGeometryRuntimeData().alphaProperty.get(), SceneCapture::AlphaRecord::kFlags);
				PushNodeUpdate(geometry, SceneCapture::NodeRecord::kHidden);
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_140e880e0 (visit): FUN_140e874d0's walk callback releasing a node's collision object (+0x40) as an equipped item loses
	// physics; the node is at visit +0x10. The body after the call.
	struct ReleaseCollisionObject
	{
		static void thunk(std::byte* a_visit)
		{
			auto* node = a_visit ? *reinterpret_cast<RE::NiAVObject**>(a_visit + 0x10) : nullptr;
			func(a_visit);
			PushNodeUpdate(node, SceneCapture::NodeRecord::kBody);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_140e8bd40 (node, collision object): NiAVObject::SetCollisionObject (+0x40), from bhkWorld::InitHavok and FUN_140e8c600
	// after the world attach (a loader's clutter gets its body after the capture). The body after the call.
	struct SetCollisionObject
	{
		static void thunk(RE::NiAVObject* a_node, RE::NiCollisionObject* a_collision)
		{
			func(a_node, a_collision);
			PushNodeUpdate(a_node, SceneCapture::NodeRecord::kBody);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_14147c1e0 (object, flag, set, Lighting only): one shader flag on every property under the object, inline (not SetFlags):
	// AIProcess::Update3DModel_Impl's kCharacterLighting, the sky, explosions. Per geometry after the call.
	struct PropertiesSetFlag
	{
		static std::uint64_t thunk(RE::NiAVObject* a_object, std::uint32_t a_flag, bool a_set, bool a_lightingOnly)
		{
			const auto result = func(a_object, a_flag, a_set, a_lightingOnly);
			if (auto* geometry = a_object ? a_object->AsGeometry() : nullptr)
				PushPropertyUpdate(geometry->GetGeometryRuntimeData().shaderProperty.get(),
					SceneCapture::PropertyRecord::kFlags | SceneCapture::PropertyRecord::kMaterialOther);
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_14147c690 (&fadeNode, object): the fade node (+0x60) of every shader property under the object, from FUN_14147bf70. Per
	// geometry after the call.
	struct PropertiesSetFadeNode
	{
		static void thunk(RE::BSFadeNode** a_fadeNode, RE::NiAVObject* a_object)
		{
			func(a_fadeNode, a_object);
			// A leaf update, so the entry holds the new fade node its tree and fade-root owners copy from (HoldProperties); a property
			// update carries no reference, and an entry attached before the fade node was set would leave its root unlisted.
			if (auto* geometry = a_object ? a_object->AsGeometry() : nullptr)
				PushLeafUpdate(*geometry);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_14147a9b0 (node, near, far): the fade range's only writer (+0x128, +0x12C; near clamped to a minimum, far to twice it),
	// from the attach (FUN_14021f200, from the radius) and an FX path (FUN_1407cfba0).
	struct FadeSetRange
	{
		static void thunk(RE::BSFadeNode* a_this, float a_near, float a_far)
		{
			func(a_this, a_near, a_far);
			PushNodeUpdate(a_this, SceneCapture::NodeRecord::kFadeNear | SceneCapture::NodeRecord::kFadeFar);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_14147aa00 (node, type): the fade node's LOD type (+0x153 & 0xF), from the model processor and the reference 3D paths.
	struct FadeSetLodType
	{
		static void thunk(RE::BSFadeNode* a_this, std::uint8_t a_type)
		{
			func(a_this, a_type);
			PushNodeUpdate(a_this, SceneCapture::NodeRecord::kFadeType);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct LodFadeRenderPasses
	{
		static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* a_property, RE::BSGeometry* a_geometry, std::uint32_t a_renderFlags,
			RE::BSShaderAccumulator* a_accumulator)
		{
			const float specular = a_property->specularLODFade;
			const float envmap = a_property->envmapLODFade;
			const float alpha = a_property->alpha;
			auto* passes = func(a_property, a_geometry, a_renderFlags, a_accumulator);
			// The LOD fades and the alpha left on the property (materialAlpha times the fade, for the last registering camera),
			// which non-member records shade with (MakeShading).
			if (std::bit_cast<std::uint32_t>(a_property->specularLODFade) != std::bit_cast<std::uint32_t>(specular) ||
				std::bit_cast<std::uint32_t>(a_property->envmapLODFade) != std::bit_cast<std::uint32_t>(envmap) ||
				std::bit_cast<std::uint32_t>(a_property->alpha) != std::bit_cast<std::uint32_t>(alpha))
				lodFadeEvents.Push(a_property);
			// The alpha's value for the mirror.
			if (std::bit_cast<std::uint32_t>(a_property->alpha) != std::bit_cast<std::uint32_t>(alpha))
				PushPropertyUpdate(a_property, SceneCapture::PropertyRecord::kAlphaValue);
			return passes;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PropertySetFlags
	{
		static void thunk(RE::BSShaderProperty* a_this, RE::BSShaderProperty::EShaderPropertyFlag8 a_flag, bool a_set)
		{
			const auto before = a_this->flags.underlying();
			func(a_this, a_flag, a_set);
			if (a_this->flags.underlying() != before) {
				PushProperty(a_this);
				// The glints follow kVertexLighting (CaptureProperty).
				PushPropertyUpdate(a_this, SceneCapture::PropertyRecord::kFlags | SceneCapture::PropertyRecord::kMaterialOther);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PropertySetMaterial
	{
		static void thunk(RE::BSShaderProperty* a_this, RE::BSShaderMaterial* a_material, bool a_unique)
		{
			const auto* before = a_this->material;
			func(a_this, a_material, a_unique);
			if (a_this->material != before) {
				PushProperty(a_this);
				PushPropertyUpdate(a_this, SceneCapture::PropertyRecord::kMaterial | SceneCapture::PropertyRecord::kMaterialAlpha |
				                               SceneCapture::PropertyRecord::kMaterialOther);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// BSLightingShaderProperty::SetMaterialAlpha (vtable slot 0x31).
	struct PropertySetMaterialAlpha
	{
		static void thunk(RE::BSLightingShaderProperty* a_this, float a_alpha)
		{
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(a_this->material);
			const float before = material ? material->materialAlpha : 0.0f;
			func(a_this, a_alpha);
			material = static_cast<const RE::BSLightingShaderMaterialBase*>(a_this->material);
			if (material && std::bit_cast<std::uint32_t>(material->materialAlpha) != std::bit_cast<std::uint32_t>(before)) {
				PushProperty(a_this);
				PushPropertyUpdate(a_this, SceneCapture::PropertyRecord::kMaterialAlpha);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PrependController
	{
		static void thunk(RE::NiObjectNET* a_target, RE::NiTimeController* a_controller)
		{
			func(a_target, a_controller);
			if (!a_target)
				return;
			if (auto* object = netimmerse_cast<RE::NiAVObject*>(a_target)) {
				PushNode(object);
				FrameValueMoves::PushNode(object);
				PushNodeUpdate(object, SceneCapture::NodeRecord::kControllers);
			} else {
				PushProperty(a_target);
				PushPropertyUpdate(netimmerse_cast<RE::BSShaderProperty*>(a_target), SceneCapture::PropertyRecord::kControllers);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct HavokNodeTransform
	{
		static void thunk(RE::NiCollisionObject* a_this, const void* a_transform)
		{
			func(a_this, a_transform);
			PushNode(a_this->sceneObject);
			FrameValueMoves::PushNode(a_this->sceneObject);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// MoveEvents' writers (SceneEvents.h), each pushing its key to the scene pass and to FrameValues' producer (FrameValueMoves).
	inline void PushMoved(const void* a_key)
	{
		PushMove(a_key);
		FrameValueMoves::PushKey(a_key);
	}

	struct AnimationGraphPlace
	{
		static bool thunk(RE::TESObjectREFR* a_reference)
		{
			const bool result = func(a_reference);
			PushMoved(a_reference);
			CountMove(0);
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ReferenceUpdate3DPosition
	{
		static void thunk(RE::TESObjectREFR* a_reference, bool a_warp)
		{
			func(a_reference, a_warp);
			PushMoved(a_reference);
			CountMove(1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Slots 2-4 (actor 3D position, sky cell skin, ragdoll) are Features/Actors' reserved writers (its early hooks).

	struct GraphAnimation
	{
		static void thunk(RE::TESObjectREFR* a_reference)
		{
			func(a_reference);
			PushMoved(a_reference);
			CountMove(5);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief A patched `call [rax + 0x168]`: the node's UpdateSelectedDownwardPass, then its reference or itself as the key. */
	struct SelectiveUpdateCall
	{
		static void thunk(RE::NiAVObject* a_node, void* a_data, std::uint32_t a_arg)
		{
			using UpdateSelectedDownwardPass = void (*)(RE::NiAVObject*, void*, std::uint32_t);
			(*reinterpret_cast<UpdateSelectedDownwardPass* const*>(a_node))[0x2D](a_node, a_data, a_arg);
			if (const auto* reference = a_node->GetUserData())
				PushMoved(reference);
			else
				PushMoved(a_node);
			CountMove(6);
		}
	};

	struct SwitchAttachChild
	{
		static void thunk(RE::NiNode* a_this, RE::NiAVObject* a_child, bool a_firstAvail)
		{
			func(a_this, a_child, a_firstAvail);
			PushSwitchStructural(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct SwitchDetachChild1
	{
		static void thunk(RE::NiNode* a_this, RE::NiAVObject* a_child, RE::NiPointer<RE::NiAVObject>& a_out)
		{
			func(a_this, a_child, a_out);
			PushSwitchStructural(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct SwitchDetachChild2
	{
		static void thunk(RE::NiNode* a_this, RE::NiAVObject* a_child)
		{
			func(a_this, a_child);
			PushSwitchStructural(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct SwitchDetachChildAt1
	{
		static void thunk(RE::NiNode* a_this, std::uint32_t a_index, RE::NiPointer<RE::NiAVObject>& a_out)
		{
			func(a_this, a_index, a_out);
			PushSwitchStructural(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct SwitchDetachChildAt2
	{
		static void thunk(RE::NiNode* a_this, std::uint32_t a_index)
		{
			func(a_this, a_index);
			PushSwitchStructural(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct SwitchSetAt1
	{
		static void thunk(RE::NiNode* a_this, std::uint32_t a_index, RE::NiAVObject* a_child, RE::NiPointer<RE::NiAVObject>& a_out)
		{
			func(a_this, a_index, a_child, a_out);
			PushSwitchStructural(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct SwitchSetAt2
	{
		static void thunk(RE::NiNode* a_this, std::uint32_t a_index, RE::NiAVObject* a_child)
		{
			func(a_this, a_index, a_child);
			PushSwitchStructural(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	template <class T>
	void DetourSwitchSlot(std::size_t a_slot)
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_NiSwitchNode[0] };
		stl::detour_thunk<T>(reinterpret_cast<const std::uintptr_t*>(vtable.address())[a_slot]);
	}

	/**
	 * @brief The fade resets (SceneWriterFramework's SWF_EVENT_FADE_RESET, after SWF made the store): currentFade (+0x130) stored inline
	 * as 0 or 1.0, then flag bit 14 (a frame bit, not mirrored) set or cleared: cell placements (FUN_1402bc1f0, grid controller, main
	 * thread; FUN_1402bb690, a job, after FUN_1402d5090), the sky cell (FUN_1402b9da0), FUN_1402d1280's no-snap branches,
	 * Explosion::Load3D and every other store-then-bit-14 site in the image.
	 */
	void OnFadeReset(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
	{
		const auto& event = *reinterpret_cast<const SWF_FadeResetEvent*>(a_event);
		PushNodeUpdate(static_cast<RE::NiAVObject*>(event.node), SceneCapture::NodeRecord::kFadeCurrent);
	}

	/** @brief An engine store changed an NiSwitchNode's index (SWF_EVENT_SWITCH_INDEX, after SWF made the store). */
	void OnSwitchIndex(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
	{
		const auto& event = *reinterpret_cast<const SWF_SwitchIndexEvent*>(a_event);
		auto* node = static_cast<RE::NiAVObject*>(event.node);
		// The mirror's value on any thread (no reference taken); the tracking's event on the render thread alone.
		PushNodeUpdate(node, SceneCapture::NodeRecord::kSwitch);
		PushSwitch(node, event.before, false);
	}

	void PushHiddenUpdate(const void* a_object, std::uint32_t a_site)
	{
		// The mirror's bit and the hidden event in one update, so both land in the same batch.
		SceneCapture::Update update{ SceneCapture::NodeRecord::kHidden, SceneCapture::CaptureNodeFields(*static_cast<const RE::NiAVObject*>(a_object), SceneCapture::NodeRecord::kHidden) };
		update.hiddenSite = a_site;
		SceneTracker::Get().PushUpdate(std::move(update));
	}

	bool HiddenStoresLive()
	{
		return hiddenEventsInstalled;
	}

	void InstallMoveEvents()
	{
		// Both update passes patched at `call qword ptr [rax + 0x168]` (FF 90 68 01 00 00), checked first; either missing places
		// every mover every frame.
		constexpr std::uintptr_t kAnimatedRefCall = 0x2b42ba;  // FUN_1402b41a0: a cell's animated reference
		constexpr std::uintptr_t kDynamicNodeCall = 0x2b3bd5;  // FUN_1402b3ae0: the cell's dynamic node
		constexpr std::array<std::uint8_t, 6> kCall{ 0xFF, 0x90, 0x68, 0x01, 0x00, 0x00 };
		const auto animatedRefCall = REL::Offset(kAnimatedRefCall).address();
		const auto dynamicNodeCall = REL::Offset(kDynamicNodeCall).address();
		if (std::memcmp(reinterpret_cast<const void*>(animatedRefCall), kCall.data(), kCall.size()) != 0 ||
			std::memcmp(reinterpret_cast<const void*>(dynamicNodeCall), kCall.data(), kCall.size()) != 0) {
			logger::warn("[DCLF] move events: the cells' update passes are not where expected; every mover is placed every frame");
			return;
		}
		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_call<6>(animatedRefCall, SelectiveUpdateCall::thunk);
		trampoline.write_call<6>(dynamicNodeCall, SelectiveUpdateCall::thunk);
		constexpr std::uintptr_t kAnimationGraphPlace = 0x66afa0;     // FUN_14066afa0
		constexpr std::uintptr_t kReferenceUpdate3DPosition = 0x2d9800;  // TESObjectREFR::Update3DPosition
		constexpr std::uintptr_t kGraphAnimation = 0x2f75a0;          // FUN_1402f75a0
		stl::detour_thunk<AnimationGraphPlace>(REL::Offset(kAnimationGraphPlace).address());
		stl::detour_thunk<ReferenceUpdate3DPosition>(REL::Offset(kReferenceUpdate3DPosition).address());
		stl::detour_thunk<GraphAnimation>(REL::Offset(kGraphAnimation).address());
		moveEventsInstalled = true;
		// The actors' 3D position, sky cell skin and ragdoll writers follow at Features/Actors' early hooks (only when these are in);
		// the line names every writer, as before they moved.
		logger::info("[DCLF] move events installed (animation, 3D positions, ragdolls, the cells' update passes)");
	}

	void InstallSceneEvents()
	{
		static bool installed = false;
		if (installed)
			return;
		installed = true;
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSFadeNode[0] };
		const auto onVisible = reinterpret_cast<const std::uintptr_t*>(vtable.address())[0x34];
		stl::detour_thunk<FadeOnVisible>(onVisible);
		constexpr std::uintptr_t kPropertySetFlags = 0x147bee0;     // BSShaderProperty::SetFlags
		constexpr std::uintptr_t kPropertySetMaterial = 0x147bff0;  // BSShaderProperty::SetMaterial
		constexpr std::uintptr_t kPrependController = 0xd268d0;     // NiObjectNET::PrependController (FUN_140d268d0)
		constexpr std::uintptr_t kHavokNodeTransform = 0xea55a0;    // FUN_140ea55a0: a node's transform from its rigid body
		stl::detour_thunk<FadeUpdate>(REL::Offset(Engine::Address::kFadeUpdate).address());
		stl::detour_thunk<LodLevelUpdate>(REL::Offset(Engine::Address::kLodLevelUpdate).address());
		// FUN_14147aa20, a fade node's placement snap, is Features/Fades' (its early hooks; the fade snaps channel's source).
		// Actor::SetAlpha's slot (0xE1) is Features/Actors' (its early hooks); no other hook takes it.
		constexpr std::uintptr_t kFadeSetRange = 0x147a9b0;         // FUN_14147a9b0: a fade node's near and far
		constexpr std::uintptr_t kFadeSetLodType = 0x147aa00;       // FUN_14147aa00: a fade node's LOD type
		stl::detour_thunk<FadeSetRange>(REL::Offset(kFadeSetRange).address());
		stl::detour_thunk<FadeSetLodType>(REL::Offset(kFadeSetLodType).address());
		constexpr std::uintptr_t kRigidBodyMotionType = 0xb44490;  // FUN_140b44490: a rigid body's motion rebuilt (its type)
		stl::detour_thunk<RigidBodyMotionType>(REL::Offset(kRigidBodyMotionType).address());
		constexpr std::uintptr_t kRemoveController = 0xd269a0;     // NiObjectNET::RemoveController
		constexpr std::uintptr_t kDismemberPartition = 0x21a530;   // BSDismemberSkinInstance::UpdateDismemberPartion
		stl::detour_thunk<RemoveController>(REL::Offset(kRemoveController).address());
		stl::detour_thunk<DismemberPartition>(REL::Offset(kDismemberPartition).address());
		constexpr std::uintptr_t kBipedDismemberPartitions = 0x218200;  // FUN_140218200: a biped slot's dismember partitions
		stl::detour_thunk<BipedDismemberPartitions>(REL::Offset(kBipedDismemberPartitions).address());
		constexpr std::uintptr_t kGeometryAttachAlpha = 0x267cf0;  // BSGeometry::AttachProperty (the alpha)
		stl::detour_thunk<GeometryAttachAlpha>(REL::Offset(kGeometryAttachAlpha).address());
		constexpr std::uintptr_t kWeaponBloodGeometry = 0x14ab580;  // FUN_1414ab580: weapon blood under an object
		stl::detour_thunk<WeaponBloodGeometry>(REL::Offset(kWeaponBloodGeometry).address());
		constexpr std::uintptr_t kAlphasSetThreshold = 0x14ab770;  // FUN_1414ab770: the alpha thresholds under an object
		stl::detour_thunk<AlphasSetThreshold>(REL::Offset(kAlphasSetThreshold).address());
		constexpr std::uintptr_t kReleaseCollisionObject = 0xe880e0;  // FUN_140e880e0: a node's collision object released (a walk's visit)
		stl::detour_thunk<ReleaseCollisionObject>(REL::Offset(kReleaseCollisionObject).address());
		stl::detour_thunk<SetCollisionObject>(REL::Offset(0xe8bd40).address());  // FUN_140e8bd40: NiAVObject::SetCollisionObject
		constexpr std::uintptr_t kPropertiesSetFlag = 0x147c1e0;  // FUN_14147c1e0: a shader flag on the properties under an object
		stl::detour_thunk<PropertiesSetFlag>(REL::Offset(kPropertiesSetFlag).address());
		constexpr std::uintptr_t kPropertiesSetFadeNode = 0x147c690;  // FUN_14147c690: the properties' fade node under an object
		stl::detour_thunk<PropertiesSetFadeNode>(REL::Offset(kPropertiesSetFadeNode).address());
		constexpr std::uintptr_t kLandSetupProperties = 0x2ad800;  // FUN_1402ad800: a land's quads' landscape properties
		stl::detour_thunk<LandSetupProperties>(REL::Offset(kLandSetupProperties).address());
		constexpr std::uintptr_t kLandBlendParams = 0x2ad0e0;  // FUN_1402ad0e0: a land's quads' LOD blend
		stl::detour_thunk<LandBlendParams>(REL::Offset(kLandBlendParams).address());
		constexpr std::uintptr_t kPropertiesProjectedUV = 0x14abd10;  // FUN_1414abd10: projected UV on the properties under an object
		stl::detour_thunk<PropertiesProjectedUV>(REL::Offset(kPropertiesProjectedUV).address());
		// A decal node's array edits are Features/Decals' (its early hooks, after these).
		constexpr std::uintptr_t kCellPlaceReference = 0x2d1280;    // FUN_1402d1280: a reference's 3D placed in its cell
		constexpr std::uintptr_t kCellPlaceReferenceAt = 0x2d5090;  // FUN_1402d5090: the same, another path
		stl::detour_thunk<CellPlaceReference>(REL::Offset(kCellPlaceReference).address());
		stl::detour_thunk<CellPlaceReferenceAt>(REL::Offset(kCellPlaceReferenceAt).address());
		stl::detour_thunk<PropertySetFlags>(REL::Offset(kPropertySetFlags).address());
		stl::detour_thunk<PropertySetMaterial>(REL::Offset(kPropertySetMaterial).address());
		stl::detour_thunk<PrependController>(REL::Offset(kPrependController).address());
		stl::detour_thunk<HavokNodeTransform>(REL::Offset(kHavokNodeTransform).address());
		stl::write_vfunc<0x2A, LodFadeRenderPasses>(RE::VTABLE_BSLightingShaderProperty[0]);
		constexpr std::uintptr_t kSkySetColor = 0x40d970;            // Sky::SetColor
		constexpr std::uintptr_t kSetExternalEmittance = 0x1480fc0;  // FUN_141480fc0: external emittance, per property
		stl::detour_thunk<SkySetColor>(REL::Offset(kSkySetColor).address());
		stl::detour_thunk<SetExternalEmittance>(REL::Offset(kSetExternalEmittance).address());
		stl::write_vfunc<0x31, PropertySetMaterialAlpha>(RE::VTABLE_BSLightingShaderProperty[0]);
		lodFadeEventsInstalled = true;
		// Switch selections are followed by event alone (memberLive, ApplySwitchEvents), and the mirror's currentFade takes the cells'
		// inline resets: SceneWriterFramework's stores, whose families PostPostLoad required (SceneWriter::MissingReason) before any
		// hook was written.
		if (!SceneWriter::Subscribe(SWF_EVENT_SWITCH_INDEX, &OnSwitchIndex) || !SceneWriter::Subscribe(SWF_EVENT_FADE_RESET, &OnFadeReset))
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused the switch or fade-reset subscription after reporting them installed");
		{
			// NiSwitchNode's own child edits (NiNode vtable slots 0x35, 0x37-0x3C, as SceneTracker's).
			DetourSwitchSlot<SwitchAttachChild>(0x35);
			DetourSwitchSlot<SwitchDetachChild1>(0x37);
			DetourSwitchSlot<SwitchDetachChild2>(0x38);
			DetourSwitchSlot<SwitchDetachChildAt1>(0x39);
			DetourSwitchSlot<SwitchDetachChildAt2>(0x3A);
			DetourSwitchSlot<SwitchSetAt1>(0x3B);
			DetourSwitchSlot<SwitchSetAt2>(0x3C);
			switchEventsInstalled = true;
		}
		logger::info("[DCLF] scene events installed (fades, property flags, materials and material alpha, LOD fades, emittance, Havok node transforms, controllers): OnVisible at {:#x}",
			onVisible - REL::Module::get().base() + 0x140000000);
		InstallMoveEvents();
		// Actor::SetAlpha and the actors' reserved move writers are Features/Actors' (its early hooks, after the reflection's).
		// The hidden stores are Features/HiddenNodes' (its early hooks, right after these). Object LOD's segment writers are Features/Lod's
		// engine channel's source (its early hooks, before tree LOD's and the hidden nodes').
		// Tree LOD's channel's source is Features/TreeLod's (its early hooks, right after these).
	}
}

namespace DCLF::Extensions
{
	// The engine side's pushes for the features' engine hooks (Core/Extensions/EngineEvents.h), onto the scene's stack and Core's hidden,
	// fade amount and move channels.

	void PushStacked(Api::EngineChannelId a_channel, std::uint64_t a_token, std::span<RE::NiAVObject* const> a_pinned, std::span<RE::NiAVObject* const> a_named)
	{
		const auto channels = RegisteredEngineChannels();
		const bool wakes = a_channel.index < channels.size() && channels[a_channel.index].wake == Api::WakeSource::Pass;
		SceneTracker::Get().PushStacked(a_channel.index, a_token, a_pinned, a_named, wakes);
	}

	void WakeChannel(Api::EngineChannelId a_channel)
	{
		const auto channels = RegisteredEngineChannels();
		if (a_channel.index < channels.size() && channels[a_channel.index].wake == Api::WakeSource::Pass)
			WakeScenePass(SceneWake::Channel);
	}

	void PushReservedHidden(const RE::NiAVObject* a_object, std::uint32_t a_site)
	{
		// A feature's own stores as events, on its reserved site (past the source's).
		if (Scene::hiddenEventsInstalled && a_object)
			Scene::PushHiddenUpdate(a_object, a_site);
	}

	bool HiddenEventsLive()
	{
		return Scene::hiddenEventsInstalled;
	}

	void PushRangesUpdate(const RE::BSGeometry* a_geometry)
	{
		Scene::PushGeometryUpdate(a_geometry, SceneCapture::GeometryRecord::kRanges);
	}

	namespace
	{
		// The reserved writers' names, copied (Scene::moveWriterNames views them); written on the main thread at PostPostLoad.
		std::array<std::string, kMoveWriterSlots> reservedMoveWriterNames;
	}

	Api::MoveWriterId ReserveMoveWriter(std::string_view a_name)
	{
		if (a_name.empty())
			stl::report_and_fail("Drawcall Limit Fix: a move writer was reserved without a name");
		for (std::uint32_t slot = 0; slot < kMoveWriterSlots; ++slot) {
			if (!Scene::moveWriterNames[slot].empty())
				continue;
			reservedMoveWriterNames[slot] = a_name;
			Scene::moveWriterNames[slot] = reservedMoveWriterNames[slot];
			++Scene::moveWritersPending;
			return { slot };
		}
		stl::report_and_fail(fmt::format("Drawcall Limit Fix: the move writer {} is past Extensions::kMoveWriterSlots", a_name));
	}

	void MoveWriterInstalled(Api::MoveWriterId a_writer)
	{
		if (a_writer && Scene::moveWritersPending)
			--Scene::moveWritersPending;
	}

	bool CoreMoveWritersLive()
	{
		return Scene::moveEventsInstalled;
	}

	void PushReservedMove(Api::MoveWriterId a_writer, const void* a_key, Api::MoveReach a_reach)
	{
		Scene::PushMove(a_key);
		if (a_reach == Api::MoveReach::WithPlacements)
			FrameValueMoves::PushKey(a_key);
		Scene::CountMove(a_writer.index);
	}
}
