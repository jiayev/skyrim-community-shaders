#include "Internal.h"

#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Diagnostics/MirrorWatch.h"


namespace DCLF::Scene
{
	struct FadeOnVisible
	{
		static void thunk(RE::BSFadeNode* a_this, RE::NiCullingProcess* a_process, std::int32_t a_alphaGroup)
		{
			const float before = CurrentFade(a_this);
			const std::uint8_t door = a_this->GetRuntimeData().unk154;
			func(a_this, a_process, a_alphaGroup);
			if (CurrentFade(a_this) != before)
				PushFade(a_this);
			// The values for the mirror (T6b1a).
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
			if (CurrentFade(a_this) != before)
				PushFade(a_this);
			if (CurrentFade(a_this) != before || a_this->GetRuntimeData().unk154 != door)
				PushNodeUpdate(a_this, SceneCapture::NodeRecord::kFadeCurrent | SceneCapture::NodeRecord::kFadeDoor);
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief Actor::SetAlpha (vfunc 0xE1, 0x1406c1a50; Actor's, Character's and PlayerCharacter's tables): the fadeAmount (+0x100) of
	 * the actor's 3D root, and the player's first-person root too (fadeAmountEvents).
	 */
	template <class T>
	struct ActorSetAlpha
	{
		static void thunk(RE::Actor* a_this, float a_alpha)
		{
			RE::NiAVObject* roots[2]{ a_this->Get3D1(false), a_this->Get3D1(true) };
			float before[2]{};
			for (std::uint32_t i = 0; i < 2; ++i)
				before[i] = roots[i] ? Engine::At<float>(roots[i], 0x100) : 0.0f;
			func(a_this, a_alpha);
			for (std::uint32_t i = 0; i < 2; ++i)
				if (roots[i] && Engine::At<float>(roots[i], 0x100) != before[i] && (i == 0 || roots[1] != roots[0])) {
					fadeAmountEvents.Push(roots[i]);
					if (roots[i]->AsFadeNode())
						PushNodeUpdate(roots[i], SceneCapture::NodeRecord::kFadeAmount);  // the mirror's value (T6b1a)
				}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct ActorTable;
	struct CharacterTable;
	struct PlayerTable;

	/**
	 * @brief FUN_14147a430 (node, level): the LOD transition, the only writer of the fade node's LOD level (+0x152 & 0xF, the skin
	 * partitions' row). The fade update (FUN_14147a160), BSLeafAnimNode::OnVisible and the feedback's fade service call it.
	 */
	struct LodLevelUpdate
	{
		static void thunk(RE::BSFadeNode* a_this, float a_level)
		{
			const std::uint8_t before = a_this->GetRuntimeData().unk152 & 0xF;
			func(a_this, a_level);
			if ((a_this->GetRuntimeData().unk152 & 0xF) != before) {
				PushFade(a_this);
				PushNodeUpdate(a_this, SceneCapture::NodeRecord::kFadeLevel);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Sky::SetColor (0x14040d970): a colour from a weather blend, written in place. External emittance reads some of those
	// colours through its emissiveColor pointer (emittanceEvents).
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

	// FUN_141480fc0 (object, &colour): points the external emittance of the object's Lighting and effect properties at a
	// colour, recursively. The colour a Lighting property reads is a classification input (ListDependents lists the entry
	// under it); a re-point with no flag change would otherwise go unseen, so it is a property event.
	struct SetExternalEmittance
	{
		static std::uint64_t thunk(RE::NiAVObject* a_object, RE::NiColor** a_colour)
		{
			auto* geometry = a_object ? a_object->AsGeometry() : nullptr;
			auto* property = geometry ? netimmerse_cast<RE::BSLightingShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get()) : nullptr;
			const RE::NiColor* before = property ? property->emissiveColor : nullptr;
			const auto result = func(a_object, a_colour);
			if (property && property->emissiveColor != before) {
				PushProperty(property);
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kEmissive);
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_14147aa20: a fade node's placement snap (fadeSnapEvents). The cell attaches (FUN_1402d1280, FUN_1402d5090) call it after
	// their own writes of the node's fade statics (the LOD type's `and`/`or` at +0x153, the range through FUN_14021f200), so its
	// update carries them.
	struct FadeSnap
	{
		static std::uint64_t thunk(RE::NiAVObject* a_node, void* a_camera)
		{
			const auto result = func(a_node, a_camera);
			if (a_node) {
				fadeSnapEvents.Push(a_node);
				if (netimmerse_cast<RE::BSFadeNode*>(a_node))
					PushNodeUpdate(a_node, kFadeStatics | kFadeState);
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_1402d1280 (cell, reference) and FUN_1402d5090 (cell, reference, flag): a reference's 3D placed in its cell. They write the
	// fade node's LOD type inline (`and`/`or` at +0x153: 10 for a reference flagged so, 6 for a distant one) around the range
	// (FUN_14021f200) and snap it only on one branch (the save-load or sky cell branch skips the snap): the statics after the call.
	inline void PushReferenceFadeStatics(RE::TESObjectREFR* a_reference)
	{
		auto* root = a_reference ? a_reference->Get3D() : nullptr;
		if (auto* fade = root ? root->AsFadeNode() : nullptr) {
			PushNodeUpdate(fade, kFadeStatics | kFadeState);
			// CS_DCLF_MIRROR_WATCH: a placed node in the world, its statics just set, is watched for the next writer.
			if (MirrorWatch::Enabled() && SceneCapture::InWorld(fade))
				MirrorWatch::Arm(fade);
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

	// FUN_140b44490 (rigid body, motion type, ...): Havok's motion change, the motion rebuilt in place (hkpRigidBody::setMotionType,
	// FUN_140b30700, calls it directly, or as a world operation when the world is locked, which runs it later). Every path to a
	// body's motion type ends here: NiAVObject::SetMotionType's collision objects, the animation graph, ragdolls, Havok's own.
	// The node owning the body (TESHavokUtilities::FindCollidableObject) after the call.
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

	// NiObjectNET::RemoveController (0x140d269a0): the controllers a node or a property holds (PrependController's other half).
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

	// BSDismemberSkinInstance::UpdateDismemberPartion (0x14021a530): a partition's editorVisible, its only writer after the load.
	// The skin instance knows no geometry: the mirror finds it by the skin (SceneMirror::Update).
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

	// FUN_140218200 (biped, ?, skin, slot, flag): an armor's dismember partitions shown for a biped slot, stored inline (a job thread:
	// the actor's 3D update). The skin's partitions after the call.
	struct BipedDismemberPartitions
	{
		static void thunk(void* a_biped, void* a_object, RE::BSDismemberSkinInstance* a_skin, std::int32_t a_slot, bool a_flag)
		{
			func(a_biped, a_object, a_skin, a_slot, a_flag);
			PushSkinShown(a_skin);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_1402ad800 (land): a TESObjectLAND's landscape properties, made and stored into each of its four quads' geometry (+0x128)
	// after the quads were attached. Each quad's leaf after the call (a swap: the new property's record with it).
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

	// FUN_1402ad0e0 (land): a TESObjectLAND's quads' land blend, after the cell's attach: each quad geometry's landscape material
	// takes its LOD blend textures (+0xF8, +0x100) and landBlendParams (+0x108). Each quad's property after the call (T6b1a).
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
				if (auto* geometry = node->GetChildren()[0]->AsGeometry())
					PushPropertyUpdate(geometry->GetGeometryRuntimeData().shaderProperty.get(), SceneCapture::PropertyRecord::kLandBlend);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_1414abd10 (object, visitor {params, colour, snow}): projected UV on every Lighting or grass property under the object,
	// recursively (each child through the entry again): kProjectedUV and kSnow (SetFlags, their own events), then projectedUVParams
	// and projectedUVColor stored inline. The geometry's property after the call (T6b1a).
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

	// A BGSDecalNode's decal array edits (T6b1a; AE 1.6.1170): the append (FUN_1401fdfb0, from BSTempEffectGeometryDecal::Attach and
	// the simple decals' attach), and the erases: a decal's or a reference's decals (FUN_1401fdc80 through FUN_1401fe020, from the decal
	// manager), the geometry decal's update (FUN_1401fdcb0), and the two that erase and detach its 3D (FUN_1401fdd50, FUN_1401fde40).
	// Each takes the node first; the node's decals after the call. The arguments past the node are passed through as they came.
	template <int N>
	struct DecalArrayEdit
	{
		static void thunk(RE::NiAVObject* a_node, void* a_1, void* a_2, void* a_3)
		{
			func(a_node, a_1, a_2, a_3);
			PushNodeUpdate(a_node, SceneCapture::NodeRecord::kDecals);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// BSGeometry::AttachProperty (0x140267cf0, geometry, alpha): the geometry's alpha property (+0x120), set after its attach
	// (object LOD's blocks). The leaf after the call.
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

	// FUN_1414ab770 (object, visitor {predicate, threshold}): the alpha test threshold (+0x32) of every geometry's alpha property
	// under the object the predicate admits, recursively (each child through the entry again), stored inline: an actor's or an
	// effect's fade in and out (job threads, every frame of it). The alpha's threshold after the call, per geometry.
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

	// FUN_1414ab580 (object, visitor {flag, hide}): weapon blood on every geometry under the object whose property has the flag,
	// recursively (each child through the entry again): the node's hidden bit, kWeaponBlood (SetFlags, its own event), and the
	// alpha's blend bits cleared inline (& 0xFE01). The alpha's flags and the hidden bit after the call, per geometry.
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

	// FUN_140e880e0 (visit): FUN_140e874d0's subtree walk callback that releases a node's collision object (+0x40), as an equipped item
	// loses its physics. The visit holds the node at +0x10. The node's body after the call.
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

	// FUN_140e8bd40 (node, collision object): NiAVObject::SetCollisionObject (+0x40), from bhkWorld::InitHavok and FUN_140e8c600 after
	// the node's world attach (a loader's clutter: its body comes after the capture). The node's body after the call (T6b1b).
	struct SetCollisionObject
	{
		static void thunk(RE::NiAVObject* a_node, RE::NiCollisionObject* a_collision)
		{
			func(a_node, a_collision);
			PushNodeUpdate(a_node, SceneCapture::NodeRecord::kBody);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_14147c1e0 (object, flag, set, Lighting only): one shader property flag set or cleared on every geometry's property under the
	// object, recursively (each child through the entry again), stored inline (not SetFlags): AIProcess::Update3DModel_Impl gives an
	// actor's 3D kCharacterLighting, the sky, explosions. The property's flags after the call, per geometry.
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

	// FUN_14147c690 (&fadeNode, object): the fade node of every geometry's shader property under the object (+0x60), recursively
	// (each child through the entry again), from FUN_14147bf70. The property's fade node after the call, per geometry.
	struct PropertiesSetFadeNode
	{
		static void thunk(RE::BSFadeNode** a_fadeNode, RE::NiAVObject* a_object)
		{
			func(a_fadeNode, a_object);
			// T6b3e: as a leaf update (its records with their pins, the new fade node among them: CaptureLeaf), so the entry holds the fade
			// node its tree and fade-root owners are copied from (HoldProperties); the property update alone carried no reference, and an
			// entry attached before the fade node was set found none (w140-w143: hundreds of roots left unlisted).
			if (auto* geometry = a_object ? a_object->AsGeometry() : nullptr)
				PushLeafUpdate(*geometry);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// FUN_14147a9b0 (node, near, far): the fade node's range (+0x128, +0x12C; near clamped to a minimum, far to twice it), from the
	// model and reference attach (FUN_14021f200, from the node's radius) and an FX path (FUN_1407cfba0). Its only writer.
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
			// The LOD fades, and the alpha it leaves on the property (materialAlpha times the fade node's fade, for whichever
			// camera registered it last), which a record that is not a member's shades with (MakeShading).
			if (std::bit_cast<std::uint32_t>(a_property->specularLODFade) != std::bit_cast<std::uint32_t>(specular) ||
				std::bit_cast<std::uint32_t>(a_property->envmapLODFade) != std::bit_cast<std::uint32_t>(envmap) ||
				std::bit_cast<std::uint32_t>(a_property->alpha) != std::bit_cast<std::uint32_t>(alpha))
				lodFadeEvents.Push(a_property);
			// The alpha's value for the mirror (T6b1a).
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

	// BSLightingShaderProperty::SetMaterialAlpha (vtable slot 0x31): the material alpha a classification and a record read.
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
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// MoveEvents' writers (Internal.h).
	struct AnimationGraphPlace
	{
		static bool thunk(RE::TESObjectREFR* a_reference)
		{
			const bool result = func(a_reference);
			PushMove(a_reference);
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
			PushMove(a_reference);
			CountMove(1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ActorUpdate3DPosition
	{
		static void thunk(RE::Actor* a_actor)
		{
			func(a_actor);
			PushMove(a_actor);
			CountMove(2);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SkyCellSkin
	{
		static std::uint64_t thunk(RE::Actor* a_actor)
		{
			const auto result = func(a_actor);
			PushMove(a_actor);
			CountMove(3);
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RagdollJob
	{
		// FUN_140770dc0's list: the actors' handles at +0x30, their count at +0x40.
		static void thunk(std::byte* a_list)
		{
			func(a_list);
			const auto* handles = *reinterpret_cast<const RE::RefHandle* const*>(a_list + 0x30);
			const auto count = *reinterpret_cast<const std::uint32_t*>(a_list + 0x40);
			for (std::uint32_t i = 0; handles && i < count; ++i) {
				RE::NiPointer<RE::TESObjectREFR> reference;
				if (RE::LookupReferenceByHandle(handles[i], reference))
					PushMove(reference.get());
				CountMove(4);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct GraphAnimation
	{
		static void thunk(RE::TESObjectREFR* a_reference)
		{
			func(a_reference);
			PushMove(a_reference);
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
				PushMove(reference);
			else
				PushMove(a_node);
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
	 * @brief The fade resets (T6b1a): a fade node's currentFade (+0x130) stored inline, fully faded out (0) or in (1.0), with flag bit 14
	 * (a frame bit the mirror does not hold) cleared or set after it: the cells' placements (FUN_1402bc1f0, the grid controller's on the
	 * main thread, and FUN_1402bb690, a job's, after FUN_1402d5090's), the sky cell's (FUN_1402b9da0), the reference placement's own
	 * branches that skip the snap (FUN_1402d1280), explosions' 3D (Explosion::Load3D) and others (every such store in the image: the
	 * store-then-bit-14 pattern). The patched store's handler: the store, and the node's currentFade.
	 */
	void FadeResetStore(RE::NiAVObject* a_node, std::int32_t a_value)
	{
		*reinterpret_cast<std::int32_t*>(reinterpret_cast<std::byte*>(a_node) + 0x130) = a_value;
		PushNodeUpdate(a_node, SceneCapture::NodeRecord::kFadeCurrent);
	}

	/** @brief One patched store: `mov dword ptr [base + offset], value`, as the bytes read before patching. */
	struct SwitchStoreSite
	{
		std::uintptr_t offset;  // from the image base
		std::array<std::uint8_t, 10> bytes;
		std::uint8_t length;
		int base;  // Xbyak::Operand register index
		int value;               // kImmediate: the instruction's immediate, imm
		std::uint32_t imm = 0;
		static constexpr int kImmediate = -1;
	};

	/**
	 * @brief The stubs the patched stores call: each loads (switch, value) into the first two argument registers
	 * and joins a common body that saves every volatile register, the flags (a store sets none, so the code after
	 * it may test flags set before it) and xmm0-5, calls the handler (SwitchIndexStore, FadeResetStore) on an aligned stack, and restores them.
	 */
	struct SwitchStoreStubs : Xbyak::CodeGenerator
	{
		SwitchStoreStubs(const std::vector<SwitchStoreSite>& a_sites, std::vector<std::size_t>& a_entries, std::uintptr_t a_handler) :
			Xbyak::CodeGenerator(4096)
		{
			using namespace Xbyak::util;
			Xbyak::Label common;
			// After the three pushes: rdx at [rsp], rcx at [rsp + 8], rax at [rsp + 0x10].
			auto slotOf = [](int a_index) -> int {
				return a_index == Xbyak::Operand::RAX ? 0x10 : a_index == Xbyak::Operand::RCX ? 0x8 : a_index == Xbyak::Operand::RDX ? 0x0 : -1;
			};
			for (const auto& site : a_sites) {
				a_entries.push_back(getSize());
				push(rax);
				push(rcx);
				push(rdx);
				if (const int slot = slotOf(site.base); slot >= 0)
					mov(rcx, qword[rsp + slot]);
				else
					mov(rcx, Xbyak::Reg64(site.base));
				if (site.value == SwitchStoreSite::kImmediate)
					mov(edx, site.imm);
				else if (const int slot = slotOf(site.value); slot >= 0)
					mov(edx, dword[rsp + slot]);
				else
					mov(edx, Xbyak::Reg32(site.value));
				jmp(common, T_NEAR);
			}
			L(common);
			push(r8);
			push(r9);
			push(r10);
			push(r11);
			push(rbx);
			pushf();
			mov(rbx, rsp);
			and_(rsp, ~std::uint32_t(0xF));
			sub(rsp, 0x80);
			for (int i = 0; i < 6; ++i)
				movdqu(ptr[rsp + 0x20 + 0x10 * i], Xbyak::Xmm(i));
			mov(rax, a_handler);
			call(rax);
			for (int i = 0; i < 6; ++i)
				movdqu(Xbyak::Xmm(i), ptr[rsp + 0x20 + 0x10 * i]);
			mov(rsp, rbx);
			popf();
			pop(rbx);
			pop(r11);
			pop(r10);
			pop(r9);
			pop(r8);
			pop(rdx);
			pop(rcx);
			pop(rax);
			ret();
		}
	};

	/** @brief Patches a_sites to call a_handler(base, value) in their place (after checking every site's bytes; none is patched when one differs). */
	bool PatchStores(const std::vector<SwitchStoreSite>& a_sites, std::uintptr_t a_handler, const char* a_what)
	{
		const auto base = REL::Module::get().base();
		for (const auto& site : a_sites) {
			if (std::memcmp(reinterpret_cast<const void*>(base + site.offset), site.bytes.data(), site.length) != 0) {
				logger::warn("[DCLF] {} not installed: the store at {:#x} is not the expected instruction", a_what, 0x140000000 + site.offset);
				return false;
			}
		}
		std::vector<std::size_t> entries;
		SwitchStoreStubs stubs(a_sites, entries, a_handler);
		auto* code = static_cast<std::uint8_t*>(SKSE::GetTrampoline().allocate(stubs.getSize()));
		std::memcpy(code, stubs.getCode(), stubs.getSize());
		for (std::size_t i = 0; i < a_sites.size(); ++i) {
			const std::uintptr_t at = base + a_sites[i].offset;
			std::array<std::uint8_t, 10> patch{ 0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
			const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(code + entries[i]) - static_cast<std::intptr_t>(at + 5));
			std::memcpy(patch.data() + 1, &displacement, sizeof(displacement));
			REL::safe_write(at, patch.data(), a_sites[i].length);
		}
		logger::info("[DCLF] {}: {} stores patched ({} bytes of stubs)", a_what, a_sites.size(), stubs.getSize());
		return true;
	}

	bool InstallFadeResetStores()
	{
		using Xbyak::Operand;
		constexpr int kImm = SwitchStoreSite::kImmediate;
		constexpr std::uint32_t kOne = 0x3f800000;
		const std::vector<SwitchStoreSite> sites{
			{ 0x2bc344, { 0x44, 0x89, 0xa0, 0x30, 0x01, 0x00, 0x00 }, 7, Operand::RAX, Operand::R12 },  // FUN_1402bc1f0
			{ 0x2bb8f5, { 0x44, 0x89, 0xa0, 0x30, 0x01, 0x00, 0x00 }, 7, Operand::RAX, Operand::R12 },  // FUN_1402bb690
			{ 0x2c859f, { 0x44, 0x89, 0xb8, 0x30, 0x01, 0x00, 0x00 }, 7, Operand::RAX, Operand::R15 },  // FUN_1402c8420
			{ 0x1ce552, { 0xc7, 0x80, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f }, 10, Operand::RAX, kImm, kOne },  // FUN_1401ce010
			{ 0x28bbd0, { 0xc7, 0x80, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 10, Operand::RAX, kImm, 0 },     // FUN_14028b9d0
			{ 0x2b9e89, { 0xc7, 0x87, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f }, 10, Operand::RDI, kImm, kOne },  // FUN_1402b9da0 (sky cell)
			{ 0x2d1f2c, { 0xc7, 0x80, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f }, 10, Operand::RAX, kImm, kOne },  // FUN_1402d1280
			{ 0x2d20dd, { 0xc7, 0x83, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f }, 10, Operand::RBX, kImm, kOne },  // FUN_1402d1280
			{ 0x2e3c27, { 0xc7, 0x80, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f }, 10, Operand::RAX, kImm, kOne },  // Explosion::Load3D
			{ 0x7d4791, { 0xc7, 0x80, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f }, 10, Operand::RAX, kImm, kOne },  // FUN_1407d4440
			{ 0x7ee6ec, { 0xc7, 0x80, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f }, 10, Operand::RAX, kImm, kOne },  // FUN_1407ee3e0
		};
		return PatchStores(sites, reinterpret_cast<std::uintptr_t>(&FadeResetStore), "fade resets");
	}

	bool InstallSwitchStores()
	{
		using Xbyak::Operand;
		const std::vector<SwitchStoreSite> sites{
			{ 0x438004, { 0x89, 0x91, 0x2c, 0x01, 0x00, 0x00 }, 6, Operand::RCX, Operand::RDX },  // BSTreeManager (FUN_140437e50)
			{ 0x43805e, { 0x89, 0x91, 0x2c, 0x01, 0x00, 0x00 }, 6, Operand::RCX, Operand::RDX },
			{ 0x438077, { 0x89, 0x81, 0x2c, 0x01, 0x00, 0x00 }, 6, Operand::RCX, Operand::RAX },
			{ 0x43812d, { 0x89, 0x91, 0x2c, 0x01, 0x00, 0x00 }, 6, Operand::RCX, Operand::RDX },
			{ 0x438146, { 0x89, 0x81, 0x2c, 0x01, 0x00, 0x00 }, 6, Operand::RCX, Operand::RAX },
			{ 0x4385cd, { 0x44, 0x89, 0x82, 0x2c, 0x01, 0x00, 0x00 }, 7, Operand::RDX, Operand::R8 },  // local map (FUN_140438580)
			{ 0x4388cc, { 0x89, 0x91, 0x2c, 0x01, 0x00, 0x00 }, 6, Operand::RCX, Operand::RDX },  // BSTreeManager (FUN_140438840)
			{ 0x4388e5, { 0x89, 0x81, 0x2c, 0x01, 0x00, 0x00 }, 6, Operand::RCX, Operand::RAX },
			{ 0x1e937c, { 0x89, 0x8f, 0x2c, 0x01, 0x00, 0x00 }, 6, Operand::RDI, Operand::RCX },  // harvest (FUN_1401e8ef0)
			{ 0x1e94ef, { 0x89, 0x8b, 0x2c, 0x01, 0x00, 0x00 }, 6, Operand::RBX, Operand::RCX },  // harvestable 3D (FUN_1401e9450)
		};
		return PatchStores(sites, reinterpret_cast<std::uintptr_t>(&SwitchIndexStore), "switch events");
	}
}

namespace DCLF
{
	void SceneStore::NoteFadeChanged(const RE::NiAVObject* a_fadeNode)
	{
		if (a_fadeNode)
			Scene::PushFade(static_cast<const RE::BSFadeNode*>(a_fadeNode));
	}

	void SceneStore::MuteFadeEvents(bool a_muted)
	{
		Scene::fadeEventsMuted = a_muted;
	}

	bool SceneStore::IsLoadingScreenUp()
	{
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
	}

	bool SceneStore::NoteLoadingScreen()
	{
		// Switch events are taken on the render thread only (PushSwitch): this is it (Present and the frame's start).
		switchEventThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		// Nothing may walk the scene graph while a load screen is up. A load tears down and rebuilds
		// TES::objRoot and the cell 3D under it, and the attach events queued across it name subtrees that
		// are still being assembled; walking either gives a pointer that is stale or simply garbage. That
		// is what crashed in RefreshCategoryNodes' objRoot walk (a child that read back as
		// 0x0001000000020001) and, the day before, in AddSubtree.
		//
		// The queues are still drained (T6b3d: by the coordinator's passes, their one consumer), because they hold references to
		// attached subtrees and must not grow while the world is not rendered - but the events are discarded for the tracking rather
		// than applied (the mirror keeps them: loadingCarry), and the first frame after the load rebuilds the tracked set from scratch.
		// A load invalidates all of it anyway, so nothing is lost by not trying to track across it.
		if (!SceneStore::IsLoadingScreenUp()) {
			if (std::exchange(loadingPosted, false)) {
				loadingSeen.store(false, std::memory_order_release);
				// The engine's events wake the passes again, and one takes the load's carry now.
				SetScenePassLoading(false);
				WakeScenePass(SceneWake::LoadDrain);
			}
			return false;
		}
		// Only the walking stops. The tracked set is deliberately left alone until the load is over:
		// dropping it here releases the game buffers the tables reference while the previous frame's
		// epoch is still in flight, and a draw then reads a freed device address. That is a
		// VK_ERROR_DEVICE_LOST on the teleport, which is exactly what happened when this branch cleared
		// eagerly. The entries hold NiPointers, so holding them across the load is the safe direction,
		// and the rescan replaces them on a normal frame.
		// T6b3a: what the load invalidates of the coordinator's (the rescan, the drained events not yet applied, the move events) is
		// dropped by the coordinator, told by a marker batch posted once a load (ApplyLoading): nothing of its state is written here.
		// T6b3d: loadingSeen first, then the marker: a pass that takes the marker sees the load, and drains into the mirror's carry.
		if (!std::exchange(loadingPosted, true)) {
			loadingSeen.store(true, std::memory_order_release);
			auto marker = std::make_shared<EventBatch>();
			marker->loading = true;
			eventBatches.Push(std::move(marker));
			// The first category capture after the load pins every category node (the coordinator rescans them all), and is made
			// whatever the signature says; the switches are brought up to date at the first frame's start after it (CatchUpSwitches),
			// and PrimaryCull reads them all again from the next frame's start.
			rescanCaptureAll = true;
			categoryCapturePending = true;
			worldCatchUpPending = true;
			switchResyncNext = true;
		}
		// The set does not survive a load: it names geometry from the cell being torn down, and withholds its passes from the
		// engine. Nothing is withheld until the next commit publishes the set again.
		PassCapture::Get().PublishSet(nullptr);
		// T6b3d: while it is up the engine's events wake no pass (a load pushes thousands a frame, each for the mirror's carry alone): one
		// pass drains them here, once a Present and frame start, as the old ingestion did.
		SetScenePassLoading(true);
		WakeScenePass(SceneWake::LoadDrain);
		return true;
	}

	std::shared_ptr<SceneStore::EventBatch> SceneStore::CollectEvents()
	{
		ZoneScopedN("CS.DCLF.Scene.CollectEvents");
		// T6b3d: the coordinator is the queues' one consumer. Each is drained up to where it stood at its drain (the tracker's stack by
		// one exchange, the rings by their head: EventQueue::Drain), all of them here at the pass's start, so a producer extending a queue
		// is the next pass's and a pass always ends. What the render thread posted for the pass was taken before (the category capture,
		// the mirror probe), so every event older than its capture is in this batch or an earlier one.
		auto& tracker = SceneTracker::Get();
		auto probe = mirrorProbePosted.Take();
		if (loadingSeen.load(std::memory_order_acquire)) {
			// The load screen's branch (NoteLoadingScreen's, T6b3a): the mirror's events carried to the first pass after the load, which
			// applies them before its own; the rest discarded for the tracking, the objects the events without values name kept for the
			// mirror's parity, and the references the discarded events hold (the node and switch events) released at Present.
			if (!loadingCarry)
				loadingCarry = std::make_shared<EventBatch>();
			loadingCarry->AppendMirror(tracker.Drain());
			auto& named = loadingCarry->mirrorNamed;
			auto keep = [&named](const void* a_key) { named.push_back(a_key); };
			auto discarded = std::make_shared<EventBatch>();
			BatchUnwindGuard discardedGuard(*this, discarded);
			{
				std::vector<const RE::BSFadeNode*> fades;
				DrainFadeEvents(fades);
				std::vector<const void*> keys;
				DrainPropertyEvents(keys);
			}
			DrainNodeEvents(discarded->nodes);
			fadeSnapEvents.Drain(keep);
			fadeAmountEvents.Drain(keep);
			lodSegmentEvents.Drain(keep);
			switchEvents.Drain([&](SwitchEvent&& a_event) {
				keep(a_event.node.get());
				discarded->switches.push_back(std::move(a_event));
			});
			if (!discarded->nodes.empty() || !discarded->switches.empty())
				batchesReleased.Push(std::move(discarded));
			++pumpStats.loading;
			return nullptr;
		}
		auto batch = std::make_shared<EventBatch>();
		// T6b3e: a throw hands the batch (its events' pins, nodes and switches) to the render thread, never destroys it here.
		BatchUnwindGuard unwindGuard(*this, batch);
		batch->probe = std::move(probe);
		if (loadingCarry) {
			batch->AppendMirror(std::exchange(loadingCarry->mirrorHead, nullptr));
			batch->mirrorNamed = std::move(loadingCarry->mirrorNamed);
			loadingCarry.reset();
		}
		// The drain alone: pointer moves into the batch, in push order. Everything that walks or evaluates is ApplyEvents'.
		auto* drained = tracker.Drain();
		batch->Append(drained);
		fadeSnapEvents.Drain([&](const void* a_node) { batch->fadeSnaps.push_back(a_node); });
		fadeAmountEvents.Drain([&](const void* a_node) { batch->fadeAmounts.push_back(a_node); });
		DrainFadeEvents(batch->fades);
		DrainPropertyEvents(batch->properties);
		DrainNodeEvents(batch->nodes);
		switchEvents.Drain([&](SwitchEvent&& a_event) { batch->switches.push_back(std::move(a_event)); });
		lodSegmentEvents.Drain([&](const void* a_key) { batch->lodSegments.push_back(a_key); });
		// The oldest stamp (the latencies), whether a detach came (the next category capture is forced: a detach can take a category
		// node with it before the signature sees its cell go), and what the switches' catch-ups need: engine code that writes, the
		// render thread's at its frame's start, asked with references the batch's events hold now (copies, dropped there).
		std::shared_ptr<SwitchCatchUp> catchUp;
		// T6b3e: on a throw its references (copies) go to the render thread (EngineReleases), never dropped here.
		struct CatchUpUnwind
		{
			std::shared_ptr<SwitchCatchUp>& held;
			int exceptions = std::uncaught_exceptions();
			~CatchUpUnwind()
			{
				if (!held || std::uncaught_exceptions() <= exceptions)
					return;
				for (auto& node : held->attached)
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(node)));
				for (auto& event : held->switches)
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(event.node)));
			}
		} catchUpUnwind{ catchUp };
		std::uint32_t events = 0;
		for (const auto* event = drained; event; event = event->next) {
			++events;
			if (event->stampNs && (!batch->oldestNs || event->stampNs < batch->oldestNs)) {
				batch->oldestNs = event->stampNs;
				batch->oldestFrame = event->stampFrame;
			}
			if (event->mirrorOnly)
				continue;
			if (event->type == SceneTracker::EventType::Detached)
				categoryDetachSeen.store(true, std::memory_order_relaxed);
			else if (event->type == SceneTracker::EventType::Attached && event->node) {
				if (!catchUp)
					catchUp = std::make_shared<SwitchCatchUp>();
				catchUp->attached.push_back(event->node);
			}
		}
		if (!batch->switches.empty()) {
			if (!catchUp)
				catchUp = std::make_shared<SwitchCatchUp>();
			catchUp->switches.reserve(batch->switches.size());
			for (const auto& event : batch->switches)
				catchUp->switches.push_back(SwitchEvent{ event.node, event.before, event.structural });
		}
		if (catchUp)
			switchCatchUps.Push(std::move(catchUp));
		events += static_cast<std::uint32_t>(batch->fadeSnaps.size() + batch->fadeAmounts.size() + batch->fades.size() + batch->properties.size() +
											 batch->nodes.size() + batch->switches.size() + batch->lodSegments.size());
		pumpStats.passEvents.push_back(events);
		return batch;
	}

	void SceneStore::ReleaseHandedBack()
	{
		// What the coordinator let go of since the last Present (T6b3a: pushed where it let go of them, so a pass still running adds to
		// the next Present's): its engine references are EngineReleases' (released right after this, at Present too).
		materialsReleased.Drain([](MaterialReference&& a_material) { MaterialReference released = std::move(a_material); });
		batchesReleased.Drain([](std::shared_ptr<EventBatch>&& a_batch) { const auto released = std::move(a_batch); });
	}

	void SceneStore::HandBackHeld(std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held)
	{
		if (!a_held || !a_held->reference)
			return;
		MaterialReference reference;
		reference.Adopt(std::exchange(a_held->reference, nullptr));
		materialsReleased.Push(std::move(reference));
	}

	void SceneStore::DrainMaterialCaptures()
	{
		auto& s = materialSnapshotStats;
		MaterialPort::captures.Drain([&](std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held) {
			++s.captured;
			const RE::BSShaderMaterial* material = a_held->reference;
			auto& entry = materialSnapshots[a_held->reference];
			if (entry.held && entry.held->sequence > a_held->sequence) {
				++s.stale;
				HandBackHeld(std::move(a_held));
				return;
			}
			if (entry.held) {
				++s.replaced;
				HandBackHeld(std::move(entry.held));
			}
			entry.held = std::move(a_held);
			entry.frame = sceneFrame;
			// T6b2c step 7: its slots' records are evaluated again from it (RefreshMaterialRecords).
			materialsCaptured.push_back(material);
		});
		// The render thread's answers to the joins' requests (ServeMaterialRequests): the join may ask for the material again. A served
		// request's capture was pushed before its answer.
		materialRequestsAnswered.Drain([&](MaterialAnswer&& a_answer) {
			materialRequested.erase(a_answer.material);
			++(a_answer.captured ? residentStats.materialsServed : residentStats.materialsStale);
		});
		for (auto it = materialSnapshots.begin(); it != materialSnapshots.end();) {
			if (sceneFrame - it->second.frame > kMaterialSnapshotFrames) {
				++s.aged;
				HandBackHeld(std::move(it->second.held));
				it = materialSnapshots.erase(it);
			} else {
				++it;
			}
		}
	}

	void SceneStore::ApplyEvents(std::shared_ptr<EventBatch> a_batch)
	{
		// T6b3d: the load markers posted since (NoteLoadingScreen: once a load) drop what the load invalidated first (ApplyLoading), then
		// the pass's own batch (CollectEvents'; none while a load screen is up: an empty one, which still takes the category capture and
		// the rescan). The markers hold nothing.
		eventBatches.Drain([this](std::shared_ptr<EventBatch>&& a_marker) {
			if (a_marker && a_marker->loading)
				ApplyLoading();
		});
		ApplyBatch(a_batch ? std::move(a_batch) : std::make_shared<EventBatch>());
	}

	void SceneStore::ApplyFadeNodeAnswers()
	{
		auto& r = referenceStats;
		fadeNodeAnswers.Drain([&](FadeNodeAnswer&& a_answer) {
			auto* geometry = const_cast<RE::BSGeometry*>(a_answer.geometry);
			const auto it = tracked.find(geometry);
			auto* entry = it != tracked.end() ? &it->second : nullptr;
			if (entry && entry->fadeNodeRequested == a_answer.fadeNode)
				entry->fadeNodeRequested = nullptr;
			// Taken only while the mirror still names that node for the entry's property (its fade root and tree are listed from it).
			const auto* record = entry ? mirror.Geometry(geometry) : nullptr;
			const auto* property = record && record->property ? mirror.Property(record->property) : nullptr;
			if (!entry || !a_answer.node || !property || property->fadeNode != a_answer.fadeNode || entry->fadeNodeRef.get() == a_answer.fadeNode) {
				r.fadeNodesAnsweredNone += a_answer.node ? 0 : 1;
				HandBack(std::move(a_answer.node));
				return;
			}
			++r.fadeNodesAnswered;
			HandBack(std::move(entry->fadeNodeRef));
			entry->fadeNodeRef = std::move(a_answer.node);
			// Its slots' fade root, and a member tree's node, listed now (the walk left them unlisted for want of the reference).
			for (const std::uint32_t slot : { entry->slot, entry->layerSlot }) {
				if (slot == kNoObjectSlot || slot >= tables.objects.size() || tables.objectGeometry[slot] != geometry)
					continue;
				ListFadeRoot(slot);
				if (IsResidentSlot(slot))
					ListTree(slot);
			}
		});
	}

	void SceneStore::ApplyLoading()
	{
		// NoteLoadingScreen's marker, the coordinator's half (T6b3a): the first frame after the load rescans; what was drained and not
		// applied, and the queues only the coordinator drains, are discarded.
		rescanPending = true;
		{
			std::vector<const void*> keys;
			DrainLodFadeEvents(keys);
			DrainEmittanceEvents(keys);
		}
		fadeChanged.clear();
		propertyChanged.clear();
		// The references these hold are the render thread's to drop (a node's last release runs the engine's destructors: its
		// collision object leaves the Havok world), so they go back through the retirement chain, never cleared here.
		HandBack(nodeChanged);
		moveEvents.Discard();
		movedFrame.clear();
		batchHidden.clear();
		for (auto& pending : switchPending)
			HandBack(std::move(pending.node));
		switchPending.clear();
		switchPendingIndex.clear();
		switchResync = true;
	}

	void SceneStore::ApplyBatch(std::shared_ptr<EventBatch> a_batch)
	{
		// T6b2a: the material captures first: a batch's attach names materials its leaves' captures pushed before it.
		DrainMaterialCaptures();
		auto batch = std::move(a_batch);
		// T6b3e: a throw hands it to the render thread (its pins and references), never destroys it here.
		BatchUnwindGuard unwindGuard(*this, batch);
		const bool rescanned = rescanPending;
		if (rescanPending) {
			// RefreshCategoryNodes treats every category node as newly appeared and walks it, which is
			// exactly the full rescan wanted here.
			rescanPending = false;
			for (auto& [geometry, entry] : tracked) {
				ReleaseObjectSlot(entry);
				HandBack(std::move(entry.geometry));
				HandBack(std::move(entry.property));
				HandBack(std::move(entry.layerProperty));
				HandBack(std::move(entry.faceHeadRef));
				HandBack(std::move(entry.fadeNodeRef));
			}
			tracked.clear();
			ClearFaceShapes();
			++trackedLayout;
			sceneIdentity.Reset();
			categoryNodes.clear();
			nodeSetsDirty = true;
			for (auto& [root, entry] : alwaysRenderRoots)
				HandBack(std::move(entry.root));
			alwaysRenderRoots.clear();
			validationCursor = 0;
			fullEvaluation = true;
			fadeDependents.clear();
			propertyDependents.clear();
			rootDependents.clear();
			lightDependents.clear();
			lightEntryChanges.push_back({ nullptr, 0 });
			ReleaseRootOwners();
			rootReference.clear();
			referenceRoot.clear();
			dirtyRoots.clear();
			rootMotion.clear();
			movingRoots.clear();
			hiddenDependents.clear();
			DropSunCandidates();
			buckets = {};
		}

		ApplyMirrorEvents(*batch);

		// The detaches are seen before the category refresh, so one this frame can force it: a detach can take a
		// category node with it, and the signature cannot see that until the cell itself goes.
		{
			DCLF_SCENE_PART(CategoryNodes, "CS.DCLF.Scene.CategoryNodes");
			bool sawDetach = false;
			for (const auto* event = batch->head; event && !sawDetach; event = event->next)
				sawDetach = event->type == SceneTracker::EventType::Detached && !event->mirrorOnly;
			addSource = rescanned ? TrackSource::Rescan : TrackSource::AttachEvent;
			RefreshCategoryNodes(sawDetach || rescanned);
			addSource = TrackSource::AttachEvent;
			// The subtrees the mirror had no chain for last time: the render thread captured them at a frame's start (T6b1b; T6b3d: onto
			// the tracker's stack, so a pass after it applied them).
			// T6b3e: their references handed back first (the retry list may be a root's last holder: never released on the pump, a throw
			// included); the retirement chain holds them through the pass, AddSubtree walking the keys.
			auto retry = std::exchange(pendingSubtrees, {});
			std::vector<RE::NiAVObject*> retryRoots;
			retryRoots.reserve(retry.size());
			for (const auto& root : retry)
				retryRoots.push_back(root.get());
			HandBack(retry);
			for (auto* root : retryRoots)
				AddSubtree(root, SubtreeSource::Retry);
			// T6b3e: the fade nodes the render thread pinned for entries that had none (FadeNodeRequest).
			ApplyFadeNodeAnswers();
		}

		{
			DCLF_SCENE_PART(AttachDetach, "CS.DCLF.Scene.AttachDetach");
			// A detach leaves the scene only if nothing attached the geometry again by the end of the batch: the engine moves
			// objects between containers (a cell's dynamic and static nodes, as their physics wakes and sleeps) with a detach and
			// an attach, which is a move. Its entry, slot and binding stay; the attach has evaluated it again (AddGeometry).
			std::vector<RE::BSGeometry*> detached;
			for (auto* event = batch->head; event; event = event->next) {
				// The render thread's captures are the mirror's alone (T6b3d: SceneTracker::PushCaptured).
				if (event->mirrorOnly)
					continue;
				// A sun candidate above it: its subtree changed, which its plan in the primary cut reads (PlanOf), whether or not a
				// tracked geometry came or went.
				for (const void* ancestor : event->ancestors)
					if (const auto it = sunTable.index.find(static_cast<const RE::NiAVObject*>(ancestor)); it != sunTable.index.end()) {
						sunEntriesForced.insert(it->first);
						sunEntriesDirty.push_back(it->first);
					}
				if (event->type == SceneTracker::EventType::Attached) {
					++stats.attachedEvents;
					if (!categoryNodes.empty())
						AddSubtree(event->node.get());
				} else if (event->type == SceneTracker::EventType::Detached) {
					++stats.detachedEvents;
					detached.insert(detached.end(), event->removed.begin(), event->removed.end());
				}
			}
			for (auto* geometry : detached) {
				// Only a tracked geometry is known to be alive (its entry holds it) and so safe to walk up from.
				if (tracked.contains(geometry) && FindCategoryNode(geometry, nullptr)) {
					++stats.detachMoves;
					continue;
				}
				EraseTracked(geometry);
			}
		}

		{
			// CS_DCLF_PERSISTENT_PARITY: a slice of the tracked geometries checked for a detach the events missed (each one found is dropped,
			// and flagged). The normal path trusts the detach events (invariant 5). Once a pass (T6b3d: one batch a pass, one consumer).
			DCLF_SCENE_PART(Validate, "CS.DCLF.Scene.Validate");
			if (SwitchEnabled(Switch::PersistentParity) && std::exchange(validatedFrame, PassStamp()) != PassStamp())
				ValidateSlice();
		}
		DCLF_SCENE_PART(StructuralEvents, "CS.DCLF.Scene.StructuralEvents");
		// Fade roots placed since their listing: their rows again from the node.
		for (const void* node : batch->fadeSnaps)
			ReseedFadeRoot(node);
		// Actors' fadeAmount, written since: their rows' input (the GPU's state stands).
		for (const void* node : batch->fadeAmounts)
			RefreshFadeAmount(node);
		// The fade nodes whose currentFade changed since the last apply (the delta walk re-evaluates their dependents).
		fadeChanged.insert(fadeChanged.end(), batch->fades.begin(), batch->fades.end());
		// The structural events (SceneEvents): properties whose flags, material or controllers changed, and nodes Havok
		// moved or gave a controller.
		propertyChanged.insert(propertyChanged.end(), batch->properties.begin(), batch->properties.end());
		for (auto& node : batch->nodes)
			nodeChanged.push_back(std::move(node));
		batch->nodes.clear();
		// The switch events, oldest first, one pending entry per switch: its index before the oldest event decides
		// whether the selection changed (ApplySwitchEvents).
		for (auto& event : batch->switches) {
			++delta.switchEvents;
			const auto [at, inserted] = switchPendingIndex.try_emplace(event.node.get(), static_cast<std::uint32_t>(switchPending.size()));
			if (inserted)
				switchPending.push_back({ std::move(event.node), event.before, event.structural });
			else
				switchPending[at->second].structural |= event.structural;
		}
		ApplyLodSegmentEvents(batch->lodSegments);

		stats.tracked = static_cast<std::uint32_t>(tracked.size());
		stats.categoryNodes = static_cast<std::uint32_t>(categoryNodes.size());
		// The tracked geometries its captures and leaf updates named: their properties as the mirror has them now, held from its pins
		// (T6b1b).
		for (const auto* head : { batch->mirrorHead, batch->head })
			for (const auto* event = head; event; event = event->next) {
				const SceneCapture::Records* records = event->type == SceneTracker::EventType::Attached ? event->captured.get() :
				                                       event->type == SceneTracker::EventType::Updated ? event->update.leaf.get() :
				                                                                                         nullptr;
				if (records)
					for (const auto& geometry : records->geometries)
						if (const auto it = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(geometry.key))); it != tracked.end()) {
							HoldProperties(it->second);
							// Written without its records (none then): again, now the mirror has them.
							if (std::exchange(it->second.leafWaiting, false)) {
								it->second.candidateFrame = 0;
								pendingEvaluation.push_back(it->first);
								++leafStats.rescheduled;
							}
						}
			}
		// T6b3d: its oldest event, applied now and published with the next publication (the event-to-publication latency).
		if (batch->oldestNs && (!unpublishedNs || batch->oldestNs < unpublishedNs)) {
			unpublishedNs = batch->oldestNs;
			unpublishedFrame = batch->oldestFrame;
		}
		// Its tracker events (and the switch events folded into a pending entry) hold engine references - the detached subtrees a
		// published version of the tables may still name: through the retirement chain (step 6e E3). T6b3d: a batch that holds none (no
		// tracker event, no switch event left in it: an idle pass's) is dropped here instead - its return would wake the pump for nothing,
		// and every pass would keep the next one going.
		if (batch->head || batch->mirrorHead || !batch->switches.empty())
			retirement.Open().events.push_back(std::move(batch));
		// Its pins name the retired batch's references: no reference is made from them after the apply (T6b1c).
		batchPins.clear();
	}

	namespace
	{
		// Object LOD's segment writers (LodSegments.h): each patched call makes the write, then names the shape.
		struct LodSegmentShow
		{
			static void thunk(RE::BSGeometry* a_shape, std::uint64_t a_segment)
			{
				func(a_shape, a_segment);
				lodSegmentEvents.Push(a_shape);
				PushGeometryUpdate(a_shape, SceneCapture::GeometryRecord::kSegments);  // T6b1a
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct LodSegmentHide
		{
			static void thunk(RE::BSGeometry* a_shape, std::uint64_t a_segment)
			{
				func(a_shape, a_segment);
				lodSegmentEvents.Push(a_shape);
				PushGeometryUpdate(a_shape, SceneCapture::GeometryRecord::kSegments);  // T6b1a
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct LodSegmentShowAll
		{
			static void thunk(RE::BSGeometry* a_shape)
			{
				func(a_shape);
				lodSegmentEvents.Push(a_shape);
				PushGeometryUpdate(a_shape, SceneCapture::GeometryRecord::kSegments);  // T6b1a
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool Scene::InstallLodSegmentHooks()
	{
		// Every call of the three writers (AE 1.6.1170, every xref): the active grid's and the large-reference grid's segment
		// updates (FUN_1405110b0, FUN_1405112f0), two other show-all callers (FUN_1404fdcf0, FUN_140509550) and the LOD block
		// builder (FUN_140e55790). None is patched unless every site calls what it should.
		constexpr std::uintptr_t kShow = 0xe31130, kHide = 0xe31160, kShowAll = 0xe310b0;
		constexpr std::uintptr_t kShowSites[] = { 0x511550, 0x511269 };
		constexpr std::uintptr_t kHideSites[] = { 0x511544, 0x51125c };
		constexpr std::uintptr_t kShowAllSites[] = { 0x4fde80, 0x509691, 0x511402, 0x5112b4, 0xe55ba3 };
		auto at = [](std::uintptr_t a_offset) { return REL::Offset(a_offset).address(); };
		bool ok = true;
		for (const auto site : kShowSites)
			ok = ok && Engine::CallsTo(at(site), at(kShow));
		for (const auto site : kHideSites)
			ok = ok && Engine::CallsTo(at(site), at(kHide));
		for (const auto site : kShowAllSites)
			ok = ok && Engine::CallsTo(at(site), at(kShowAll));
		if (!ok) {
			logger::warn("[DCLF] object LOD: a segment writer's call site differs; object LOD stays native");
			return false;
		}
		for (const auto site : kShowSites)
			stl::write_thunk_call<LodSegmentShow>(at(site));
		for (const auto site : kHideSites)
			stl::write_thunk_call<LodSegmentHide>(at(site));
		for (const auto site : kShowAllSites)
			stl::write_thunk_call<LodSegmentShowAll>(at(site));
		return true;
	}

	void SceneStore::SampleLodRanges(const RE::BSGeometry& a_shape, bool a_event)
	{
		// The record's (T6b1b: its segment events carry them, LodSegments::DrawnRanges at the write).
		const auto* record = mirror.Geometry(&a_shape);
		std::vector<LodSegments::Range> next = record ? record->segments : std::vector<LodSegments::Range>{};
		auto& ranges = lodRanges[&a_shape];
		// Changed by an event: its record again this frame (its range slots and their chain, WriteObject).
		if (a_event && next != ranges) {
			++lodSegmentStats.changed;
			pendingEvaluation.push_back(const_cast<RE::BSGeometry*>(&a_shape));
		}
		ranges = std::move(next);
	}

	void SceneStore::ApplyLodSegmentEvents(const std::vector<const void*>& a_shapes)
	{
		for (const void* key : a_shapes) {
			++lodSegmentStats.events;
			// Only a tracked shape is known to be alive (its entry holds it).
			if (const auto it = lodRanges.find(static_cast<const RE::BSGeometry*>(key)); it != lodRanges.end())
				SampleLodRanges(*it->first, true);
		}
		// CS_DCLF_PERSISTENT_PARITY: every tracked shape's ranges against its live state (T6b3d: every 60th pass, once in it).
		if (lodRanges.empty() || !SwitchEnabled(Switch::PersistentParity) || !PassParityDue() || std::exchange(lodParityFrame, PassStamp()) == PassStamp())
			return;
		++lodSegmentStats.checks;
		std::vector<LodSegments::Range> live;
		for (const auto& [shape, ranges] : lodRanges) {
			// An item's lease (T6b1d: the scene work may run beside the engine's update).
			const EngineReadWindow::Lease lease;
			if (!lease)
				break;
			++lodSegmentStats.shapes;
			LodSegments::DrawnRanges(shape, live);
			if (live == ranges)
				continue;
			if (lodSegmentStats.differ++ == 0)
				lodSegmentStats.first = fmt::format("'{}' ({} segments, dirty {}): {} ranges held, {} live (first {}+{} against {}+{})", shape->name.c_str() ? shape->name.c_str() : "",
					LodSegments::At<std::uint32_t>(shape, LodSegments::kSegmentCount), LodSegments::At<std::uint8_t>(shape, LodSegments::kDirty), ranges.size(), live.size(),
					ranges.empty() ? 0u : ranges[0].firstIndex, ranges.empty() ? 0u : ranges[0].indexCount, live.empty() ? 0u : live[0].firstIndex, live.empty() ? 0u : live[0].indexCount);
		}
	}

	void SceneStore::InstallSceneEvents()
	{
		static bool installed = false;
		if (installed)
			return;
		installed = true;
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSFadeNode[0] };
		const auto onVisible = reinterpret_cast<const std::uintptr_t*>(vtable.address())[0x34];
		stl::detour_thunk<FadeOnVisible>(onVisible);
		constexpr std::uintptr_t kFadeUpdate = 0x147a160;           // FUN_14147a160: BSFadeNode's fade state machine
		constexpr std::uintptr_t kPropertySetFlags = 0x147bee0;     // BSShaderProperty::SetFlags
		constexpr std::uintptr_t kPropertySetMaterial = 0x147bff0;  // BSShaderProperty::SetMaterial
		constexpr std::uintptr_t kPrependController = 0xd268d0;     // NiObjectNET::PrependController (FUN_140d268d0)
		constexpr std::uintptr_t kHavokNodeTransform = 0xea55a0;    // FUN_140ea55a0: a node's transform from its rigid body
		stl::detour_thunk<FadeUpdate>(REL::Offset(kFadeUpdate).address());
		constexpr std::uintptr_t kLodLevelUpdate = 0x147a430;       // FUN_14147a430: the LOD transition (+0x152)
		stl::detour_thunk<LodLevelUpdate>(REL::Offset(kLodLevelUpdate).address());
		constexpr std::uintptr_t kFadeSnap = 0x147aa20;             // FUN_14147aa20: a fade node's placement snap
		stl::detour_thunk<FadeSnap>(REL::Offset(kFadeSnap).address());
		stl::write_vfunc<0xE1, ActorSetAlpha<ActorTable>>(RE::VTABLE_Actor[0]);
		stl::write_vfunc<0xE1, ActorSetAlpha<CharacterTable>>(RE::VTABLE_Character[0]);
		stl::write_vfunc<0xE1, ActorSetAlpha<PlayerTable>>(RE::VTABLE_PlayerCharacter[0]);
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
		constexpr std::uintptr_t kLandBlendParams = 0x2ad0e0;  // FUN_1402ad0e0: a land's quads' LOD blend (T6b1a)
		stl::detour_thunk<LandBlendParams>(REL::Offset(kLandBlendParams).address());
		constexpr std::uintptr_t kPropertiesProjectedUV = 0x14abd10;  // FUN_1414abd10: projected UV on the properties under an object (T6b1a)
		stl::detour_thunk<PropertiesProjectedUV>(REL::Offset(kPropertiesProjectedUV).address());
		// A decal node's array edits (T6b1a): DecalArrayEdit.
		stl::detour_thunk<DecalArrayEdit<0>>(REL::Offset(0x1fdfb0).address());
		stl::detour_thunk<DecalArrayEdit<1>>(REL::Offset(0x1fdc80).address());
		stl::detour_thunk<DecalArrayEdit<2>>(REL::Offset(0x1fdcb0).address());
		stl::detour_thunk<DecalArrayEdit<3>>(REL::Offset(0x1fdd50).address());
		stl::detour_thunk<DecalArrayEdit<4>>(REL::Offset(0x1fde40).address());
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
		// The switch nodes' selections are followed by event alone (the cut's memberLive, ApplySwitchEvents): without the stores' hooks
		// (an unsupported build) DCLF cannot run.
		if (!InstallSwitchStores())
			stl::report_and_fail("Drawcall Limit Fix: the switch nodes' hooks could not be installed (an unsupported game build)");
		// The mirror's currentFade (T6b1a): the cells' inline resets.
		if (!InstallFadeResetStores())
			stl::report_and_fail("Drawcall Limit Fix: the fade resets' hooks could not be installed (an unsupported game build)");
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
		hiddenEventsInstalled = InstallHiddenStores();
		lodSegmentEventsInstalled = InstallLodSegmentHooks();
		TreeLod::Install();
	}

	void SceneStore::InstallMoveEvents()
	{
		// The two update passes are patched at their call: `call qword ptr [rax + 0x168]` (FF 90 68 01 00 00), checked
		// first. Either missing leaves every mover placed every frame.
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
		constexpr std::uintptr_t kActorUpdate3DPosition = 0x69eb80;   // Actor::UpdateActor3DPosition
		constexpr std::uintptr_t kSkyCellSkin = 0x663430;             // FUN_140663430
		constexpr std::uintptr_t kRagdollJob = 0x770dc0;              // FUN_140770dc0
		constexpr std::uintptr_t kGraphAnimation = 0x2f75a0;          // FUN_1402f75a0
		stl::detour_thunk<AnimationGraphPlace>(REL::Offset(kAnimationGraphPlace).address());
		stl::detour_thunk<ReferenceUpdate3DPosition>(REL::Offset(kReferenceUpdate3DPosition).address());
		stl::detour_thunk<ActorUpdate3DPosition>(REL::Offset(kActorUpdate3DPosition).address());
		stl::detour_thunk<SkyCellSkin>(REL::Offset(kSkyCellSkin).address());
		stl::detour_thunk<RagdollJob>(REL::Offset(kRagdollJob).address());
		stl::detour_thunk<GraphAnimation>(REL::Offset(kGraphAnimation).address());
		moveEventsInstalled = true;
		logger::info("[DCLF] move events installed (animation, 3D positions, ragdolls, the cells' update passes)");
	}

	bool SceneStore::MoveEventsLive()
	{
		return moveEventsInstalled;
	}

	bool SceneStore::SwitchEventsLive()
	{
		return switchEventsInstalled;
	}

	void SceneStore::ApplyMirrorEvents(const EventBatch& a_batch)
	{
		ZoneScopedN("CS.DCLF.Scene.Mirror");
		mirror.BeginBatch();
		const bool parity = SwitchEnabled(Switch::MirrorParity);
		mirrorReadParity = parity;
		// The batch's pins (T6b1c): its attach captures' references and its leaf updates' (T6b1b), by key, for the scene work's
		// references while it applies it.
		batchPins.clear();
		for (const auto* head : { a_batch.mirrorHead, a_batch.head })
			for (const auto* event = head; event; event = event->next) {
				if (event->type == SceneTracker::EventType::Attached && event->captured)
					PinBatch(event->captured->pins);
				PinBatch(event->pins);
			}
		auto apply = [&](const SceneTracker::Event* a_head) {
			for (const auto* event = a_head; event; event = event->next) {
				if (event->type == SceneTracker::EventType::Attached) {
					if (event->captured && event->mirrorFill) {
						// T6b3d: a mirror request's capture fills what the mirror lacks alone (SceneTracker::Event::mirrorFill).
						const auto& captured = *event->captured;
						SceneCapture::Records fill;
						fill.sequence = captured.sequence;
						for (const auto& n : captured.nodes)
							if (!mirror.Node(n.key))
								fill.nodes.push_back(n);
						for (const auto& g : captured.geometries)
							if (!mirror.Geometry(g.key))
								fill.geometries.push_back(g);
						for (const auto& p : captured.properties)
							if (!mirror.Property(p.key))
								fill.properties.push_back(p);
						for (const auto& a : captured.alphas)
							if (!mirror.Alpha(a.key))
								fill.alphas.push_back(a);
						if (!fill.nodes.empty() || !fill.geometries.empty() || !fill.properties.empty() || !fill.alphas.empty())
							mirror.Apply(fill);
					} else if (event->captured)
						mirror.Apply(*event->captured);
					else if (event->node && mirror.Node(event->node.get()))
						// Attached out of the world, yet held (T6b1b): its record is stale (the event holds the object: its name is safe).
						mirror.Evict(event->node.get(), fmt::format("'{}' {}", event->node->name.c_str() ? event->node->name.c_str() : "",
															 const_cast<RE::NiAVObject*>(event->node.get())->GetRTTI() ? const_cast<RE::NiAVObject*>(event->node.get())->GetRTTI()->name : "?"));
				} else if (event->type == SceneTracker::EventType::Detached) {
					if (event->detachedRoot)
						mirror.Detach(event->detachedRoot, event->removed, event->removedNodes);
				} else {
					const auto [type, key] = mirror.Update(event->update);
					if (parity && key)
						mirrorEventFields[SceneMirror::Key(key, type)] |= event->update.fields;
					// A hidden store's event (T6b1b): DrainHiddenEvents', the same batch's as the mirror's bit.
					if (event->update.hiddenSite != ~0u)
						if (const auto* record = std::get_if<SceneCapture::NodeRecord>(&event->update.record))
							batchHidden.push_back({ record->key, event->update.hiddenSite, (record->flags & 1u) != 0 });
				}
			}
		};
		apply(a_batch.mirrorHead);
		apply(a_batch.head);
		RecheckDeferredReads();
		if (!parity)
			return;
		// The objects the batch's events without values named: a field the probe finds different on one of them has a hook that
		// carries no value (step 6e F3). The fade, property, node and hidden events' values are updates (F3b).
		auto add = [&](const auto& a_keys) {
			for (const auto& key : a_keys) {
				if constexpr (requires { key.get(); })
					mirrorEventKeys.insert(key.get());
				else if constexpr (requires { key.node; })
					mirrorEventKeys.insert(key.node.get());
				else
					mirrorEventKeys.insert(key);
			}
		};
		// Not the fade snaps: their updates carry the fade node's values (FadeSnap; a node that is not a fade node has no fade fields).
		add(a_batch.lodSegments);
		add(a_batch.mirrorNamed);
		if (a_batch.probe)
			mirrorProbe = std::make_unique<SceneCapture::Records>(*a_batch.probe);
	}

	void SceneStore::PostMirrorProbeRequest()
	{
		// T6b3d: the coordinator's half of the mirror parity's probe (CS_DCLF_MIRROR_PARITY): the next slice of its tracked set, each
		// geometry held by the request (a copy of its entry's reference: the entry may let go before the render thread captures it),
		// asked of the render thread once a frame and one at a time. The render thread drops the references.
		if (!SwitchEnabled(Switch::MirrorParity) || tracked.empty() || mirrorProbeFrame == sceneFrame || mirrorProbeAsked.load(std::memory_order_acquire))
			return;
		mirrorProbeFrame = sceneFrame;
		constexpr std::size_t kSlice = 256;
		auto request = std::make_shared<MirrorProbeRequest>();
		request->geometries.reserve(std::min(kSlice, tracked.size()));
		auto it = tracked.begin() + static_cast<std::ptrdiff_t>(std::min(mirrorProbeCursor, tracked.size()));
		for (std::size_t n = 0; n < std::min(kSlice, tracked.size()); ++n, ++it) {
			if (it == tracked.end())
				it = tracked.begin();
			if (it->second.geometry)
				request->geometries.push_back(it->second.geometry);
		}
		mirrorProbeCursor = static_cast<std::size_t>(it - tracked.begin());
		mirrorProbeAsked.store(true, std::memory_order_release);
		mirrorProbeRequests.Push(std::move(request));
	}

	void SceneStore::ProbeMirror()
	{
		if (!SwitchEnabled(Switch::MirrorParity))
			return;
		// CS_DCLF_MIRROR_WATCH=parity: the property the last check found stale, watched from here (the render thread).
		if (const void* property = mirrorWatchRequest.exchange(nullptr, std::memory_order_acq_rel))
			MirrorWatch::ArmProperty(property);
		if (const void* alpha = mirrorWatchAlpha.exchange(nullptr, std::memory_order_acq_rel))
			MirrorWatch::ArmAlpha(alpha);
		if (const void* node = mirrorWatchNode.exchange(nullptr, std::memory_order_acq_rel))
			MirrorWatch::ArmCurrent(node);
		// T6b3d: the slice the coordinator asked for (the newest: an older one is dropped here, with its references), not its tracked set.
		std::shared_ptr<MirrorProbeRequest> request;
		mirrorProbeRequests.Drain([&request](std::shared_ptr<MirrorProbeRequest>&& a_request) { request = std::move(a_request); });
		if (!request)
			return;
		ZoneScopedN("CS.DCLF.Ingest.ProbeMirror");
		// Live (the render thread at the frame's start: the update done, before the culls): each geometry's records and its ancestors'
		// up to the world's root, each object once. Each geometry is held by the request.
		auto probe = std::make_unique<SceneCapture::Records>();
		ankerl::unordered_dense::set<const void*> taken;
		for (const auto& held : request->geometries) {
			const auto* geometry = held.get();
			if (!geometry || !SceneCapture::InWorld(geometry) || !taken.insert(geometry).second)
				continue;
			SceneCapture::CaptureLeaf(*geometry, *probe);
			for (const auto* ancestor = geometry->parent; ancestor && taken.insert(ancestor).second; ancestor = ancestor->parent)
				probe->nodes.push_back(SceneCapture::CaptureNode(*ancestor));
		}
		// To the next pass, which takes it before its drains (CollectEvents); the next request may follow.
		mirrorProbePosted.Post(std::move(probe));
		mirrorProbeAsked.store(false, std::memory_order_release);
		WakeScenePass(SceneWake::Capture);
	}

	void SceneStore::CheckMirror()
	{
		if (mirrorProbe) {
			ZoneScopedN("CS.DCLF.Scene.MirrorParity");
			mirror.Check(*mirrorProbe, mirrorEventKeys, mirrorEventFields);
			if (const void* property = mirror.TakeMissedProperty())
				mirrorWatchRequest.store(property, std::memory_order_release);
			if (const void* alpha = mirror.TakeMissedAlpha())
				mirrorWatchAlpha.store(alpha, std::memory_order_release);
			if (const void* node = mirror.TakeMissedNode())
				mirrorWatchNode.store(node, std::memory_order_release);
			mirrorProbe.reset();
		}
		mirrorEventKeys.clear();
		mirrorEventFields.clear();
	}

	void SceneStore::CatchUpSwitches(std::span<RE::NiAVObject* const> a_attached, std::span<const SwitchEvent> a_switches)
	{
		const bool world = std::exchange(worldCatchUpPending, false);
		if (!world && a_attached.empty() && a_switches.empty())
			return;
		ZoneScopedN("CS.DCLF.Ingest.SwitchCatchUps");
		const auto start = std::chrono::steady_clock::now();
		const auto* root = static_cast<const RE::NiAVObject*>(RE::Main::WorldRootNode());
		auto inWorld = [root](const RE::NiAVObject* a_object) {
			for (std::uint32_t depth = 0; root && a_object && depth <= kMaxParentDepth; ++depth, a_object = a_object->parent)
				if (a_object == root)
					return true;
			return false;
		};
		// Every switch under a_from (a whole subtree: no bound, unlike an event's walk).
		std::vector<RE::NiAVObject*> stack;
		auto catchUpUnder = [&](RE::NiAVObject* a_from, std::atomic<std::uint64_t>& a_count) {
			stack.assign(1, a_from);
			std::uint64_t count = 0;
			while (!stack.empty()) {
				auto* object = stack.back();
				stack.pop_back();
				auto* node = object ? object->AsNode() : nullptr;
				if (!node)
					continue;
				if (auto* switchNode = node->AsSwitchNode(); switchNode && CatchUpSwitch(*switchNode))
					++count;
				for (auto& child : node->GetChildren())
					if (child)
						stack.push_back(child.get());
			}
			a_count.fetch_add(count, std::memory_order_relaxed);
		};
		if (world && root)
			catchUpUnder(const_cast<RE::NiAVObject*>(root), catchUpsByAttach);
		for (auto* attachedRoot : a_attached)
			if (inWorld(attachedRoot))
				catchUpUnder(attachedRoot, catchUpsByAttach);
		for (const auto& event : a_switches) {
			auto* node = event.node.get();
			auto* switchNode = node ? node->AsSwitchNode() : nullptr;
			if (!switchNode || !inWorld(node) || (!event.structural && SwitchIndexOf(node) == event.before))
				continue;
			if (CatchUpSwitch(*switchNode))
				catchUpsBySwitch.fetch_add(1, std::memory_order_relaxed);
		}
		catchUpNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
	}

	bool SceneStore::CatchUpSwitch(RE::NiSwitchNode& a_switch)
	{
		// NiSwitchNode::OnVisible: childRevID.SetAt(index, revID) (FUN_140d29990), then the child's UpdateDownwardPass
		// (vtable slot 0x2C) with NiUpdateData { savedTime (+0x130), flags bit 1 of +0x128 as the update flag }.
		const SwitchState state = ReadSwitch(a_switch);
		const auto selection = SelectionOf(a_switch, state);
		if (!selection.child || selection.current)
			return false;
		auto* base = reinterpret_cast<std::byte*>(&a_switch);
		using SetRevision = void (*)(void*, std::uint32_t, const std::uint32_t*);
		static const REL::Relocation<SetRevision> setRevision{ REL::Offset(0xd29990) };
		setRevision(base + 0x138, selection.index, reinterpret_cast<const std::uint32_t*>(base + 0x134));
		struct UpdateData
		{
			float time;
			std::uint32_t flags;
		} data{ *reinterpret_cast<const float*>(base + 0x130), (state.flags >> 1) & 1u };
		auto* child = selection.child;
		using UpdateDownwardPass = void (*)(RE::NiAVObject*, UpdateData*, std::uint32_t);
		(*reinterpret_cast<UpdateDownwardPass* const*>(child))[0x2C](child, &data, 0);
		return true;
	}

	bool SceneStore::TakeSwitchChanges(std::vector<const RE::NiAVObject*>& a_out)
	{
		a_out.clear();
		a_out.swap(frameSwitchChanges);
		return std::exchange(frameSwitchResync, false);
	}

	void SceneStore::ApplySwitchEvents(bool a_full)
	{
		if (a_full)
			switchResync = true;
		for (auto& pending : switchPending) {
			auto* node = pending.node.get();
			// The mirror's (T6b1b: the switch's stores carry its index).
			const auto* record = node ? mirror.Node(node) : nullptr;
			// Only the scene the tables cover: a switch still loading is brought up to date by its attach (AddSubtree).
			if (!record || !(record->kind & SceneCapture::kKindSwitch) || !FindCategoryNode(node, nullptr))
				continue;
			if (!pending.structural && record->switchIndex == pending.before)
				continue;
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::SwitchEvent)];
			if (mirrorReadParity && !a_full)
				CheckTrackedBelow(MirrorRead::SwitchEvent, node);
			++delta.switchChanges;
			switchesApplied.push_back(node);
			RefreshFadeRootSwitch(node);
			if (a_full)
				continue;
			// Every entry under it: which of them the switch draws is a classification input (ClassifyFrame).
			VisitMirrorSubtree(mirror, node, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
				if (a_record.kind & SceneCapture::kKindGeometry)
					if (const auto entry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key))); entry != tracked.end()) {
						Reclassify(entry->first, entry->second);
						++delta.switchReclassified;
					}
				return true;
			});
		}
		// The walk runs on the coordinator: the switches' references are the render thread's to drop.
		for (auto& pending : switchPending)
			HandBack(std::move(pending.node));
		switchPending.clear();
		switchPendingIndex.clear();
	}

	void SceneStore::ApplyNodeEvent(RE::NiAVObject* a_node)
	{
		// Only the scene the tables cover: a node outside it (a subtree still loading, the sky) is left alone, and not
		// walked, as AddSubtree does.
		if (!a_node || !FindCategoryNode(a_node, nullptr))
			return;
		// The mirror's (T6b1b): FindCategoryNode found its record.
		const auto* record = mirror.Node(a_node);
		// An actor's entries are evaluated every frame anyway, and its sun entry is never tested.
		if (record->userData && record->formType == static_cast<std::uint8_t>(RE::FormType::ActorCharacter))
			return;
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::NodeEvent)];
		if (mirrorReadParity)
			CheckTrackedBelow(MirrorRead::NodeEvent, a_node);
		// Written every frame already, placement included: a record written in full, or one the light path moves.
		auto placedEveryFrame = [](const Tracked& a_tracked) {
			return a_tracked.perFrame && (!a_tracked.lightTraits || (a_tracked.lightTraits & (kTraitMoves | kTraitRootMoves)));
		};
		// Every sun entry node above it: its bound takes this node in, and its motion may have changed. A root already
		// known to move has its bound taken by the root pass (QueueRoots) whenever its reference has a move event, which
		// this one is (DrainMoveEvents).
		for (const void* key = a_node; key;) {
			const auto* object = static_cast<const RE::NiAVObject*>(key);
			const auto* at = mirror.Node(key);
			key = at ? at->parent : nullptr;
			if (!rootDependents.contains(object))
				continue;
			if (const auto motion = rootMotion.find(object); motion != rootMotion.end() && motion->second)
				continue;
			ScheduleRoot(object);
		}
		// Every entry below it: its placement, and its traits (a body or controller it did not have when classified).
		VisitMirrorSubtree(mirror, a_node, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
			if (a_record.kind & SceneCapture::kKindGeometry)
				if (const auto entry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key)));
					entry != tracked.end() && !placedEveryFrame(entry->second) && PlacementMatters(entry->second))
					Reclassify(entry->first, entry->second);
			return true;
		});
	}

	void SceneStore::CheckTrackedBelow(MirrorRead a_read, RE::NiAVObject* a_root)
	{
		const auto lease = LiveCheckLease(a_read);
		if (!lease)
			return;
		const auto [live, mirrored] = TrackedBelow(mirror, a_root, tracked);
		NoteMirrorRead(a_read, live != mirrored, [&] {
			return fmt::format("'{}' {}: {} tracked geometries below by the mirror, {} live", a_root->name.c_str() ? a_root->name.c_str() : "", static_cast<const void*>(a_root),
				mirrored.size(), live.size());
		}, [this, key = static_cast<const void*>(a_root), live] {
			// The mirror's walk again, the live list as tracked then (what was tracked since is not the read's).
			std::vector<const void*> now;
			VisitMirrorSubtree(mirror, key, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
				if ((a_record.kind & SceneCapture::kKindGeometry) && std::ranges::binary_search(live, a_key))
					now.push_back(a_key);
				return true;
			});
			std::ranges::sort(now);
			return now == live;
		});
	}
}
