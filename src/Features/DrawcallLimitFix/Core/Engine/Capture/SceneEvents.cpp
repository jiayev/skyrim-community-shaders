// The scene's events (subscribed on the render thread at PostPostLoad): SceneWriterFramework's engine call hooks, patched stores and
// movers (SceneWriter.h), each pushing the mirror's update and the scene pass's event on the writer's thread (SceneEvents.h).

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValueMoves.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Diagnostics/FadeProbe.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"

#include <bit>

namespace DCLF::Scene
{
	// The engine writers are SceneWriterFramework's call hooks (SWF_EVENT_CALL(SWF_HOOK_*)): each handler below is one, taking the
	// values it compares on PRE into its scratch and pushing on POST, on the writer's thread.
	namespace
	{
		using SceneWriter::Arg;
		using SceneWriter::IsPre;

		std::uintptr_t Bits(float a_value) { return std::bit_cast<std::uint32_t>(a_value); }
		float FloatOf(std::uintptr_t a_word) { return std::bit_cast<float>(static_cast<std::uint32_t>(a_word)); }

		// BSFadeNode::OnVisible (slot 0x34) and FUN_14147a160 (node, fadeAmount, camera), the fade state machine: currentFade and the
		// door state (+0x154) for the mirror.
		void OnFadeValues(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* fade = Arg<RE::BSFadeNode*>(a_event, 0);
			if (IsPre(a_event)) {
				a_scratch[0] = Bits(CurrentFade(fade));
				a_scratch[1] = fade->GetRuntimeData().unk154;
				return;
			}
			const float before = FloatOf(a_scratch[0]);
			const auto door = static_cast<std::uint8_t>(a_scratch[1]);
			if (SceneWriter::CallOf(a_event).hook == SWF_HOOK_FADE_UPDATE)
				Diagnostics::FadeProbe::NoteFade(before, CurrentFade(fade), fade->GetRuntimeData().unk154);  // TEMP
			if (CurrentFade(fade) != before || fade->GetRuntimeData().unk154 != door)
				PushNodeUpdate(fade, SceneCapture::NodeRecord::kFadeCurrent | SceneCapture::NodeRecord::kFadeDoor);
		}

		// Actor::SetAlpha's fadeAmount writes are Features/Actors' (its early hooks; the fade amounts channel's push, Features/Fades).

		/**
		 * @brief FUN_14147a430 (node, level): the only writer of the fade node's LOD level (+0x152 & 0xF, the skin partitions' row);
		 * called by the fade update (FUN_14147a160), BSLeafAnimNode::OnVisible and the feedback's fade service.
		 */
		void OnLodLevel(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* fade = Arg<RE::BSFadeNode*>(a_event, 0);
			const std::uint8_t level = fade->GetRuntimeData().unk152 & 0xF;
			if (IsPre(a_event))
				a_scratch[0] = level;
			else if (level != a_scratch[0])
				PushNodeUpdate(fade, SceneCapture::NodeRecord::kFadeLevel);
		}

		// Sky::SetColor (0x14040d970): a weather-blend colour written in place; external emittance may point at it.
		void OnSkySetColor(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* colour = Arg<RE::NiColor*>(a_event, 1);
			static_assert(sizeof(RE::NiColor) <= 2 * sizeof(std::uintptr_t));
			if (!colour)
				return;
			if (IsPre(a_event))
				std::memcpy(a_scratch, colour, sizeof(RE::NiColor));
			else if (std::memcmp(a_scratch, colour, sizeof(RE::NiColor)) != 0)
				emittanceEvents.Push(colour);
		}

		// FUN_141480fc0 (object, &colour): points the external emittance of the Lighting and material port properties under the object
		// at a colour. The colour is a classification input (ListDependents), so a re-point is a property event.
		void OnSetExternalEmittance(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			if (IsPre(a_event)) {
				auto* object = Arg<RE::NiAVObject*>(a_event, 0);
				auto* geometry = object ? object->AsGeometry() : nullptr;
				auto* shaderProperty = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				auto* property = shaderProperty ? netimmerse_cast<RE::BSLightingShaderProperty*>(shaderProperty) : nullptr;
				// A port property's emissive (the Effect port's emittanceColor, its PropertyColor: the mirror's emissive): a re-point is an
				// event too. The port's (its exact RTTI, as the capture's), read through its emissiveOf.
				const std::uint8_t port = shaderProperty && !property ? Extensions::PortOfRtti(shaderProperty->GetRTTI()) : std::uint8_t{ 0 };
				const auto emissiveOf = port ? Extensions::MaterialPorts()[port].emissiveOf : nullptr;
				a_scratch[0] = reinterpret_cast<std::uintptr_t>(property);
				a_scratch[1] = reinterpret_cast<std::uintptr_t>(shaderProperty);
				a_scratch[2] = reinterpret_cast<std::uintptr_t>(property ? property->emissiveColor : emissiveOf ? emissiveOf(*shaderProperty) :
																												  nullptr);
				a_scratch[3] = port;
				return;
			}
			auto* property = reinterpret_cast<RE::BSLightingShaderProperty*>(a_scratch[0]);
			auto* shaderProperty = reinterpret_cast<RE::BSShaderProperty*>(a_scratch[1]);
			const void* before = reinterpret_cast<const void*>(a_scratch[2]);
			const auto port = static_cast<std::uint8_t>(a_scratch[3]);
			const auto emissiveOf = port ? Extensions::MaterialPorts()[port].emissiveOf : nullptr;
			if (property && property->emissiveColor != before) {
				PushProperty(property);
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kEmissive);
			} else if (emissiveOf && emissiveOf(*shaderProperty) != before) {
				PushProperty(shaderProperty);
				PushPropertyUpdate(shaderProperty, SceneCapture::PropertyRecord::kEmissive);
			}
		}

		// FUN_1402d1280 (cell, reference) and FUN_1402d5090 (cell, reference, flag): a reference's 3D placed in its cell. They write
		// the LOD type inline (`and`/`or` at +0x153: 10 if flagged, 6 if distant) around the range (FUN_14021f200) and snap only on
		// one branch (save-load and sky cell skip it): the statics after the call.
		void OnCellPlaceReference(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* reference = Arg<RE::TESObjectREFR*>(a_event, 1);
			auto* root = reference ? reference->Get3D() : nullptr;
			if (auto* fade = root ? root->AsFadeNode() : nullptr) {
				PushNodeUpdate(fade, kFadeStatics | kFadeState);
				Diagnostics::Notify(Diagnostics::FadeStaticsPlaced{ fade });
			}
		}

		// FUN_140b44490 (rigid body, motion type, ...): the motion rebuilt in place (from hkpRigidBody::setMotionType, FUN_140b30700,
		// directly or deferred while the world is locked). Every motion type path ends here (SetMotionType, animation graph, ragdolls,
		// Havok). The owning node (TESHavokUtilities::FindCollidableObject) after the call.
		void OnRigidBodyMotionType(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* body = Arg<RE::hkpRigidBody*>(a_event, 0))
				PushNodeUpdate(RE::TESHavokUtilities::FindCollidableObject(*body->GetCollidable()), SceneCapture::NodeRecord::kBody);
		}

		// NiObjectNET::RemoveController (0x140d269a0): PrependController's other half.
		void OnRemoveController(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* target = Arg<RE::NiObjectNET*>(a_event, 0);
			if (!target)
				return;
			if (auto* object = netimmerse_cast<RE::NiAVObject*>(target))
				PushNodeUpdate(object, SceneCapture::NodeRecord::kControllers);
			else
				PushPropertyUpdate(netimmerse_cast<RE::BSShaderProperty*>(target), SceneCapture::PropertyRecord::kControllers);
		}

		// NiObjectNET::PrependController (FUN_140d268d0).
		void OnPrependController(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* target = Arg<RE::NiObjectNET*>(a_event, 0);
			if (!target)
				return;
			if (auto* object = netimmerse_cast<RE::NiAVObject*>(target)) {
				PushNode(object);
				FrameValueMoves::PushNode(object);
				PushNodeUpdate(object, SceneCapture::NodeRecord::kControllers);
			} else {
				PushProperty(target);
				PushPropertyUpdate(netimmerse_cast<RE::BSShaderProperty*>(target), SceneCapture::PropertyRecord::kControllers);
			}
		}

		// FUN_140ea55a0: a node's transform from its rigid body.
		void OnHavokNodeTransform(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* collision = Arg<RE::NiCollisionObject*>(a_event, 0);
			PushNode(collision->sceneObject);
			FrameValueMoves::PushNode(collision->sceneObject);
		}

		// BSDismemberSkinInstance::UpdateDismemberPartion (0x14021a530): a partition's editorVisible, its only writer after load. The
		// skin instance knows no geometry: the mirror finds it by the skin.
		void PushSkinShown(const RE::BSDismemberSkinInstance* a_skin)
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

		void OnDismemberPartition(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushSkinShown(Arg<RE::BSDismemberSkinInstance*>(a_event, 0));
		}

		// FUN_140218200 (biped, ?, skin, slot, flag): an armor's partitions shown for a biped slot, stored inline (job thread, the
		// actor's 3D update). The skin's partitions after the call.
		void OnBipedDismemberPartitions(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushSkinShown(Arg<RE::BSDismemberSkinInstance*>(a_event, 2));
		}

		// The four quads' first geometry of a TESObjectLAND's loaded nodes (+0x40).
		template <class F>
		void ForEachLandQuad(const RE::TESObjectLAND* a_land, F&& a_visit)
		{
			const auto* loaded = a_land ? Engine::At<const RE::NiNode* const*>(a_land, 0x40) : nullptr;
			if (!loaded)
				return;
			for (std::uint32_t quad = 0; quad < 4; ++quad) {
				const auto* node = loaded[quad];
				if (!node || node->GetChildren().empty() || !node->GetChildren()[0])
					continue;
				if (auto* geometry = node->GetChildren()[0]->AsGeometry())
					a_visit(*geometry);
			}
		}

		// FUN_1402ad800 (land): a TESObjectLAND's landscape properties stored into its four quads' geometry (+0x128) after their
		// attach. Each quad's leaf after the call (a swap, with the new property's record).
		void OnLandSetupProperties(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			ForEachLandQuad(Arg<RE::TESObjectLAND*>(a_event, 0), [](RE::BSGeometry& a_geometry) { PushLeafUpdate(a_geometry); });
		}

		// FUN_1402ad0e0 (land): after the cell's attach, each quad's landscape material takes LOD blend textures (+0xF8, +0x100) and
		// landBlendParams (+0x108). Each quad's material (a write: its record takes the LOD blend textures, MTLandLODBlend's t13 and t15,
		// which it otherwise holds null from the attach's capture) and property after the call.
		void OnLandBlendParams(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			ForEachLandQuad(Arg<RE::TESObjectLAND*>(a_event, 0), [](RE::BSGeometry& a_geometry) {
				const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
				if (property)
					MaterialWrites::NoteWritten(property->material);
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kLandBlend);
			});
		}

		RE::BSGeometry* GeometryArg(const SWF_EventHeader* a_event, std::uint32_t a_index)
		{
			auto* object = Arg<RE::NiAVObject*>(a_event, a_index);
			return object ? object->AsGeometry() : nullptr;
		}

		// FUN_1414abd10 (object, visitor {params, colour, snow}): projected UV on every Lighting or grass property under the object
		// (recursing through the entry): kProjectedUV and kSnow via SetFlags, projectedUVParams and projectedUVColor inline. The
		// property after the call.
		void OnPropertiesProjectedUV(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* geometry = GeometryArg(a_event, 0))
				PushPropertyUpdate(geometry->GetGeometryRuntimeData().shaderProperty.get(), SceneCapture::PropertyRecord::kProjected);
		}

		// BSGeometry::AttachProperty (0x140267cf0, geometry, alpha): the alpha property (+0x120), set after attach (object LOD's
		// blocks). The leaf after the call.
		void OnGeometryAttachAlpha(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* geometry = Arg<RE::BSGeometry*>(a_event, 0))
				PushLeafUpdate(*geometry);
		}

		// FUN_1414ab770 (object, visitor {predicate, threshold}): the alpha test threshold (+0x32) of admitted geometries under the
		// object, inline: an actor's or effect's fade (job threads, every frame of it). Per geometry after the call.
		void OnAlphasSetThreshold(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* geometry = GeometryArg(a_event, 0))
				PushAlphaUpdate(geometry->GetGeometryRuntimeData().alphaProperty.get(), SceneCapture::AlphaRecord::kThreshold);
		}

		// FUN_1414ab580 (object, visitor {flag, hide}): weapon blood on flagged geometries under the object: hidden bit, kWeaponBlood
		// (SetFlags), alpha blend bits cleared inline (& 0xFE01). Alpha flags and hidden bit after the call, per geometry.
		void OnWeaponBloodGeometry(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* geometry = GeometryArg(a_event, 0)) {
				PushAlphaUpdate(geometry->GetGeometryRuntimeData().alphaProperty.get(), SceneCapture::AlphaRecord::kFlags);
				PushNodeUpdate(geometry, SceneCapture::NodeRecord::kHidden);
			}
		}

		// FUN_140e880e0 (visit): FUN_140e874d0's walk callback releasing a node's collision object (+0x40) as an equipped item loses
		// physics; the node is at visit +0x10. The body after the call.
		void OnReleaseCollisionObject(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			if (IsPre(a_event)) {
				auto* visit = Arg<std::byte*>(a_event, 0);
				a_scratch[0] = visit ? *reinterpret_cast<std::uintptr_t*>(visit + 0x10) : 0;
			} else
				PushNodeUpdate(reinterpret_cast<RE::NiAVObject*>(a_scratch[0]), SceneCapture::NodeRecord::kBody);
		}

		// bhkWorld::InitHavok (0x140e86ad0: world, from, to): moves a collision object from `from` to `to`, SetCollisionObject on `to`
		// and `from`'s +0x40 cleared inline (0x140e86fb7, 0x140e870c6; Hazard::InitHavok's, a projectile's or a quiver's). Both bodies
		// after the call.
		void OnHavokInit(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushNodeUpdate(Arg<RE::NiAVObject*>(a_event, 1), SceneCapture::NodeRecord::kBody);
			PushNodeUpdate(Arg<RE::NiAVObject*>(a_event, 2), SceneCapture::NodeRecord::kBody);
		}

		// The reference's 3D's LOD type (+0x153 & 0xF) after one of its inline writers: Papyrus Disable with a fade (FUN_140a3db90: type 9,
		// the fade out before the disable; a critter's despawn) and the script command Enable (0x14034c9f0: a type of 9 reset to the
		// model's).
		void PushReferenceFadeType(RE::TESObjectREFR* a_reference)
		{
			auto* root = a_reference ? a_reference->Get3D() : nullptr;
			if (auto* fade = root ? root->AsFadeNode() : nullptr)
				PushNodeUpdate(fade, SceneCapture::NodeRecord::kFadeType);
		}
		void OnReferenceDisable(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushReferenceFadeType(Arg<RE::TESObjectREFR*>(a_event, 0));
		}
		void OnEnableCommand(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushReferenceFadeType(Arg<RE::TESObjectREFR*>(a_event, 2));
		}

		// FUN_140e8bd40 (node, collision object): NiAVObject::SetCollisionObject (+0x40), from bhkWorld::InitHavok and FUN_140e8c600
		// after the world attach (a loader's clutter gets its body after the capture). The body after the call.
		void OnSetCollisionObject(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushNodeUpdate(Arg<RE::NiAVObject*>(a_event, 0), SceneCapture::NodeRecord::kBody);
		}

		// FUN_14147c1e0 (object, flag, set, Lighting only): one shader flag on every property under the object, inline (not SetFlags):
		// AIProcess::Update3DModel_Impl's kCharacterLighting, the sky, explosions. Per geometry after the call.
		void OnPropertiesSetFlag(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* geometry = GeometryArg(a_event, 0))
				PushPropertyUpdate(geometry->GetGeometryRuntimeData().shaderProperty.get(),
					SceneCapture::PropertyRecord::kFlags | SceneCapture::PropertyRecord::kMaterialOther);
		}

		// FUN_14147c690 (&fadeNode, object): the fade node (+0x60) of every shader property under the object, from FUN_14147bf70. Per
		// geometry after the call.
		void OnPropertiesSetFadeNode(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			// A leaf update, so the entry holds the new fade node its tree and fade-root owners copy from (HoldProperties); a property
			// update carries no reference, and an entry attached before the fade node was set would leave its root unlisted.
			if (auto* geometry = GeometryArg(a_event, 1))
				PushLeafUpdate(*geometry);
		}

		// FUN_14147a9b0 (node, near, far): the fade range's only writer (+0x128, +0x12C; near clamped to a minimum, far to twice it),
		// from the attach (FUN_14021f200, from the radius) and an FX path (FUN_1407cfba0).
		void OnFadeSetRange(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushNodeUpdate(Arg<RE::BSFadeNode*>(a_event, 0), SceneCapture::NodeRecord::kFadeNear | SceneCapture::NodeRecord::kFadeFar);
		}

		// FUN_14147aa00 (node, type): the fade node's LOD type (+0x153 & 0xF), from the model processor and the reference 3D paths.
		void OnFadeSetLodType(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushNodeUpdate(Arg<RE::BSFadeNode*>(a_event, 0), SceneCapture::NodeRecord::kFadeType);
		}

		// BSLightingShaderProperty::GetRenderPasses (slot 0x2A).
		void OnLodFadeRenderPasses(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* property = Arg<RE::BSLightingShaderProperty*>(a_event, 0);
			if (IsPre(a_event)) {
				a_scratch[0] = Bits(property->specularLODFade);
				a_scratch[1] = Bits(property->envmapLODFade);
				a_scratch[2] = Bits(property->alpha);
				return;
			}
			// The LOD fades and the alpha left on the property (materialAlpha times the fade, for the last registering camera),
			// which non-member records shade with (MakeShading).
			if (Bits(property->specularLODFade) != a_scratch[0] || Bits(property->envmapLODFade) != a_scratch[1] || Bits(property->alpha) != a_scratch[2])
				lodFadeEvents.Push(property);
			// The alpha's value for the mirror.
			if (Bits(property->alpha) != a_scratch[2])
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kAlphaValue);
		}

		// BSShaderProperty::SetFlags (0x14147bee0).
		void OnPropertySetFlags(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* property = Arg<RE::BSShaderProperty*>(a_event, 0);
			if (IsPre(a_event))
				a_scratch[0] = property->flags.underlying();
			else if (property->flags.underlying() != a_scratch[0]) {
				PushProperty(property);
				// The glints follow kVertexLighting (CaptureProperty).
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kFlags | SceneCapture::PropertyRecord::kMaterialOther);
			}
		}

		// BSShaderProperty::SetMaterial (0x14147bff0).
		void OnPropertySetMaterial(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* property = Arg<RE::BSShaderProperty*>(a_event, 0);
			if (IsPre(a_event))
				a_scratch[0] = reinterpret_cast<std::uintptr_t>(property->material);
			else if (reinterpret_cast<std::uintptr_t>(property->material) != a_scratch[0]) {
				PushProperty(property);
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kMaterial | SceneCapture::PropertyRecord::kMaterialAlpha |
												 SceneCapture::PropertyRecord::kMaterialOther);
			}
		}

		// BSLightingShaderProperty::SetMaterialAlpha (vtable slot 0x31).
		void OnPropertySetMaterialAlpha(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* property = Arg<RE::BSLightingShaderProperty*>(a_event, 0);
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
			if (IsPre(a_event))
				a_scratch[0] = Bits(material ? material->materialAlpha : 0.0f);
			else if (material && Bits(material->materialAlpha) != a_scratch[0]) {
				PushProperty(property);
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kMaterialAlpha);
			}
		}

		// MoveEvents' writers (SceneEvents.h), each pushing its key to the scene pass and to FrameValues' producer (FrameValueMoves).
		void PushMoved(const void* a_key)
		{
			PushMove(a_key);
			FrameValueMoves::PushKey(a_key);
		}

		// SceneWriterFramework's movers (SWF_EVENT_MOVED), counted under Core's writer slots: the animation graph's placement
		// (FUN_14066afa0) 0, TESObjectREFR::Update3DPosition 1, a reference's graph animation (FUN_1402f75a0) 5, the cells' update
		// passes (UpdateSelectedDownwardPass, its reference or the node as the key) 6. Slots 2-4 (actor 3D position, sky cell skin,
		// ragdoll) are Features/Actors' reserved writers (its early hooks).
		void OnMoved(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& event = *reinterpret_cast<const SWF_MovedEvent*>(a_event);
			static constexpr std::size_t kSlots[] = { 0, 1, 5, 6 };
			PushMoved(event.key);
			if (event.writer < std::size(kSlots))
				CountMove(kSlots[event.writer]);
		}

		// SceneWriterFramework's Write (SWF_EVENT_SCENE_WRITE, a plugin's scene write made through SWF): a property swapped on a geometry,
		// its leaf again (the record and the new property, with pins); an alpha property's flags, its record. The hidden bit is
		// Features/HiddenNodes' (a hidden event on its reserved site).
		void OnSceneWrite(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& write = *reinterpret_cast<const SWF_SceneWriteEvent*>(a_event);
			if (write.kind == SWF_WRITE_SHADER_PROPERTY || write.kind == SWF_WRITE_ALPHA_PROPERTY) {
				if (auto* geometry = static_cast<RE::NiAVObject*>(write.object)->AsGeometry())
					PushLeafUpdate(*geometry);
			} else if (write.kind == SWF_WRITE_ALPHA_FLAGS)
				PushAlphaUpdate(static_cast<const RE::NiAlphaProperty*>(write.object), SceneCapture::AlphaRecord::kFlags);
		}

		// NiSwitchNode's own child edits (its vtable's implementations of NiNode slots 0x35, 0x37-0x3C).
		void OnSwitchStructural(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushSwitchStructural(static_cast<RE::NiNode*>(reinterpret_cast<const SWF_NodeEvent*>(a_event)->node));
		}

		struct CallSubscription
		{
			std::uint32_t hook;
			SWF_EventFn handler;
			std::uint32_t phases;
		};
		constexpr std::uint32_t kPost = SWF_PHASE_POST;
		constexpr std::uint32_t kBoth = SWF_PHASE_PRE | SWF_PHASE_POST;
		constexpr CallSubscription kCalls[] = {
			{ SWF_HOOK_FADE_ON_VISIBLE, &OnFadeValues, kBoth },
			{ SWF_HOOK_FADE_UPDATE, &OnFadeValues, kBoth },
			{ SWF_HOOK_FADE_LOD_LEVEL, &OnLodLevel, kBoth },
			{ SWF_HOOK_FADE_SET_RANGE, &OnFadeSetRange, kPost },
			{ SWF_HOOK_FADE_SET_LOD_TYPE, &OnFadeSetLodType, kPost },
			{ SWF_HOOK_CELL_PLACE_REFERENCE, &OnCellPlaceReference, kPost },
			{ SWF_HOOK_CELL_PLACE_REFERENCE_AT, &OnCellPlaceReference, kPost },
			{ SWF_HOOK_RIGID_BODY_MOTION_TYPE, &OnRigidBodyMotionType, kPost },
			{ SWF_HOOK_RELEASE_COLLISION_OBJECT, &OnReleaseCollisionObject, kBoth },
			{ SWF_HOOK_SET_COLLISION_OBJECT, &OnSetCollisionObject, kPost },
			{ SWF_HOOK_HAVOK_INIT, &OnHavokInit, kPost },
			{ SWF_HOOK_REFERENCE_DISABLE, &OnReferenceDisable, kPost },
			{ SWF_HOOK_ENABLE_COMMAND, &OnEnableCommand, kPost },
			{ SWF_HOOK_PREPEND_CONTROLLER, &OnPrependController, kPost },
			{ SWF_HOOK_REMOVE_CONTROLLER, &OnRemoveController, kPost },
			{ SWF_HOOK_HAVOK_NODE_TRANSFORM, &OnHavokNodeTransform, kPost },
			{ SWF_HOOK_DISMEMBER_PARTITION, &OnDismemberPartition, kPost },
			{ SWF_HOOK_BIPED_DISMEMBER_PARTITIONS, &OnBipedDismemberPartitions, kPost },
			{ SWF_HOOK_PROPERTY_SET_FLAGS, &OnPropertySetFlags, kBoth },
			{ SWF_HOOK_PROPERTY_SET_MATERIAL, &OnPropertySetMaterial, kBoth },
			{ SWF_HOOK_LIGHTING_SET_MATERIAL_ALPHA, &OnPropertySetMaterialAlpha, kBoth },
			{ SWF_HOOK_LIGHTING_GET_RENDER_PASSES, &OnLodFadeRenderPasses, kBoth },
			{ SWF_HOOK_PROPERTIES_SET_FLAG, &OnPropertiesSetFlag, kPost },
			{ SWF_HOOK_PROPERTIES_SET_FADE_NODE, &OnPropertiesSetFadeNode, kPost },
			{ SWF_HOOK_PROPERTIES_PROJECTED_UV, &OnPropertiesProjectedUV, kPost },
			{ SWF_HOOK_SET_EXTERNAL_EMITTANCE, &OnSetExternalEmittance, kBoth },
			{ SWF_HOOK_SKY_SET_COLOR, &OnSkySetColor, kBoth },
			{ SWF_HOOK_GEOMETRY_ATTACH_ALPHA, &OnGeometryAttachAlpha, kPost },
			{ SWF_HOOK_ALPHAS_SET_THRESHOLD, &OnAlphasSetThreshold, kPost },
			{ SWF_HOOK_WEAPON_BLOOD_GEOMETRY, &OnWeaponBloodGeometry, kPost },
			{ SWF_HOOK_LAND_SETUP_PROPERTIES, &OnLandSetupProperties, kPost },
			{ SWF_HOOK_LAND_BLEND_PARAMS, &OnLandBlendParams, kPost },
		};
	}
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
		// SceneWriterFramework's movers (OnMoved); without them every mover is placed every frame.
		if (!(SceneWriter::Caps() & SWF_CAP_MOVES) || !SceneWriter::Subscribe(SWF_EVENT_MOVED, &OnMoved)) {
			logger::warn("[DCLF] move events: SceneWriterFramework has no movers (see the [SWF] log lines); every mover is placed every frame");
			return;
		}
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
		// Every family below is one DrawcallLimitFix::PostPostLoad required (SceneWriter::kRequiredCaps) before any hook was written, so a
		// refused subscription is SWF contradicting its own capabilities.
		for (const auto& call : kCalls)
			if (!SceneWriter::SubscribeCall(call.hook, call.handler, call.phases))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the scene writer framework refused engine call hook {} after reporting it installed", call.hook));
		lodFadeEventsInstalled = true;
		// FUN_14147aa20, a fade node's placement snap, is Features/Fades' (its early hooks; the fade snaps channel's source).
		// Actor::SetAlpha's slot (0xE1) is Features/Actors' (its early hooks); no other hook takes it.
		// Switch selections are followed by event alone (memberLive, ApplySwitchEvents), and the mirror's currentFade takes the cells'
		// inline resets: SceneWriterFramework's stores. NiSwitchNode's own child edits are its structural events.
		if (!SceneWriter::Subscribe(SWF_EVENT_SWITCH_INDEX, &OnSwitchIndex) || !SceneWriter::Subscribe(SWF_EVENT_FADE_RESET, &OnFadeReset) ||
			!SceneWriter::Subscribe(SWF_EVENT_SWITCH_STRUCTURAL, &OnSwitchStructural))
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused the switch or fade-reset subscriptions after reporting them installed");
		switchEventsInstalled = true;
		// A plugin's scene writes through SWF (CS's own among them: DrawcallLimitFix::WriteShaderProperty): a provider before ABI minor 2
		// has none, and every writer then stores itself.
		if (const auto* api = SceneWriter::Api(); api && api->abiMinor >= 2 && !SceneWriter::Subscribe(SWF_EVENT_SCENE_WRITE, &OnSceneWrite))
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused its scene write events at ABI minor 2");
		logger::info("[DCLF] scene events subscribed to SceneWriterFramework ({} engine calls: fades, property flags, materials and material alpha, LOD fades, emittance, Havok node transforms, controllers)",
			std::size(kCalls));
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
