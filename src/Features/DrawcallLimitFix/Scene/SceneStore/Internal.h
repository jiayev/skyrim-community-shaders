#pragma once

// SceneStore's implementation, shared by the files of this folder only (SceneStore.h is the interface).

#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Scene/LightingConstants.h"
#include "Features/DrawcallLimitFix/Scene/MaterialSources.h"
#include "Features/DrawcallLimitFix/Engine/EngineStates.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Common/Toggles.h"
#include "Features/DrawcallLimitFix/Draws/GpuResources.h"
#include "Features/DrawcallLimitFix/Engine/PassCapture.h"
#include "Features/DrawcallLimitFix/Engine/SceneTracker.h"
#include "Features/DrawcallLimitFix/Engine/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Engine/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Engine/ShadowViews.h"
#include "Features/DrawcallLimitFix/Scene/VertexInput.h"
#include "Features/DrawcallLimitFix/Engine/FaceSnapshots.h"
#include <bit>
#include <xbyak/xbyak.h>
#include <chrono>
#include "Features/ExtendedTranslucency.h"
#include "Features/LightLimitFix.h"
#include "Features/Skin.h"
#include "Features/Skylighting.h"
#include "State.h"
#include "Utils/ExternalEmittance.h"

namespace DCLF
{
	namespace Scene
	{
		// Cell 3D category nodes Drawcall Limit Fix draws from (engine notes: cell 3D category nodes).
		// The Actor node holds the actors of an exterior (an interior moves them into its rooms). It is
		// walked whatever CS_DCLF_ACTORS says: the toggle is ClassifyFrame's Actor rule, so it can change live.
		constexpr std::array<std::uint32_t, 4> kDrawnCategories{ 0 /*Actor*/, 3 /*Static*/, 4 /*Dynamic*/, 5 /*MultiBound*/ };
		constexpr std::uint32_t kMaxParentDepth = 64;
		constexpr std::size_t kValidationSlice = 256;
		constexpr std::uint32_t kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT
		constexpr std::uint32_t kSpecularBit = 0x200;  // pass descriptor Specular
		constexpr std::uint32_t kTechniqueEnvmap = 1;
		constexpr std::uint32_t kTechniqueTreeAnim = 12;

		/**
		 * @brief Visits the subtree under a_root depth first, a_root included, until a_visit(object) returns false. An
		 * event's walk: bounded to kMaxEventNodes objects, and whatever it leaves out the frame's walk still covers.
		 */
		template <class Visit>
		void VisitSubtree(RE::NiAVObject* a_root, Visit&& a_visit)
		{
			constexpr std::size_t kMaxEventNodes = 4096;
			std::vector<RE::NiAVObject*> stack{ a_root };
			for (std::size_t visited = 0; !stack.empty() && visited < kMaxEventNodes; ++visited) {
				auto* object = stack.back();
				stack.pop_back();
				if (!a_visit(*object))
					return;
				if (auto* node = object->AsNode())
					for (auto& child : node->GetChildren())
						if (child)
							stack.push_back(child.get());
			}
		}

		/** @brief A 64-bit FNV-1a hash over values folded in one at a time: the input signatures' hash. */
		struct Fnv1a
		{
			std::uint64_t value = 0xcbf29ce484222325ull;
			void Mix(std::uint64_t a_value) { value = (value ^ a_value) * 0x100000001b3ull; }
			void Mix(const void* a_pointer) { Mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_pointer))); }
		};

		inline const RE::BSRenderPass* FindLightingPass(RE::BSShaderProperty* a_property)
		{
			for (auto* pass = a_property->renderPassList.head; pass; pass = pass->next) {
				if (pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Lighting && pass->numLights > 0 && pass->sceneLights)
					return pass;
			}
			return nullptr;
		}

		// a_member: a scene member's (SceneStore's resident records), whose fades are DCLF's (FadeStateCS) and so not in its alpha.
		inline ObjectShading MakeShading(const RE::BSLightingShaderProperty& a_property, const LightingDescriptors& a_descriptors, std::uint32_t a_renderFlags,
			float& a_emissiveMult, bool a_member)
		{
			// BSLightingShader::SetupGeometry (engine notes): which components it writes depends on the pass.
			const float unwritten = std::bit_cast<float>(kUnwrittenBits);
			const bool specular = (a_descriptors.pass & kSpecularBit) != 0;
			ObjectShading shading{};
			shading.materialData[0] = a_descriptors.technique == kTechniqueEnvmap ? a_descriptors.envmapLODFade : unwritten;
			shading.materialData[1] = specular ? a_descriptors.specularLODFade : unwritten;
			// GetRenderPasses leaves materialAlpha * the fade node's currentFade on the property, for whichever camera called it last.
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(a_property.material);
			shading.materialData[2] = a_member && material ? material->materialAlpha : a_property.alpha;
			shading.materialData[3] = unwritten;
			const float mult = a_property.emissiveMult;
			// The same sample the emissive colour below folds in: the shader divides it out again, so the
			// two must never come from different reads of an animated value.
			a_emissiveMult = mult;
			const auto* emissive = a_property.emissiveColor;
			shading.emitColor[0] = emissive ? emissive->red * mult : unwritten;
			shading.emitColor[1] = emissive ? emissive->green * mult : unwritten;
			shading.emitColor[2] = emissive ? emissive->blue * mult : unwritten;
			shading.ssrSpecular = ((a_renderFlags & 2) ? 0.0f : 1.0f) * (specular ? a_descriptors.specularLODFade : 0.0f);
			return shading;
		}

		inline void StoreTransform(const RE::NiTransform& a_transform, float (&a_out)[12])
		{
			// Row-major 3x4: rotation scaled, translation in the last column.
			const auto& r = a_transform.rotate.entry;
			const float s = a_transform.scale;
			for (int row = 0; row < 3; ++row) {
				a_out[row * 4 + 0] = r[row][0] * s;
				a_out[row * 4 + 1] = r[row][1] * s;
				a_out[row * 4 + 2] = r[row][2] * s;
			}
			a_out[3] = a_transform.translate.x;
			a_out[7] = a_transform.translate.y;
			a_out[11] = a_transform.translate.z;
		}

		inline bool IsHidden(const RE::NiAVObject* a_object)
		{
			return a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden);
		}

		/**
		 * @brief The engine's per-frame palette update (AE FUN_140e4ff90): what the bone setter runs from the
		 * native draw an owned object no longer gets. Idempotent within a frame (frameID); it copies the current
		 * palette to the previous one first and writes three float4 rows a bone in absolute world space.
		 * Render thread only.
		 */
		inline void UpdateSkin(RE::NiSkinInstance* a_skin, const RE::NiTransform& a_world)
		{
			using UpdateSkinInstance = void (*)(RE::NiSkinInstance*, const RE::NiTransform*);
			static const REL::Relocation<UpdateSkinInstance> updateSkinInstance{ REL::Offset(0xe4ff90) };
			updateSkinInstance(a_skin, &a_world);
		}

		/**
		 * @brief FadeWatch: the fade nodes whose BSFadeNode::currentFade or LOD level (+0x152 & 0xF, LodLevelUpdate) changed, pushed
		 * from the engine's writers.
		 *
		 * currentFade is written by the cull (AE 1.6.1170): BSFadeNode::OnVisible (0x141479f50, which every fade node
		 * class reaches; BSLeafAnimNode::OnVisible calls it) writes it directly for one LOD mode and through the fade
		 * update FUN_14147a160 otherwise, and FUN_1402cff60 calls that update outside a cull. Both are detoured; each
		 * compares the value before and after and pushes the node when it moved. Cull job threads push, the render
		 * thread drains (SceneStore::ProcessEvents). A few tens a frame while the camera moves, none at rest.
		 */
		/**
		 * @brief CS_DCLF_DECAL_ORDER_PROBE (DecalOrder.cpp): the main registration's decal chains against the order the scene lists
		 * give them. Render thread, once a frame, with the drained capture.
		 */
		void ProbeDecalOrder(std::span<const PassCapture::Entry> a_entries, const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>& a_main);

		inline EventQueue<const RE::BSFadeNode*> fadeEvents;
		constexpr std::size_t kMaxFadeChanges = 1u << 16;

		inline float CurrentFade(RE::BSFadeNode* a_node)
		{
			return a_node->GetRuntimeData().currentFade;
		}

		// SceneStore::MuteFadeEvents: this thread runs the engine's fade functions on a copy of a node.
		inline thread_local bool fadeEventsMuted = false;
		inline void PushFade(const RE::BSFadeNode* a_node)
		{
			if (!fadeEventsMuted)
				fadeEvents.Push(a_node);
		}

		inline void DrainFadeEvents(std::vector<const RE::BSFadeNode*>& a_out)
		{
			fadeEvents.Drain([&](const RE::BSFadeNode* a_node) { a_out.push_back(a_node); });
		}

		/**
		 * @brief Placement snaps: FUN_14147aa20 (node, camera), from the cell attach (FUN_1402d1280, FUN_1402d5090), sets a fade
		 * node's fade, its faded-in flag, its snap radius (+0x134) and its last visible frame from the camera. A root listed
		 * before its snap (a dynamic reference's) would keep the state it was listed with on the GPU: its row is taken from
		 * the node again (SceneStore::ReseedFadeRoot). Identity only; the drain follows the attach and detach events.
		 */
		inline EventQueue<const void*> fadeSnapEvents;

		/**
		 * @brief SceneEvents: the delta walk's structural events (dclf-event-driven-tables.md, "Phase 3"), pushed
		 * from the engine's writers on whichever thread runs them, drained by the render thread (ProcessEvents).
		 *
		 * - A property event: BSShaderProperty::SetFlags (0x14147bee0) or SetMaterial (0x14147bff0) changed a shader
		 *   property, or a controller was added to a property. It carries the pointer as a key only: the drain looks it
		 *   up among the tracked entries' properties and never dereferences it.
		 * - A node event: Havok wrote a node's transform from its rigid body (FUN_140ea55a0, which every collision object
		 *   class's SetNodeTransformsFromWorldTransform and the island activation listener call), or a controller was
		 *   added to a node (NiObjectNET::PrependController, FUN_140d268d0, which NiTimeController::SetTarget calls). It
		 *   holds a reference, as SceneTracker's attach events do, because the drain walks the node's subtree and its
		 *   ancestors.
		 */
		inline EventQueue<const void*> propertyEvents;
		inline EventQueue<RE::NiPointer<RE::NiAVObject>> nodeEvents;
		constexpr std::size_t kMaxStructuralEvents = 1u << 16;

		inline void PushProperty(const void* a_property) { propertyEvents.Push(a_property); }

		inline void PushNode(RE::NiAVObject* a_node)
		{
			if (a_node)
				nodeEvents.Push(RE::NiPointer<RE::NiAVObject>(a_node));
		}

		/**
		 * @brief LOD fade events: the Lighting properties whose specularLODFade or envmapLODFade (+0x100, +0x104) changed.
		 * GetRenderPasses (BSLightingShaderProperty vtable slot 0x2A) writes them from the fade node's LOD metric
		 * (skyrim-engine-notes.md, "LOD fades in GetRenderPasses") whenever any view registers the object, which can be
		 * after the accumulate phase sampled its patch or while it is a resident. The detour compares the two floats
		 * around the call and pushes the property when either moved; RefreshFrameConstants resamples its dependents'
		 * shading. Cull and accumulation job threads push, the render thread drains.
		 */
		inline EventQueue<const void*> lodFadeEvents;
		inline bool lodFadeEventsInstalled = false;

		inline void DrainLodFadeEvents(std::vector<const void*>& a_out)
		{
			lodFadeEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
		}

		/**
		 * @brief Emittance events: the shared colours external emittance reads, when they change. A Lighting property with
		 * kExternalEmittance has its emissiveColor pointed (FUN_141480fc0, from the cell attach FUN_1402d2f60) at its
		 * reference's emittance source: a light's colour (TESObjectLIGH +0x118, constant), a region's emittanceColor
		 * (TESRegion +0x40, which the cell's emittance update FUN_1402b4390 blends from the region's weather) or the sky's
		 * colour for sky-lit movable statics (Sky +0x9c of its colours, from Sky's colour update FUN_14040ba70). Both
		 * updates write through Sky::SetColor (0x14040d970), whose detour pushes the colour when it moved; the colour is a
		 * key in propertyDependents (ListDependents), and RefreshFrameConstants resamples its dependents' shading as it does
		 * for a LOD fade event. The main thread and the cell update jobs push, the render thread drains.
		 */
		inline EventQueue<const void*> emittanceEvents;

		inline void DrainEmittanceEvents(std::vector<const void*>& a_out)
		{
			emittanceEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
		}

		inline void DrainPropertyEvents(std::vector<const void*>& a_out)
		{
			propertyEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
		}

		inline void DrainNodeEvents(std::vector<RE::NiPointer<RE::NiAVObject>>& a_out)
		{
			nodeEvents.Drain([&](RE::NiPointer<RE::NiAVObject>&& a_node) { a_out.push_back(std::move(a_node)); });
		}

		/**
		 * @brief MoveEvents: what the engine moved, as keys (never dereferenced): a reference whose 3D an update pass
		 * took, or a category node whose subtree one did (dclf-event-driven-tables.md, "Movers by event"). The light path
		 * places a mover only when its key had an event this frame or the last (SceneStore::MoveGated). Pushed after the
		 * engine's write, from the animation and cell jobs' threads; drained by the render thread at the delta walk.
		 * - Actors: FUN_14066afa0, which every animation path starts with (RunOneActorAnimationUpdateJob,
		 *   Actor::UpdateAnimation, PlayerCharacter::UpdateAnimation, Actor::FinishLoadGame); TESObjectREFR::Update3DPosition
		 *   (0x1402d9800, which Actor::Update3DPosition calls); Actor::UpdateActor3DPosition (0x14069eb80); the sky cell's
		 *   skin job (FUN_140663430); the ragdoll job (FUN_140770dc0), for every actor on its list.
		 * - Other references: a cell's animated references' update pass (UpdateSelectedDownwardPass on the reference's 3D,
		 *   called from FUN_1402b41a0 only for those in a visible room or fading), the cell's dynamic node's (FUN_1402b3ae0,
		 *   keyed by the node), and the graph-animated references' (FUN_1402f75a0, from UpdateAnimationJob).
		 * - The node events (Havok's node writes and controller additions) name the reference above the node.
		 */
		inline EventQueue<const void*> moveEvents;
		// Calls per writer (the order of kMoveWriterNames), for the report.
		inline std::array<std::atomic<std::uint64_t>, 8> moveWriterCalls{};
		constexpr std::array<const char*, 8> kMoveWriterNames{ "animation", "reference 3D position", "actor 3D position", "sky cell skin", "ragdoll",
			"graph animation", "update pass", "" };
		inline void CountMove(std::size_t a_writer) { moveWriterCalls[a_writer].fetch_add(1, std::memory_order_relaxed); }

		/**
		 * @brief HiddenEvents: a node whose kHidden bit (NiAVObject::flags bit 0) a store changed, as a key (never
		 * dereferenced), from the patched stores (HiddenStores.cpp) on whichever thread runs them; drained by the render
		 * thread at the delta walk. An actor entry's frame verdict is taken again only on the frame a node on its chain
		 * had one (SceneStore::hiddenDependents), and on parity frames for all of them (the witness).
		 */
		inline EventQueue<const void*> hiddenEvents;
		inline bool hiddenEventsInstalled = false;
		bool InstallHiddenStores();
		/** @brief The patched store (its address in the image) whose stub holds a_address, or 0 (HiddenWatch's report). */
		std::uintptr_t HiddenStoreSiteOf(std::uintptr_t a_address);

		inline void PushMove(const void* a_key)
		{
			if (a_key)
				moveEvents.Push(a_key);
		}

		/**
		 * @brief SwitchEvents: an NiSwitchNode's selection may have changed (dclf-cull-job-elimination.md, "Phase 3"),
		 * pushed from the writer's thread, drained at ProcessEvents and applied by the next walk (ApplySwitchEvents).
		 *
		 * NiSwitchNode::index (+0x12C) has no setter. Its only stores outside construction, cloning and loading (AE
		 * 1.6.1170, every `mov [reg+0x12c]` in .text) are:
		 * - BSTreeManager's LOD selection in Main::Update (FUN_140437e50, five stores; FUN_140438840, two) and the local
		 *   map's (FUN_140438580, one), on a tree's LOD switch (BSTreeNode +0x180). They leave the new child as it was:
		 *   NiSwitchNode::OnVisible brings it up to date in the cull.
		 * - Harvesting (FUN_1401e8ef0, from TESObjectTREE::Activate and the flora's) and a harvestable's 3D setup
		 *   (FUN_1401e9450), on the switch under the reference's root. Both update the switch right after.
		 * Each store is patched with a call to a stub (SwitchStoreStubs) that makes the store itself and pushes an event
		 * with the index before it when the value changed. The tree manager can store twice in one pass (a level, then
		 * the far level), so the event is judged by the net change when applied.
		 *
		 * NiSwitchNode's AttachChild, DetachChild and SetAt reset revID to 1, which leaves the selected child out of date
		 * without a store to the index, and DetachChild clears the index when the selected slot empties: those push a
		 * structural event.
		 */
		struct SwitchEvent
		{
			RE::NiPointer<RE::NiAVObject> node;
			std::int32_t before = 0;
			bool structural = false;
		};
		inline EventQueue<SwitchEvent> switchEvents;
		constexpr std::size_t kSwitchIndex = 0x12C;
		constexpr std::size_t kMaxSwitchChanges = 1u << 14;

		inline std::int32_t& SwitchIndexOf(RE::NiAVObject* a_switch)
		{
			return *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::byte*>(a_switch) + kSwitchIndex);
		}

		// The render thread (Skyrim's main thread), recorded at the first ProcessEvents. A switch event is taken only there:
		// a loader thread builds subtrees that are not in the scene yet (their attach brings the switches up to date,
		// AddSubtree), and a reference taken to a node a loader is still assembling, released later on another thread,
		// is not safe (a QueuedTree load crashed on a freed child under a tree's switch with these events taken there).
		inline std::atomic<std::uint32_t> switchEventThread{ 0 };

		inline void PushSwitch(RE::NiAVObject* a_switch, std::int32_t a_before, bool a_structural)
		{
			if (a_switch && ::GetCurrentThreadId() == switchEventThread.load(std::memory_order_relaxed))
				switchEvents.Push(SwitchEvent{ RE::NiPointer<RE::NiAVObject>(a_switch), a_before, a_structural });
		}

		/** @brief The patched stores' handler (SwitchStoreStubs): the store, and an event when it changed the index. */
		inline void SwitchIndexStore(RE::NiAVObject* a_switch, std::int32_t a_index)
		{
			auto& index = SwitchIndexOf(a_switch);
			const std::int32_t before = index;
			index = a_index;
			if (before != a_index)
				PushSwitch(a_switch, before, false);
		}

		/** @brief After one of NiSwitchNode's own child edits (its vtable's implementations). */
		inline void PushSwitchStructural(RE::NiNode* a_switch)
		{
			PushSwitch(a_switch, SwitchIndexOf(a_switch), true);
		}

		inline bool switchEventsInstalled = false;
		inline bool moveEventsInstalled = false;

		/** @brief Patches the stores (after checking every site's bytes; none is patched when one differs). */
		bool InstallSwitchStores();

		/** @brief Takes a geometry off a dependents list; true when others remain under the key. */
		template <class Map, class Key>
		bool Unlist(Map& a_map, Key a_key, RE::BSGeometry* a_geometry)
		{
			const auto it = a_map.find(a_key);
			if (it == a_map.end())
				return false;
			auto& list = it->second;
			if (const auto at = std::find(list.begin(), list.end(), a_geometry); at != list.end()) {
				*at = list.back();
				list.pop_back();
			}
			if (list.empty()) {
				a_map.erase(it);
				return false;
			}
			return true;
		}

		inline bool SameTransform(const RE::NiTransform& a_lhs, const RE::NiTransform& a_rhs)
		{
			return std::memcmp(&a_lhs, &a_rhs, sizeof(RE::NiTransform)) == 0;
		}

		/** @brief A rigid body on this node whose motion type is not kFixed: Havok moves it. */
		inline bool NonFixedBody(const RE::NiAVObject& a_object)
		{
			auto* collision = a_object.collisionObject.get();
			auto* ni = collision ? collision->AsBhkNiCollisionObject() : nullptr;
			if (!ni || !ni->body || !ni->body->GetRTTI() || !std::strstr(ni->body->GetRTTI()->GetName(), "RigidBody"))
				return false;
			auto* entity = static_cast<RE::hkpEntity*>(static_cast<RE::hkReferencedObject*>(ni->body->referencedObject.get()));
			return entity && entity->motion.type.get() != RE::hkpMotion::MotionType::kFixed;
		}
		inline ObjectRecord FreeObjectRecord()
		{
			ObjectRecord record{};
			record.flags = kObjectFree | kObjectNoBindings | kObjectNoShadow;
			return record;
		}
		// What a node between a leaf and its category node makes of the leaf. An ordered node depends on draw
		// order, which does not survive being drawn out of the native loop. A billboard turns to the camera in
		// the main cull (NiBillboardNode::OnVisible), after the scene walk read its world transform. A switch
		// node draws one child at a time: Switch marks the leaf for the per-frame test (SwitchSelects).
		inline Ineligible ParentReason(RE::NiNode* a_node)
		{
			if (a_node->AsSwitchNode())
				return Ineligible::Switch;
			if (netimmerse_cast<RE::BSOrderedNode*>(a_node))
				return Ineligible::UnsupportedParent;
			if (netimmerse_cast<RE::NiBillboardNode*>(a_node))
				return Ineligible::Billboard;
			return Ineligible::None;
		}

		// NiSkinPartition::Unk_25 (AE 140d43a10) draws partition i when this table holds a non-zero byte at
		// (LODMode.index + LODMode.singleLevel * 4) * 3 + the partition's LOD byte (Partition+0x42). Read from
		// AE 1.6.1170 at 0x14202a030, where nothing writes it: level n draws the LOD bytes below n, and
		// single-level n draws LOD byte n alone.
		constexpr std::array<std::uint8_t, 24> kPartitionLodTable{ 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
		// The stronger of two parent reasons: an unsupported parent outranks a billboard, which outranks a
		// switch (the only one decided per frame).
		inline Ineligible CombineParentReasons(Ineligible a_lhs, Ineligible a_rhs)
		{
			for (const auto reason : { Ineligible::UnsupportedParent, Ineligible::Billboard, Ineligible::Switch })
				if (a_lhs == reason || a_rhs == reason)
					return reason;
			return Ineligible::None;
		}
		// The frame's globals (kPSFrameGeometry, kVSEyePosition: LightingConstants.h). EyePosition is written only by Envmap,
		// Eye and technique 0x10; every other technique leaves whatever the constant buffer last held there, which no draw
		// of it reads.
		static_assert(std::tuple_size_v<decltype(SceneStore::Tables::frameLighting)> == std::tuple_size_v<FrameLighting>);
		// What ObjectGeometryConstants writes over the pipeline's block for every object (or every object of the pipeline's
		// kind): World, PreviousWorld, LandBlendParams, TreeParams, WindTimers, TextureProj; the light assignment Light
		// Limit Fix never reads, MaterialData, EmitColor, ShadowLightMaskSelect, ProjectedUVParams 1-3, SSRParams.
		constexpr std::uint64_t kObjectGeometryVS = (1ull << 0) | (1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6);
		constexpr std::uint64_t kObjectGeometryPS = (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 7) | (1ull << 8) | (1ull << 10) | (1ull << 12) |
		                                            (1ull << 13) | (1ull << 14) | (1ull << 16);

		/** @brief The frame variables of a_sample written over a_out's where both write them; true when any differed. */
		inline bool CopyFrameGeometry(const GeometryConstants& a_sample, GeometryConstants& a_out)
		{
			const auto& layout = LightingPSLayout();
			bool changed = false;
			for (const auto v : kPSFrameGeometry) {
				const std::uint32_t offset = layout.offset[v];
				const std::size_t bytes = layout.size[v] * sizeof(float);
				if (!a_out.ps.Written(offset) || !a_sample.ps.Written(offset) || std::memcmp(&a_out.ps.floats[offset], &a_sample.ps.floats[offset], bytes) == 0)
					continue;
				std::memcpy(&a_out.ps.floats[offset], &a_sample.ps.floats[offset], bytes);
				changed = true;
			}
			return changed;
		}

		/** @brief a_sample's EyePosition over a_out's where both write it; true when it differed. */
		inline bool CopyEyePosition(const GeometryConstants& a_sample, GeometryConstants& a_out)
		{
			const auto& layout = LightingVSLayout();
			const std::uint32_t offset = layout.offset[kVSEyePosition];
			const std::size_t bytes = layout.size[kVSEyePosition] * sizeof(float);
			if (!a_out.vs.Written(offset) || !a_sample.vs.Written(offset) || std::memcmp(&a_out.vs.floats[offset], &a_sample.vs.floats[offset], bytes) == 0)
				return false;
			std::memcpy(&a_out.vs.floats[offset], &a_sample.vs.floats[offset], bytes);
			return true;
		}
		inline float GlobalFloatAt(const REL::Relocation<std::uintptr_t>& a_at)
		{
			return *reinterpret_cast<const float*>(a_at.address());
		}
		// Grows each column of a table by one slot (Tables::GeometryColumns and the like).
		inline constexpr auto kGrowColumn = [](auto& a_column, auto&&... a_initial) { a_column.emplace_back(a_initial...); };
		/**
		 * @brief Whether an ineligibility verdict is one the scene phase cannot reach on its own.
		 *
		 * The decal rule is the only one: the group a decal draws in comes from its accumulated pass's
		 * accumulation hint (LightingDescriptors, the engine's GetRenderPasses), which does not exist
		 * before the shadow maps. Rejecting on it in the scene phase would leave every decal the main
		 * pass draws without a record, so the record is built and the verdict is taken again, with the
		 * pass, by the accumulate phase.
		 */
		inline bool DeferredToAccumulate(Ineligible a_reason)
		{
			return a_reason == Ineligible::Decal;
		}

		/**
		 * @brief Whether an object the main pass cannot take is still a shadow caster DCLF can draw: its reason is
		 * one the shadow views are indifferent to, and the engine would draw it into a shadow map. Measured
		 * (the engine's cascade draws with DCLF on) and reverse engineered per reason:
		 * - Technique: the lighting technique is outside the main pass's set (the terrain's landscape blocks);
		 *   a shadow view draws the Utility technique, which ShadowUtilityTechnique derives from the property.
		 * - UnsupportedParent: under a BSOrderedNode (hay), which orders blended draws and nothing else.
		 * Not a billboard: NiBillboardNode turns to the culling camera, which for a shadow view is the light's.
		 */
		inline bool ShadowOnlyReason(Ineligible a_reason)
		{
			return a_reason == Ineligible::Technique || a_reason == Ineligible::UnsupportedParent;
		}

		inline bool ShadowOnlyCaster(Ineligible a_reason, RE::BSGeometry& a_geometry)
		{
			if (!ShadowOnlyReason(a_reason))
				return false;
			// Its main pass is the engine's, so its fade root is never stood in: the node is its state.
			const auto reject = ShadowCasterReject(a_geometry.GetGeometryRuntimeData().shaderProperty.get(), &a_geometry, false);
			return reject == ShadowReject::None || reject == ShadowReject::VolumetricOnly;
		}
		// Only columns the accumulator patch below can change. It never writes placement, geometry,
		// bones, wetness or shadow inputs. Extras contents are unchanged unless their offset changes
		// (allocate/free), which already invalidates them. Skin-mask changes are noted before the patch.
		struct AccumulateSnapshot
		{
			std::uint32_t flags, material, pipeline, drawPipeline, extras;
			ObjectShading shading;
			ObjectLights lights;
			ObjectTreeAnim tree;
			float emissive, fade;
			std::uint8_t resident;

			AccumulateSnapshot(const SceneStore::Tables& a_tables, std::uint32_t a_slot) :
				flags(a_tables.objects[a_slot].flags), material(a_tables.objects[a_slot].materialIndex),
				pipeline(a_tables.objects[a_slot].pipelineIndex), drawPipeline(a_tables.draws[a_slot].pipelineIndex),
				extras(a_tables.extraOffset[a_slot]), shading(a_tables.shading[a_slot]), lights(a_tables.lights[a_slot]),
				tree(a_tables.treeAnim[a_slot]), emissive(a_tables.emissiveMult[a_slot]), fade(a_tables.fadeDistance[a_slot]),
				resident(a_tables.residentSlot[a_slot])
			{}

			void NoteWrite(SceneStore::Tables& a_tables, std::uint32_t a_slot) const
			{
				auto differs = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) != 0; };
				const auto& object = a_tables.objects[a_slot];
				std::uint32_t causes = 0;
				if (flags != object.flags || material != object.materialIndex || pipeline != object.pipelineIndex ||
					drawPipeline != a_tables.draws[a_slot].pipelineIndex || differs(fade, a_tables.fadeDistance[a_slot]))
					causes |= kChangeBindings;
				if (differs(shading, a_tables.shading[a_slot]) || differs(emissive, a_tables.emissiveMult[a_slot]))
					causes |= kChangeShading;
				if (differs(lights, a_tables.lights[a_slot]))
					causes |= kChangeLights;
				if (differs(tree, a_tables.treeAnim[a_slot]))
					causes |= kChangeTree;
				if (extras != a_tables.extraOffset[a_slot])
					causes |= kChangeExtras;
				if (resident != a_tables.residentSlot[a_slot])
					causes |= kChangeMembership;
				a_tables.NoteChange(a_slot, causes);
			}
		};
	}

	// What were one translation unit's anonymous namespaces: their names resolve here as they did there.
	using namespace Scene;

	/**
	 * @brief Attributes the time since the last call to one BuildPart.
	 *
	 * Inert unless CS_DCLF_PROFILE=1: with profiling off `parts` is null and Add() is a null test, so
	 * the loop makes no clock calls. That matters because the calls themselves were ~4000 a frame,
	 * about 0.1 ms, which is 6% of what BuildFrame was being measured at.
	 *
	 * Timing a loop from inside it perturbs what it measures, so the profiled and unprofiled totals
	 * are both reported and the difference is the instrument's own cost.
	 */
	/** @brief Adds the time from construction to destruction to a scene part's sum and its frame's time (ms). */
	struct ScenePartScope
	{
		double& sum;
		double& frame;
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

		ScenePartScope(double& a_sum, double& a_frame) :
			sum(a_sum), frame(a_frame) {}
		~ScenePartScope()
		{
			const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			sum += elapsed;
			frame += elapsed;
		}
		ScenePartScope(const ScenePartScope&) = delete;
		ScenePartScope& operator=(const ScenePartScope&) = delete;
	};

	// One scene sub-zone (ScenePart) for the rest of the enclosing block: a Tracy zone and the part's sum. One per block.
#define DCLF_SCENE_PART(a_part, a_zone) \
	ZoneScopedN(a_zone);                \
	ScenePartScope scenePartScope(stats.scenePartMs[static_cast<std::size_t>(ScenePart::a_part)], \
		stats.scenePartFrameMs[static_cast<std::size_t>(ScenePart::a_part)])

	struct PartTimer
	{
		std::array<double, static_cast<std::size_t>(BuildPart::Count)>* parts = nullptr;
		std::array<double, static_cast<std::size_t>(BuildPart::Count)>* phaseParts = nullptr;
		std::chrono::steady_clock::time_point last;

		explicit PartTimer(std::array<double, static_cast<std::size_t>(BuildPart::Count)>& a_parts,
			std::array<double, static_cast<std::size_t>(BuildPart::Count)>* a_phaseParts = nullptr)
		{
			if (SceneStore::ProfileEnabled()) {
				parts = &a_parts;
				phaseParts = a_phaseParts;
				last = std::chrono::steady_clock::now();
			}
		}

		void Add(BuildPart a_part)
		{
			if (!parts)
				return;
			const auto now = std::chrono::steady_clock::now();
			const double elapsed = std::chrono::duration<double, std::milli>(now - last).count();
			(*parts)[static_cast<std::size_t>(a_part)] += elapsed;
			if (phaseParts)
				(*phaseParts)[static_cast<std::size_t>(a_part)] += elapsed;
			last = now;
		}
	};
}
