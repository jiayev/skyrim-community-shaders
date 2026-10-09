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
			func(a_this, a_process, a_alphaGroup);
			if (CurrentFade(a_this) != before)
				PushFade(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct FadeUpdate
	{
		static std::uint64_t thunk(RE::BSFadeNode* a_this, float a_fadeAmount, void* a_camera)
		{
			const float before = CurrentFade(a_this);
			const auto result = func(a_this, a_fadeAmount, a_camera);
			if (CurrentFade(a_this) != before)
				PushFade(a_this);
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
				if (roots[i] && Engine::At<float>(roots[i], 0x100) != before[i] && (i == 0 || roots[1] != roots[0]))
					fadeAmountEvents.Push(roots[i]);
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
			if ((a_this->GetRuntimeData().unk152 & 0xF) != before)
				PushFade(a_this);
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
					PushNodeUpdate(a_node, kFadeStatics);
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
			PushNodeUpdate(fade, kFadeStatics);
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
			if (auto* geometry = a_object ? a_object->AsGeometry() : nullptr)
				PushPropertyUpdate(geometry->GetGeometryRuntimeData().shaderProperty.get(), SceneCapture::PropertyRecord::kFadeNode);
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

	/** @brief One patched store: `mov dword ptr [base + 0x12c], value`, as the bytes read before patching. */
	struct SwitchStoreSite
	{
		std::uintptr_t offset;  // from the image base
		std::array<std::uint8_t, 7> bytes;
		std::uint8_t length;
		int base;  // Xbyak::Operand register index
		int value;
	};

	/**
	 * @brief The stubs the patched stores call: each loads (switch, value) into the first two argument registers
	 * and joins a common body that saves every volatile register, the flags (a store sets none, so the code after
	 * it may test flags set before it) and xmm0-5, calls SwitchIndexStore on an aligned stack, and restores them.
	 */
	struct SwitchStoreStubs : Xbyak::CodeGenerator
	{
		SwitchStoreStubs(const std::vector<SwitchStoreSite>& a_sites, std::vector<std::size_t>& a_entries) :
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
				if (const int slot = slotOf(site.value); slot >= 0)
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
			mov(rax, reinterpret_cast<std::uintptr_t>(&SwitchIndexStore));
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
		const auto base = REL::Module::get().base();
		for (const auto& site : sites) {
			if (std::memcmp(reinterpret_cast<const void*>(base + site.offset), site.bytes.data(), site.length) != 0) {
				logger::warn("[DCLF] switch events not installed: the store at {:#x} is not the expected instruction", 0x140000000 + site.offset);
				return false;
			}
		}
		std::vector<std::size_t> entries;
		SwitchStoreStubs stubs(sites, entries);
		auto* code = static_cast<std::uint8_t*>(SKSE::GetTrampoline().allocate(stubs.getSize()));
		std::memcpy(code, stubs.getCode(), stubs.getSize());
		for (std::size_t i = 0; i < sites.size(); ++i) {
			const std::uintptr_t at = base + sites[i].offset;
			std::array<std::uint8_t, 7> patch{ 0xE8, 0, 0, 0, 0, 0x90, 0x90 };
			const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(code + entries[i]) - static_cast<std::intptr_t>(at + 5));
			std::memcpy(patch.data() + 1, &displacement, sizeof(displacement));
			REL::safe_write(at, patch.data(), sites[i].length);
		}
		logger::info("[DCLF] switch events: {} index stores patched ({} bytes of stubs)", sites.size(), stubs.getSize());
		return true;
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

	void SceneStore::IngestEvents(bool a_frameStart)
	{
		// Switch events are taken on the render thread only (PushSwitch): this is it.
		switchEventThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		// Nothing may walk the scene graph while a load screen is up. A load tears down and rebuilds
		// TES::objRoot and the cell 3D under it, and the attach events queued across it name subtrees that
		// are still being assembled; walking either gives a pointer that is stale or simply garbage. That
		// is what crashed in RefreshCategoryNodes' objRoot walk (a child that read back as
		// 0x0001000000020001) and, the day before, in AddSubtree.
		//
		// The queues are still drained, because they hold references to attached subtrees and must not grow
		// while the world is not rendered - but the events are discarded rather than applied, and the first
		// frame after the load rebuilds the tracked set from scratch. A load invalidates all of it anyway, so
		// nothing is lost by not trying to track across it.
		auto& tracker = SceneTracker::Get();
		if (SceneStore::IsLoadingScreenUp()) {
			// Only the walking stops. The tracked set is deliberately left alone until the load is over:
			// dropping it here releases the game buffers the tables reference while the previous frame's
			// epoch is still in flight, and a draw then reads a freed device address. That is a
			// VK_ERROR_DEVICE_LOST on the teleport, which is exactly what happened when this branch cleared
			// eagerly. The entries hold NiPointers, so holding them across the load is the safe direction,
			// and the rescan replaces them on a normal frame.
			rescanPending = true;
			// The set does not survive a load: it names geometry from the cell being torn down, and withholds its passes from the
			// engine. Nothing is withheld until the next commit publishes the set again.
			PassCapture::Get().PublishSet(nullptr);
			ingested.reset();
			// The mirror's (step 6e F3): carried to the next batch, which applies them before its own. The tracking takes none.
			if (!loadingCarry)
				loadingCarry = std::make_shared<EventBatch>();
			loadingCarry->AppendMirror(tracker.Drain());
			// The other queues are discarded for the tracking; the objects the ones without values name are kept for the mirror's
			// parity.
			auto& named = loadingCarry->mirrorNamed;
			auto keep = [&named](const void* a_key) { named.push_back(a_key); };
			{
				std::vector<const RE::BSFadeNode*> fades;
				DrainFadeEvents(fades);
				std::vector<const void*> keys;
				DrainPropertyEvents(keys);
				DrainLodFadeEvents(keys);
				DrainEmittanceEvents(keys);
				std::vector<RE::NiPointer<RE::NiAVObject>> nodes;
				DrainNodeEvents(nodes);
			}
			fadeChanged.clear();
			propertyChanged.clear();
			nodeChanged.clear();
			fadeSnapEvents.Drain(keep);
			fadeAmountEvents.Drain(keep);
			lodSegmentEvents.Drain(keep);
			moveEvents.Discard();
			movedFrame.clear();
			hiddenEvents.Discard();
			// The switches are brought up to date at the first ingestion after the load (CatchUpSwitches); PrimaryCull reads them all
			// again.
			worldCatchUpPending = true;
			categoryCapturePending = true;
			switchEvents.Drain([&keep](SwitchEvent&& a_event) { keep(a_event.node.get()); });
			switchPending.clear();
			switchPendingIndex.clear();
			switchResync = true;
			return;
		}
		// The drain alone: pointer moves into the batch, in push order. Everything that walks or evaluates is ApplyEvents'.
		if (!ingested)
			ingested = std::make_shared<EventBatch>();
		auto& batch = *ingested;
		if (loadingCarry) {
			batch.AppendMirror(std::exchange(loadingCarry->mirrorHead, nullptr));
			batch.mirrorNamed = std::move(loadingCarry->mirrorNamed);
			loadingCarry.reset();
		}
		auto* attached = tracker.Drain();
		batch.Append(attached);
		fadeSnapEvents.Drain([&](const void* a_node) { batch.fadeSnaps.push_back(a_node); });
		fadeAmountEvents.Drain([&](const void* a_node) { batch.fadeAmounts.push_back(a_node); });
		DrainFadeEvents(batch.fades);
		DrainPropertyEvents(batch.properties);
		DrainNodeEvents(batch.nodes);
		const std::size_t switchesBefore = batch.switches.size();
		switchEvents.Drain([&](SwitchEvent&& a_event) { batch.switches.push_back(std::move(a_event)); });
		lodSegmentEvents.Drain([&](const void* a_key) { batch.lodSegments.push_back(a_key); });
		// The switches' catch-ups are engine code that writes: the render thread's, here, never the scene work's.
		attachedRoots.clear();
		for (const auto* event = attached; event; event = event->next)
			if (event->type == SceneTracker::EventType::Attached && event->node)
				attachedRoots.push_back(event->node.get());
		CatchUpSwitches(attachedRoots, std::span<const SwitchEvent>(batch.switches).subspan(switchesBefore));
		// The category nodes as the scene work will diff them (step 6e F2): a detach can take one with it before the signature sees
		// its cell go, and a load's end finds every cell new.
		// Only at the frame's start (a_frameStart: the engine's update done, the scene work about to run): one taken at Present would
		// be the update's to change before the scene work diffed it. A detach Present ingested forces the next.
		for (const auto* event = attached; event && !categoryCapturePending; event = event->next)
			categoryCapturePending = event->type == SceneTracker::EventType::Detached;
		if (a_frameStart) {
			CaptureCategories(std::exchange(categoryCapturePending, false));
			ProbeMirror(batch);
		}
		// Tree LOD's mirror is the render thread's (DecideTreeLod reads it at the frame's start).
		treeLod.Drain(frame);
	}

	void SceneStore::NoteEventsPresent()
	{
		if (ingested)
			++ingested->presents;
	}

	bool SceneStore::EventsUnapplied() const
	{
		return ingested && ingested->presents > 1;
	}

	void SceneStore::ReleaseHandedBack()
	{
		handedBack.clear();
		spentBatches.clear();
		materialsHandedBack.clear();
	}

	void SceneStore::ApplyEvents()
	{
		auto batch = std::exchange(ingested, nullptr);
		if (!batch)
			batch = std::make_shared<EventBatch>();
		const bool rescanned = rescanPending;
		if (rescanPending) {
			// RefreshCategoryNodes treats every category node as newly appeared and walks it, which is
			// exactly the full rescan wanted here.
			rescanPending = false;
			for (auto& [geometry, entry] : tracked) {
				ReleaseObjectSlot(entry);
				HandBack(std::move(entry.geometry));
			}
			tracked.clear();
			ClearFaceShapes();
			++trackedLayout;
			sceneIdentity.Reset();
			categoryNodes.clear();
			for (auto& [root, entry] : alwaysRenderRoots)
				HandBack(std::move(entry.root));
			alwaysRenderRoots.clear();
			validationCursor = 0;
			fullEvaluation = true;
			fadeDependents.clear();
			propertyDependents.clear();
			rootDependents.clear();
			lightDependents.clear();
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
				sawDetach = event->type == SceneTracker::EventType::Detached;
			addSource = rescanned ? TrackSource::Rescan : TrackSource::AttachEvent;
			RefreshCategoryNodes(sawDetach || rescanned);
			addSource = TrackSource::AttachEvent;
		}

		{
			DCLF_SCENE_PART(AttachDetach, "CS.DCLF.Scene.AttachDetach");
			// A detach leaves the scene only if nothing attached the geometry again by the end of the batch: the engine moves
			// objects between containers (a cell's dynamic and static nodes, as their physics wakes and sleeps) with a detach and
			// an attach, which is a move. Its entry, slot and binding stay; the attach has evaluated it again (AddGeometry).
			std::vector<RE::BSGeometry*> detached;
			for (auto* event = batch->head; event; event = event->next) {
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
			// and flagged). The normal path trusts the detach events (invariant 5).
			DCLF_SCENE_PART(Validate, "CS.DCLF.Scene.Validate");
			if (SwitchEnabled(Switch::PersistentParity) && std::exchange(validatedFrame, frame) != frame)
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
		// Its tracker events (and the switch events folded into a pending entry) hold engine references - the detached subtrees a
		// published version of the tables may still name: through the retirement chain (step 6e E3).
		retirement.Open().events.push_back(std::move(batch));
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
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct LodSegmentHide
		{
			static void thunk(RE::BSGeometry* a_shape, std::uint64_t a_segment)
			{
				func(a_shape, a_segment);
				lodSegmentEvents.Push(a_shape);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct LodSegmentShowAll
		{
			static void thunk(RE::BSGeometry* a_shape)
			{
				func(a_shape);
				lodSegmentEvents.Push(a_shape);
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
		std::vector<LodSegments::Range> next;
		LodSegments::DrawnRanges(&a_shape, next);
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
		// CS_DCLF_PERSISTENT_PARITY: every tracked shape's ranges against its live state (once a frame: Present may apply too).
		if (lodRanges.empty() || !SwitchEnabled(Switch::PersistentParity) || !ParityDue(frame) || std::exchange(lodParityFrame, frame) == frame)
			return;
		++lodSegmentStats.checks;
		std::vector<LodSegments::Range> live;
		for (const auto& [shape, ranges] : lodRanges) {
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
		constexpr std::uintptr_t kPropertiesSetFlag = 0x147c1e0;  // FUN_14147c1e0: a shader flag on the properties under an object
		stl::detour_thunk<PropertiesSetFlag>(REL::Offset(kPropertiesSetFlag).address());
		constexpr std::uintptr_t kPropertiesSetFadeNode = 0x147c690;  // FUN_14147c690: the properties' fade node under an object
		stl::detour_thunk<PropertiesSetFadeNode>(REL::Offset(kPropertiesSetFadeNode).address());
		constexpr std::uintptr_t kLandSetupProperties = 0x2ad800;  // FUN_1402ad800: a land's quads' landscape properties
		stl::detour_thunk<LandSetupProperties>(REL::Offset(kLandSetupProperties).address());
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
		auto apply = [&](const SceneTracker::Event* a_head) {
			for (const auto* event = a_head; event; event = event->next) {
				if (event->type == SceneTracker::EventType::Attached) {
					if (event->captured)
						mirror.Apply(*event->captured);
				} else if (event->type == SceneTracker::EventType::Detached) {
					if (event->detachedRoot)
						mirror.Detach(event->detachedRoot, event->removed, event->removedNodes);
				} else {
					const auto [type, key] = mirror.Update(event->update);
					if (parity && key)
						mirrorEventFields[SceneMirror::Key(key, type)] |= event->update.fields;
				}
			}
		};
		apply(a_batch.mirrorHead);
		apply(a_batch.head);
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
		add(a_batch.fadeSnaps);
		add(a_batch.switches);
		add(a_batch.lodSegments);
		add(a_batch.mirrorNamed);
		if (a_batch.probe)
			mirrorProbe = std::make_unique<SceneCapture::Records>(*a_batch.probe);
	}

	void SceneStore::ProbeMirror(EventBatch& a_batch)
	{
		if (!SwitchEnabled(Switch::MirrorParity) || tracked.empty())
			return;
		// CS_DCLF_MIRROR_WATCH=parity: the property the last check found stale, watched from here (the render thread).
		if (const void* property = mirrorWatchRequest.exchange(nullptr, std::memory_order_acq_rel))
			MirrorWatch::ArmProperty(property);
		if (const void* alpha = mirrorWatchAlpha.exchange(nullptr, std::memory_order_acq_rel))
			MirrorWatch::ArmAlpha(alpha);
		ZoneScopedN("CS.DCLF.Ingest.ProbeMirror");
		// A slice of the tracked set, live (the render thread at the frame's start: the update done, before the culls): each
		// geometry's records and its ancestors' up to the world's root, each object once. A tracked geometry is held by its entry.
		constexpr std::size_t kSlice = 256;
		auto probe = std::make_unique<SceneCapture::Records>();
		ankerl::unordered_dense::set<const void*> taken;
		auto it = tracked.begin() + static_cast<std::ptrdiff_t>(std::min(mirrorProbeCursor, tracked.size()));
		for (std::size_t n = 0; n < std::min(kSlice, tracked.size()); ++n, ++it) {
			if (it == tracked.end())
				it = tracked.begin();
			const auto* geometry = it->first;
			if (!geometry || !SceneCapture::InWorld(geometry) || !taken.insert(geometry).second)
				continue;
			SceneCapture::CaptureLeaf(*geometry, *probe);
			for (const auto* ancestor = geometry->parent; ancestor && taken.insert(ancestor).second; ancestor = ancestor->parent)
				probe->nodes.push_back(SceneCapture::CaptureNode(*ancestor));
		}
		mirrorProbeCursor = static_cast<std::size_t>(it - tracked.begin());
		a_batch.probe = std::move(probe);
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
			auto* switchNode = node ? node->AsSwitchNode() : nullptr;
			// Only the scene the tables cover: a switch still loading is brought up to date by its attach (AddSubtree).
			if (!switchNode || !FindCategoryNode(node, nullptr))
				continue;
			if (!pending.structural && SwitchIndexOf(node) == pending.before)
				continue;
			++delta.switchChanges;
			if (switchesApplied.size() < kMaxSwitchChanges)
				switchesApplied.push_back(node);
			else
				switchResync = true;
			RefreshFadeRootSwitch(node);
			if (a_full)
				continue;
			// Every entry under it: which of them the switch draws is a classification input (ClassifyFrame).
			VisitSubtree(node, [&](RE::NiAVObject& a_object) {
				if (auto* geometry = a_object.AsGeometry())
					if (const auto entry = tracked.find(geometry); entry != tracked.end()) {
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
		// An actor's entries are evaluated every frame anyway, and its sun entry is never tested.
		if (const auto* reference = a_node->GetUserData(); reference && reference->GetFormType() == RE::FormType::ActorCharacter)
			return;
		// Written every frame already, placement included: a record written in full, or one the light path moves.
		auto placedEveryFrame = [](const Tracked& a_tracked) {
			return a_tracked.perFrame && (!a_tracked.lightTraits || (a_tracked.lightTraits & (kTraitMoves | kTraitRootMoves)));
		};
		// Every sun entry node above it: its bound takes this node in, and its motion may have changed. A root already
		// known to move has its bound taken by the root pass (QueueRoots) whenever its reference has a move event, which
		// this one is (DrainMoveEvents).
		for (const RE::NiAVObject* object = a_node; object; object = object->parent) {
			if (!rootDependents.contains(object))
				continue;
			if (const auto motion = rootMotion.find(object); motion != rootMotion.end() && motion->second)
				continue;
			ScheduleRoot(object);
		}
		// Every entry below it: its placement, and its traits (a body or controller it did not have when classified).
		VisitSubtree(a_node, [&](RE::NiAVObject& a_object) {
			if (auto* geometry = a_object.AsGeometry())
				if (const auto entry = tracked.find(geometry); entry != tracked.end() && !placedEveryFrame(entry->second) && PlacementMatters(entry->second))
					Reclassify(entry->first, entry->second);
			return true;
		});
	}
}
