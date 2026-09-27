#include "SceneStore.h"

#include "EventQueue.h"

#include "LightingConstants.h"

#include "MaterialSources.h"

#include "EngineStates.h"

#include "Switches.h"
#include "Toggles.h"

#include "GpuResources.h"
#include "PassCapture.h"
#include "SceneTracker.h"
#include "ShadowProbe.h"
#include "SunAccumulation.h"
#include "PrimaryCull.h"
#include "ShadowViews.h"
#include "VertexInput.h"
#include "VolumetricProbe.h"
#include "FaceSnapshots.h"

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
	namespace
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

		const RE::BSRenderPass* FindLightingPass(RE::BSShaderProperty* a_property)
		{
			for (auto* pass = a_property->renderPassList.head; pass; pass = pass->next) {
				if (pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Lighting && pass->numLights > 0 && pass->sceneLights)
					return pass;
			}
			return nullptr;
		}

		ObjectShading MakeShading(const RE::BSLightingShaderProperty& a_property, const LightingDescriptors& a_descriptors, std::uint32_t a_renderFlags,
			float& a_emissiveMult)
		{
			// BSLightingShader::SetupGeometry (engine notes): which components it writes depends on the pass.
			const float unwritten = std::bit_cast<float>(kUnwrittenBits);
			const bool specular = (a_descriptors.pass & kSpecularBit) != 0;
			ObjectShading shading{};
			shading.materialData[0] = a_descriptors.technique == kTechniqueEnvmap ? a_descriptors.envmapLODFade : unwritten;
			shading.materialData[1] = specular ? a_descriptors.specularLODFade : unwritten;
			shading.materialData[2] = a_property.alpha;
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

		void StoreTransform(const RE::NiTransform& a_transform, float (&a_out)[12])
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

		bool IsHidden(const RE::NiAVObject* a_object)
		{
			return a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden);
		}

		/**
		 * @brief The engine's per-frame palette update (AE FUN_140e4ff90): what the bone setter runs from the
		 * native draw an owned object no longer gets. Idempotent within a frame (frameID); it copies the current
		 * palette to the previous one first and writes three float4 rows a bone in absolute world space.
		 * Render thread only.
		 */
		void UpdateSkin(RE::NiSkinInstance* a_skin, const RE::NiTransform& a_world)
		{
			using UpdateSkinInstance = void (*)(RE::NiSkinInstance*, const RE::NiTransform*);
			static const REL::Relocation<UpdateSkinInstance> updateSkinInstance{ REL::Offset(0xe4ff90) };
			updateSkinInstance(a_skin, &a_world);
		}

		/**
		 * @brief FadeWatch: the fade nodes whose BSFadeNode::currentFade changed, pushed from the engine's writers.
		 *
		 * currentFade is written by the cull (AE 1.6.1170): BSFadeNode::OnVisible (0x141479f50, which every fade node
		 * class reaches; BSLeafAnimNode::OnVisible calls it) writes it directly for one LOD mode and through the fade
		 * update FUN_14147a160 otherwise, and FUN_1402cff60 calls that update outside a cull. Both are detoured; each
		 * compares the value before and after and pushes the node when it moved. Cull job threads push, the render
		 * thread drains (SceneStore::ProcessEvents). A few tens a frame while the camera moves, none at rest.
		 */
		EventQueue<const RE::BSFadeNode*> fadeEvents;
		constexpr std::size_t kMaxFadeChanges = 1u << 16;

		float CurrentFade(RE::BSFadeNode* a_node)
		{
			return a_node->GetRuntimeData().currentFade;
		}

		void PushFade(const RE::BSFadeNode* a_node) { fadeEvents.Push(a_node); }

		void DrainFadeEvents(std::vector<const RE::BSFadeNode*>& a_out)
		{
			fadeEvents.Drain([&](const RE::BSFadeNode* a_node) { a_out.push_back(a_node); });
		}

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
		 * @brief SceneEvents: CS_DCLF_SCENE_DELTA's structural events (dclf-event-driven-tables.md, "Phase 3"), pushed
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
		EventQueue<const void*> propertyEvents;
		EventQueue<RE::NiPointer<RE::NiAVObject>> nodeEvents;
		constexpr std::size_t kMaxStructuralEvents = 1u << 16;

		void PushProperty(const void* a_property) { propertyEvents.Push(a_property); }

		void PushNode(RE::NiAVObject* a_node)
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
		EventQueue<const void*> lodFadeEvents;
		bool lodFadeEventsInstalled = false;

		struct LodFadeRenderPasses
		{
			static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* a_property, RE::BSGeometry* a_geometry, std::uint32_t a_renderFlags,
				RE::BSShaderAccumulator* a_accumulator)
			{
				const float specular = a_property->specularLODFade;
				const float envmap = a_property->envmapLODFade;
				auto* passes = func(a_property, a_geometry, a_renderFlags, a_accumulator);
				if (std::bit_cast<std::uint32_t>(a_property->specularLODFade) != std::bit_cast<std::uint32_t>(specular) ||
					std::bit_cast<std::uint32_t>(a_property->envmapLODFade) != std::bit_cast<std::uint32_t>(envmap))
					lodFadeEvents.Push(a_property);
				return passes;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void DrainLodFadeEvents(std::vector<const void*>& a_out)
		{
			lodFadeEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
		}

		void DrainPropertyEvents(std::vector<const void*>& a_out)
		{
			propertyEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
		}

		void DrainNodeEvents(std::vector<RE::NiPointer<RE::NiAVObject>>& a_out)
		{
			nodeEvents.Drain([&](RE::NiPointer<RE::NiAVObject>&& a_node) { a_out.push_back(std::move(a_node)); });
		}

		struct PropertySetFlags
		{
			static void thunk(RE::BSShaderProperty* a_this, RE::BSShaderProperty::EShaderPropertyFlag8 a_flag, bool a_set)
			{
				const auto before = a_this->flags.underlying();
				func(a_this, a_flag, a_set);
				if (a_this->flags.underlying() != before)
					PushProperty(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct PropertySetMaterial
		{
			static void thunk(RE::BSShaderProperty* a_this, RE::BSShaderMaterial* a_material, bool a_unique)
			{
				const auto* before = a_this->material;
				func(a_this, a_material, a_unique);
				if (a_this->material != before)
					PushProperty(a_this);
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
				if (auto* object = netimmerse_cast<RE::NiAVObject*>(a_target))
					PushNode(object);
				else
					PushProperty(a_target);
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
		EventQueue<SwitchEvent> switchEvents;
		constexpr std::size_t kSwitchIndex = 0x12C;
		constexpr std::size_t kMaxSwitchChanges = 1u << 14;

		std::int32_t& SwitchIndexOf(RE::NiAVObject* a_switch)
		{
			return *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::byte*>(a_switch) + kSwitchIndex);
		}

		// The render thread (Skyrim's main thread), recorded at the first ProcessEvents. A switch event is taken only there:
		// a loader thread builds subtrees that are not in the scene yet (their attach brings the switches up to date,
		// AddSubtree), and a reference taken to a node a loader is still assembling, released later on another thread,
		// is not safe (a QueuedTree load crashed on a freed child under a tree's switch with these events taken there).
		std::atomic<std::uint32_t> switchEventThread{ 0 };

		void PushSwitch(RE::NiAVObject* a_switch, std::int32_t a_before, bool a_structural)
		{
			if (a_switch && ::GetCurrentThreadId() == switchEventThread.load(std::memory_order_relaxed))
				switchEvents.Push(SwitchEvent{ RE::NiPointer<RE::NiAVObject>(a_switch), a_before, a_structural });
		}

		/** @brief The patched stores' handler (SwitchStoreStubs): the store, and an event when it changed the index. */
		void SwitchIndexStore(RE::NiAVObject* a_switch, std::int32_t a_index)
		{
			auto& index = SwitchIndexOf(a_switch);
			const std::int32_t before = index;
			index = a_index;
			if (before != a_index)
				PushSwitch(a_switch, before, false);
		}

		/** @brief After one of NiSwitchNode's own child edits (its vtable's implementations). */
		void PushSwitchStructural(RE::NiNode* a_switch)
		{
			PushSwitch(a_switch, SwitchIndexOf(a_switch), true);
		}

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

		bool switchEventsInstalled = false;

		/** @brief Patches the stores (after checking every site's bytes; none is patched when one differs). */
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

		bool SameTransform(const RE::NiTransform& a_lhs, const RE::NiTransform& a_rhs)
		{
			return std::memcmp(&a_lhs, &a_rhs, sizeof(RE::NiTransform)) == 0;
		}

		/** @brief A rigid body on this node whose motion type is not kFixed: Havok moves it. */
		bool NonFixedBody(const RE::NiAVObject& a_object)
		{
			auto* collision = a_object.collisionObject.get();
			auto* ni = collision ? collision->AsBhkNiCollisionObject() : nullptr;
			if (!ni || !ni->body || !ni->body->GetRTTI() || !std::strstr(ni->body->GetRTTI()->GetName(), "RigidBody"))
				return false;
			auto* entity = static_cast<RE::hkpEntity*>(static_cast<RE::hkReferencedObject*>(ni->body->referencedObject.get()));
			return entity && entity->motion.type.get() != RE::hkpMotion::MotionType::kFixed;
		}

		/** @brief CS_DCLF_ASYNC=probe: the scene walk's per-object arrays, two walks compared byte for byte. */
		bool SameSceneTables(const SceneStore::Tables& a, const SceneStore::Tables& b, std::string& a_difference)
		{
			auto differs = [&](const char* a_name, const auto& a_lhs, const auto& a_rhs) {
				using T = std::remove_cvref_t<decltype(*a_lhs.data())>;
				const std::size_t lhsBytes = a_lhs.size() * sizeof(T), rhsBytes = a_rhs.size() * sizeof(T);
				if (lhsBytes != rhsBytes) {
					a_difference = fmt::format("{}: {} vs {} entries", a_name, a_lhs.size(), a_rhs.size());
					return true;
				}
				if (lhsBytes && std::memcmp(a_lhs.data(), a_rhs.data(), lhsBytes) != 0) {
					const auto* l = reinterpret_cast<const std::uint8_t*>(a_lhs.data());
					const auto* r = reinterpret_cast<const std::uint8_t*>(a_rhs.data());
					std::size_t k = 0;
					while (l[k] == r[k])
						++k;
					a_difference = fmt::format("{}: entry {} of {} (byte {} of the entry)", a_name, k / sizeof(T), a_lhs.size(), k % sizeof(T));
					return true;
				}
				return false;
			};
			return !(differs("objects", a.objects, b.objects) || differs("object geometry", a.objectGeometry, b.objectGeometry) ||
					 differs("draws", a.draws, b.draws) || differs("skin partitions", a.skinPartitions, b.skinPartitions) || differs("bones", a.bones, b.bones) || differs("previous bones", a.previousBones, b.previousBones) ||
					 differs("bone offsets", a.boneOffset, b.boneOffset) || differs("bone rows", a.boneRows, b.boneRows) ||
					 differs("shadow techniques", a.shadowTechnique, b.shadowTechnique) || differs("shadow rejects", a.shadowReject, b.shadowReject) ||
					 differs("sky techniques", a.skyTechnique, b.skyTechnique) || differs("sky keys", a.skyKeysUsed, b.skyKeysUsed) ||
					 differs("sun entries", a.sunEntry, b.sunEntry) || differs("face streams", a.faceStream, b.faceStream) ||
					 differs("shadow diffuse", a.shadowDiffuse, b.shadowDiffuse) || differs("shadow materials", a.shadowMaterial, b.shadowMaterial) || differs("shadow keys", a.shadowKeysUsed, b.shadowKeysUsed) ||
					 differs("shadow textures", a.shadowTextureSet, b.shadowTextureSet) || differs("extra offsets", a.extraOffset, b.extraOffset) ||
					 differs("geometry slots used", a.geometryLastUsed, b.geometryLastUsed));
		}
	}

	namespace
	{
		ObjectRecord FreeObjectRecord()
		{
			ObjectRecord record{};
			record.flags = kObjectFree | kObjectNoBindings | kObjectNoShadow;
			return record;
		}
	}

	void SceneStore::Tables::GrowObjects(std::size_t a_count)
	{
		if (a_count <= objects.size())
			return;
		objects.resize(a_count, FreeObjectRecord());
		objectGeometry.resize(a_count, nullptr);
		shading.resize(a_count, ObjectShading{});
		emissiveMult.resize(a_count, 1.0f);
		lights.resize(a_count, ObjectLights{});
		treeAnim.resize(a_count, ObjectTreeAnim{});
		skinWetness.resize(a_count, std::array<float, 4>{});
		skinPartitions.resize(a_count, 0);
		draws.resize(a_count, DrawSequence{});
		boneOffset.resize(a_count, 0);
		boneRows.resize(a_count, 0);
		extraOffset.resize(a_count, kNoExtraRows);
		shadowTechnique.resize(a_count, 0);
		shadowReject.resize(a_count, 0);
		skyTechnique.resize(a_count, 0);
		sunEntry.resize(a_count, std::array<float, 4>{});
		fadeDistance.resize(a_count, 0.0f);
		residentSlot.resize(a_count, 0);
		faceStream.resize(a_count, kNoFaceStream);
		shadowDiffuse.resize(a_count, nullptr);
		shadowMaterial.resize(a_count, nullptr);
		objectSeen.resize(a_count, 0);
		sceneFlags.resize(a_count, FreeObjectRecord().flags);
	}

	SceneStore::Tables::Columns SceneStore::Tables::ColumnsOf(std::uint32_t a_slot) const
	{
		Columns columns;
		if (a_slot >= objects.size())
			return columns;
		columns.object = objects[a_slot];
		columns.draw = draws[a_slot];
		columns.shading = shading[a_slot];
		columns.lights = lights[a_slot];
		columns.tree = treeAnim[a_slot];
		columns.wetness = skinWetness[a_slot];
		columns.sunEntry = sunEntry[a_slot];
		columns.extraOffset = extraOffset[a_slot];
		if (columns.extraOffset != kNoExtraRows && (std::size_t(columns.extraOffset) + kExtraRows) * 4 <= extraRows.size())
			std::memcpy(columns.extras.data(), &extraRows[std::size_t(columns.extraOffset) * 4], sizeof(columns.extras));
		columns.geometry = objectGeometry[a_slot];
		columns.shadowDiffuse = shadowDiffuse[a_slot];
		columns.shadowMaterial = shadowMaterial[a_slot];
		columns.emissiveMult = emissiveMult[a_slot];
		columns.fadeDistance = fadeDistance[a_slot];
		columns.boneOffset = boneOffset[a_slot];
		columns.boneRows = boneRows[a_slot];
		columns.shadowTechnique = shadowTechnique[a_slot];
		columns.skyTechnique = skyTechnique[a_slot];
		columns.faceStream = faceStream[a_slot];
		columns.boneCapacity = (columns.boneRows || columns.extraOffset != kNoExtraRows) ? BoneCapacity() : 0u;
		columns.sceneFlags = sceneFlags[a_slot];
		columns.skinPartitions = skinPartitions[a_slot];
		columns.shadowReject = shadowReject[a_slot];
		columns.resident = residentSlot[a_slot];
		return columns;
	}

	std::uint32_t SceneStore::Tables::CausesBetween(const Columns& a, const Columns& b)
	{
		auto same = [](const auto& a_left, const auto& a_right) { return std::memcmp(&a_left, &a_right, sizeof(a_left)) == 0; };
		std::uint32_t causes = 0;
		const auto& x = a.object;
		const auto& y = b.object;
		if (!same(x.world, y.world) || !same(x.previousWorld, y.previousWorld) || !same(x.boundCenter, y.boundCenter) || !same(x.boundRadius, y.boundRadius) ||
			!same(a.sunEntry, b.sunEntry))
			causes |= kChangePlacement;
		if (x.flags != y.flags || x.materialIndex != y.materialIndex || x.pipelineIndex != y.pipelineIndex || a.draw.pipelineIndex != b.draw.pipelineIndex ||
			!same(a.fadeDistance, b.fadeDistance) || a.sceneFlags != b.sceneFlags)
			causes |= kChangeBindings;
		if (!same(a.shading, b.shading) || !same(a.emissiveMult, b.emissiveMult) || !same(a.wetness, b.wetness))
			causes |= kChangeShading;
		if (!same(a.lights, b.lights))
			causes |= kChangeLights;
		if (!same(a.tree, b.tree))
			causes |= kChangeTree;
		if (a.skinPartitions != b.skinPartitions || a.boneOffset != b.boneOffset || a.boneRows != b.boneRows)
			causes |= kChangeSkin;
		if (a.boneCapacity != b.boneCapacity)
			causes |= (a.boneRows || b.boneRows ? kChangeSkin : 0u) | (a.extraOffset != kNoExtraRows || b.extraOffset != kNoExtraRows ? kChangeExtras : 0u);
		if (a.extraOffset != b.extraOffset || !same(a.extras, b.extras))
			causes |= kChangeExtras;
		if (a.shadowTechnique != b.shadowTechnique || a.shadowReject != b.shadowReject || a.skyTechnique != b.skyTechnique || a.shadowDiffuse != b.shadowDiffuse ||
			a.shadowMaterial != b.shadowMaterial)
			causes |= kChangeShadow;
		// The draw template's geometry half: everything but the pipeline index, which is the bindings'.
		auto geometryHalf = [](DrawSequence a_draw) {
			a_draw.pipelineIndex = 0;
			return a_draw;
		};
		if (x.geometryIndex != y.geometryIndex || !same(geometryHalf(a.draw), geometryHalf(b.draw)) || a.faceStream != b.faceStream || a.geometry != b.geometry)
			causes |= kChangeGeometry;
		if (a.resident != b.resident)
			causes |= kChangeMembership;
		return causes;
	}

	std::uint32_t SceneStore::Tables::PlaceBones(std::uint32_t a_slot, std::uint32_t a_rows)
	{
		if (a_rows && boneRows[a_slot] == a_rows)
			return boneOffset[a_slot];
		FreeBones(a_slot);
		auto& free = boneFree[std::min<std::size_t>(a_rows / 3, boneFree.size() - 1)];
		std::uint32_t offset;
		if (!free.empty()) {
			offset = free.back();
			free.pop_back();
		} else {
			offset = boneTop;
			boneTop += a_rows;
			if (boneTop > BoneCapacity()) {
				const std::size_t grown = (std::size_t(boneTop) + kBoneGrowRows - 1) / kBoneGrowRows * kBoneGrowRows;
				bones.resize(grown * 4, 0.0f);
				previousBones.resize(grown * 4, 0.0f);
				// Every record's previous palette and extras are addressed past the capacity: all of them moved.
				for (std::uint32_t slot = 0; slot < objects.size(); ++slot)
					NoteChange(slot, (boneRows[slot] ? kChangeSkin : 0u) | (extraOffset[slot] != kNoExtraRows ? kChangeExtras : 0u));
			}
		}
		boneOffset[a_slot] = offset;
		boneRows[a_slot] = a_rows;
		return offset;
	}

	void SceneStore::Tables::FreeBones(std::uint32_t a_slot)
	{
		if (a_slot >= boneRows.size())
			return;
		if (boneRows[a_slot])
			boneFree[std::min<std::size_t>(boneRows[a_slot] / 3, boneFree.size() - 1)].push_back(boneOffset[a_slot]);
		boneOffset[a_slot] = 0;
		boneRows[a_slot] = 0;
	}

	void SceneStore::Tables::ResetObject(std::uint32_t a_slot)
	{
		const auto before = ColumnsOf(a_slot);
		FreeExtras(a_slot);
		FreeBones(a_slot);
		objects[a_slot] = FreeObjectRecord();
		objectGeometry[a_slot] = nullptr;
		shading[a_slot] = ObjectShading{};
		emissiveMult[a_slot] = 1.0f;
		lights[a_slot] = ObjectLights{};
		treeAnim[a_slot] = ObjectTreeAnim{};
		skinWetness[a_slot] = {};
		skinPartitions[a_slot] = 0;
		draws[a_slot] = DrawSequence{};
		shadowTechnique[a_slot] = 0;
		shadowReject[a_slot] = 0;
		skyTechnique[a_slot] = 0;
		sunEntry[a_slot] = {};
		fadeDistance[a_slot] = 0.0f;
		residentSlot[a_slot] = 0;
		faceStream[a_slot] = kNoFaceStream;
		shadowDiffuse[a_slot] = nullptr;
		shadowMaterial[a_slot] = nullptr;
		objectSeen[a_slot] = 0;
		sceneFlags[a_slot] = FreeObjectRecord().flags;
		NoteWrite(a_slot, before);
	}

	void SceneStore::Tables::ClearFrame(bool a_keepObjects)
	{
		if (!a_keepObjects) {
			objects.clear();
			objectGeometry.clear();
			shading.clear();
			emissiveMult.clear();
			lights.clear();
			treeAnim.clear();
			skinWetness.clear();
			skinPartitions.clear();
			shadingWatch.clear();
			watchList.clear();
			draws.clear();
			boneOffset.clear();
			boneRows.clear();
			bones.clear();
			previousBones.clear();
			boneFree = {};
			boneTop = 0;
			extraOffset.clear();
			extraRows.clear();
			extraFree.clear();
			shadowTechnique.clear();
			shadowReject.clear();
			skyTechnique.clear();
			sunEntry.clear();
			fadeDistance.clear();
			residentSlot.clear();
			// Every slot is gone: the log's readers read them all again.
			InvalidateChangeLog();
			faceStream.clear();
			shadowDiffuse.clear();
			shadowMaterial.clear();
			objectSeen.clear();
			sceneFlags.clear();
			objectFree.clear();
			liveObjects = 0;
		}
		// The per-frame lists: every walk refills them, and the slots' offsets into them are rewritten with them.
		actorObjects.clear();
		decalOrdinal.clear();
		decalCount = {};
		faceStreams.clear();
		shadowTextureSet.clear();
		shadowTextureSeen.clear();
		shadowKeysUsed.clear();
		skyKeysUsed.clear();
	}

	void SceneStore::Tables::Clear()
	{
		auto clear = [](auto& a_column, auto&&...) { a_column.clear(); };
		GeometryColumns(clear);
		PipelineColumns(clear);
		MaterialColumns(clear);
		geometrySlots.Clear();
		pipelineSlots.Clear();
		materialSlots.Clear();
		frameSignatures.clear();
		materialSignatureListed.clear();
		materialFramePending.clear();
		transformWatch.clear();
		transformWatchFrame.clear();
		objects.clear();
		objectGeometry.clear();
		geometries.clear();
		geometryLog.Invalidate();
		pipelines.clear();
		materials.clear();
		materialVersion.clear();
		materialFrameVersion.clear();
		pipelineConstantsVersion.clear();
		pipelineBindingVersion.clear();
		shading.clear();
		emissiveMult.clear();
		lights.clear();
		treeAnim.clear();
		skinWetness.clear();
		actorObjects.clear();
		skinPartitions.clear();
		shadingWatch.clear();
		watchList.clear();
		geometryConstants.clear();
		geometryConstantsValid.clear();
		geometryTemplate.clear();
		geometryTemplateNative.clear();
		techniques.clear();
		techniqueRow.clear();
		permutations.clear();
		draws.clear();
		decalOrdinal.clear();
		decalCount = {};
		bones.clear();
		previousBones.clear();
		boneOffset.clear();
		boneRows.clear();
		boneFree = {};
		boneTop = 0;
		extraRows.clear();
		extraOffset.clear();
		extraFree.clear();
		shadowTechnique.clear();
		shadowReject.clear();
		skyTechnique.clear();
		sunEntry.clear();
		fadeDistance.clear();
		residentSlot.clear();
		InvalidateChangeLog();
		faceStreams.clear();
		faceStream.clear();
		shadowDiffuse.clear();
		shadowMaterial.clear();
		shadowTextureSet.clear();
		shadowTextureSeen.clear();
		shadowKeysUsed.clear();
		skyKeysUsed.clear();
		objectSeen.clear();
		sceneFlags.clear();
		objectFree.clear();
		liveObjects = 0;
	}

	SceneStore& SceneStore::Get()
	{
		static SceneStore store;
		return store;
	}

	void SceneStore::Clear()
	{
		AbandonSceneJob();
		tracked.clear();
		categoryNodes.clear();
		ResetSlotTables();
		InvalidateObjectIndices();
		// They hold raw pointers into game allocations now that they outlive the frame, so the teardown
		// paths have to drop them rather than leave them to the next BuildFrame.
		geometryIndex.clear();
		pipelineIndex.clear();
		materialIndex.clear();
		materialCache.clear();
		validationCursor = 0;
		fullEvaluation = true;
		perFrameSet.clear();
		pendingEvaluation.clear();
		accumulatePatched.clear();
		lastPatched.clear();
		fadeChanged.clear();
		fadeDependents.clear();
		propertyChanged.clear();
		nodeChanged.clear();
		dirtyRoots.clear();
		propertyDependents.clear();
		rootDependents.clear();
		rootMotion.clear();
		DropSunCandidates();
		buckets = {};
	}

	namespace
	{
		// What a node between a leaf and its category node makes of the leaf. An ordered node depends on draw
		// order, which does not survive being drawn out of the native loop. A billboard turns to the camera in
		// the main cull (NiBillboardNode::OnVisible), after the scene walk read its world transform. A switch
		// node draws one child at a time: Switch marks the leaf for the per-frame test (SwitchSelects).
		Ineligible ParentReason(RE::NiNode* a_node)
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
	}

	std::uint32_t SceneStore::LodRowOf(const RE::BSGeometry& a_geometry, const RE::BSShaderProperty* a_property)
	{
		if (!a_geometry.GetFlags().any(RE::NiAVObject::Flag::kMeshLOD) || !a_property || !a_property->fadeNode)
			return 3;
		return a_property->fadeNode->GetRuntimeData().unk152 & 0xF;
	}

	std::uint32_t SceneStore::LodRowOf(const RE::BSRenderPass& a_pass)
	{
		return a_pass.LODMode.index + (a_pass.LODMode.singleLevel ? 4u : 0u);
	}

	std::uint32_t SceneStore::SkinPartitionMask(const RE::NiSkinInstance& a_skin, std::uint32_t a_lodRow)
	{
		const auto* partition = a_skin.skinPartition.get();
		if (!partition || a_lodRow >= 8)
			return 0;
		static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
		const RE::BSDismemberSkinInstance::Data* shown = a_skin.GetRTTI() == dismemberSkinInstance.get() ?
		                                                     static_cast<const RE::BSDismemberSkinInstance&>(a_skin).GetRuntimeData().partitions :
		                                                     nullptr;
		std::uint32_t mask = 0;
		for (std::uint32_t i = 0; i < partition->numPartitions && i < kMaxSkinPartitions; ++i) {
			if (shown && !shown[i].editorVisible)
				continue;
			const std::uint32_t lodByte = partition->partitions[i].pad42 & 0xFF;
			if (lodByte <= 2 && kPartitionLodTable[a_lodRow * 3 + lodByte])
				mask |= 1u << i;
		}
		return mask;
	}

	namespace
	{
		// The stronger of two parent reasons: an unsupported parent outranks a billboard, which outranks a
		// switch (the only one decided per frame).
		Ineligible CombineParentReasons(Ineligible a_lhs, Ineligible a_rhs)
		{
			for (const auto reason : { Ineligible::UnsupportedParent, Ineligible::Billboard, Ineligible::Switch })
				if (a_lhs == reason || a_rhs == reason)
					return reason;
			return Ineligible::None;
		}
	}

	RE::NiNode* SceneStore::FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason) const
	{
		Ineligible reason = Ineligible::None;
		RE::NiNode* node = a_object ? a_object->parent : nullptr;
		for (std::uint32_t depth = 0; node && depth < kMaxParentDepth; ++depth, node = node->parent) {
			if (categoryNodes.contains(node)) {
				if (a_parentReason)
					*a_parentReason = reason;
				return node;
			}
			reason = CombineParentReasons(reason, ParentReason(node));
		}
		return nullptr;
	}

	std::uint64_t SceneStore::CategorySignature() const
	{
		// A cheap stand-in for "the set of category nodes may have changed". It has to be conservative in
		// one direction only: if it misses a change, geometry attached under a newly appeared node is
		// never tracked, because AddSubtree needs the category node to already be known. So it folds in
		// everything the refresh below reads to *find* nodes - the interior cell, the grid cells, each
		// cell's loaded data and cell3D and how many children it has, and objRoot's child count - rather
		// than just the cell pointers.
		std::uint64_t hash = 0xcbf29ce484222325ull;
		auto mix = [&hash](auto a_value) {
			hash = (hash ^ static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_value))) * 0x100000001b3ull;
		};
		auto mixCell = [&](RE::TESObjectCELL* a_cell) {
			mix(a_cell);
			if (!a_cell || !a_cell->IsAttached())
				return;
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			mix(loaded);
			RE::NiNode* cell3D = loaded ? loaded->cell3D.get() : nullptr;
			mix(cell3D);
			if (cell3D)
				hash = (hash ^ cell3D->GetChildren().size()) * 0x100000001b3ull;
		};
		if (auto* tes = RE::TES::GetSingleton()) {
			if (auto* objRoot = tes->objRoot)
				hash = (hash ^ objRoot->GetChildren().size()) * 0x100000001b3ull;
			mix(tes->objRoot);
			mix(tes->interiorCell);
			if (tes->interiorCell) {
				mixCell(tes->interiorCell);
			} else if (auto* grid = tes->gridCells) {
				const std::uint32_t count = grid->length * grid->length;
				for (std::uint32_t i = 0; i < count; ++i)
					mixCell(grid->cells[i]);
			}
		}
		return hash;
	}

	void SceneStore::RefreshCategoryNodes(bool a_force)
	{
		// This used to rebuild and diff the whole set on every Present, including an O(tracked) scan
		// whenever any node had gone. Its *content* changes only when a cell attaches or detaches, so it
		// now runs when the signature says something moved, when a detach was seen, or on a slow backstop
		// cadence in case both miss something.
		constexpr std::uint32_t kBackstopFrames = 30;
		const std::uint64_t signature = CategorySignature();
		if (!a_force && signature == categorySignature && ++categoryIdleFrames < kBackstopFrames)
			return;
		const std::uint8_t cause = a_force ? 1 : signature != categorySignature ? 0 : 2;
		categorySignature = signature;
		categoryIdleFrames = 0;

		ankerl::unordered_dense::set<RE::NiNode*> current;

		auto addCell = [&](RE::TESObjectCELL* a_cell) {
			if (!a_cell || !a_cell->IsAttached())
				return;
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			if (!loaded || !loaded->cell3D)
				return;
			auto& children = loaded->cell3D->GetChildren();
			for (auto category : kDrawnCategories) {
				const auto index = static_cast<std::uint16_t>(category);
				if (index < children.size() && children[index]) {
					if (auto* node = children[index]->AsNode())
						current.insert(node);
				}
			}
			// References the engine moves out of the category nodes into multibounds (exteriors) and the
			// portal graph's rooms (interiors), all under ObjectLODRoot (engine notes: cell multibounds and rooms).
			if (auto* multiBound = loaded->multiBoundNode.get())
				current.insert(multiBound);
			if (auto* graph = loaded->portalGraph.get()) {
				for (auto& room : graph->rooms) {
					if (room)
						current.insert(room.get());
				}
				if (auto* shared = graph->portalSharedNode.get())
					current.insert(shared);
			}
		};

		if (auto* tes = RE::TES::GetSingleton()) {
			// Multibounds: the engine moves references out of the category nodes into BSMultiBoundNodes
			// under ObjectLODRoot (TES::objRoot), in exteriors and (holding the rooms) in interiors
			// (engine notes: multibounds and rooms).
			if (auto* objRoot = tes->objRoot) {
				for (auto& child : objRoot->GetChildren()) {
					if (auto* multiBound = child ? netimmerse_cast<RE::BSMultiBoundNode*>(child.get()) : nullptr)
						current.insert(multiBound);
				}
			}
			if (tes->interiorCell) {
				addCell(tes->interiorCell);
			} else if (auto* grid = tes->gridCells) {
				const std::uint32_t count = grid->length * grid->length;
				for (std::uint32_t i = 0; i < count; ++i)
					addCell(grid->cells[i]);
			}
		}

		// Cells that went away: drop what was tracked under them.
		bool removedAny = false;
		for (auto* node : categoryNodes) {
			if (!current.contains(node)) {
				removedAny = true;
				break;
			}
		}
		if (removedAny) {
			std::vector<RE::BSGeometry*> stale;
			for (auto& [geometry, entry] : tracked) {
				if (!current.contains(entry.categoryNode))
					stale.push_back(geometry);
			}
			for (auto* geometry : stale)
				EraseTracked(geometry);
		}

		// Cells that appeared: their content was attached before the cell was, so scan it now.
		std::vector<RE::NiNode*> added;
		for (auto* node : current) {
			if (!categoryNodes.contains(node))
				added.push_back(node);
		}
		categoryNodes = std::move(current);
		std::erase_if(categoryFound, [&](const auto& a_entry) { return !categoryNodes.contains(const_cast<RE::NiNode*>(a_entry.first)); });
		const TrackSource previousSource = addSource;
		if (addSource != TrackSource::Rescan)
			addSource = TrackSource::CategoryAppeared;
		for (auto* node : added) {
			categoryFound[node] = { frame, cause };
			for (auto& child : node->GetChildren()) {
				if (child)
					AddSubtree(child.get());
			}
		}
		addSource = previousSource;
	}

	std::array<float, 4> SceneStore::SunEntryOf(Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		constexpr std::array<float, 4> kNeverTested{ 0.0f, 0.0f, 0.0f, -1.0f };
		ResolveSunEntry(a_tracked, a_geometry);
		if (!a_tracked.sunEntryNode)
			return kNeverTested;
		const auto& bound = a_tracked.sunEntryNode->worldBound;
		return { bound.center.x, bound.center.y, bound.center.z, bound.radius };
	}

	void SceneStore::ResolveSunEntry(Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		if (!a_tracked.sunEntryResolved) {
			a_tracked.sunEntryResolved = true;
			auto* geometry = const_cast<RE::BSGeometry*>(&a_geometry);
			if (auto* reference = geometry->GetUserData()) {
				// An actor's entry is its cell's container, which the full-frustum cull never tests.
				if (reference->GetFormType() == RE::FormType::ActorCharacter) {
					a_tracked.sunEntryNode = nullptr;
				} else {
					const RE::NiAVObject* root = geometry;
					for (auto* node = geometry->parent; node && node->GetUserData() == reference; node = node->parent)
						root = node;
					a_tracked.sunEntryNode = root;
				}
			} else {
				static const REL::Relocation<const RE::NiRTTI*> multiBound{ RE::BSMultiBoundNode::Ni_RTTI };
				for (auto* node = geometry->parent; node; node = node->parent)
					if (node->GetRTTI() == multiBound.get()) {
						a_tracked.sunEntryNode = node;
						break;
					}
			}
		}
	}

	void SceneStore::AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason)
	{
		const auto [it, inserted] = tracked.try_emplace(a_geometry);
		auto& entry = it->second;
		if (inserted) {
			entry.trackedFrame = frame;
			entry.trackedBy = addSource;
		}
		entry.geometry.reset(a_geometry);
		entry.categoryNode = a_categoryNode;
		entry.parentReason = a_parentReason;
		// Attached (again): its classification stands no more, and every other entry reading the same sun entry node is
		// evaluated again, since that node's bound takes this one in now (dclf-event-driven-tables.md, "Phase 3").
		entry.candidateFrame = 0;
		if (SceneDeltaEnabled()) {
			UnlistDependents(a_geometry, entry, true);
			entry.sunEntryResolved = false;
			entry.sunEntryNode = nullptr;
			ResolveSunEntry(entry, *a_geometry);
			if (entry.sunEntryNode) {
				rootDependents[entry.sunEntryNode].push_back(a_geometry);
				entry.listedRoot = entry.sunEntryNode;
				dirtyRoots.push_back(entry.sunEntryNode);
				MarkSunEntryDirty(entry.sunEntryNode);
			}
		}
		pendingEvaluation.push_back(a_geometry);
	}

	void SceneStore::AddSubtree(RE::NiAVObject* a_root)
	{
		// The attach event is drained a frame or more after it was queued, so its node can already be gone
		// (a cell transition releases the subtree). Walking it then dereferences null.
		if (!a_root)
			return;
		Ineligible reasonAbove = Ineligible::None;
		RE::NiNode* category = FindCategoryNode(a_root, &reasonAbove);
		if (!category)
			return;

		// Walk down, carrying what the nodes between the category node and the leaf make of it (ParentReason).
		std::vector<std::pair<RE::NiAVObject*, Ineligible>> stack;
		stack.emplace_back(a_root, reasonAbove);
		while (!stack.empty()) {
			auto [object, reason] = stack.back();
			stack.pop_back();
			if (!object)
				continue;
			if (auto* geometry = object->AsGeometry()) {
				AddGeometry(geometry, category, reason);
				continue;
			}
			if (auto* node = object->AsNode()) {
				// Its selected child as the cull would find it (with the switch events, nothing else does it before the
				// walk classifies it).
				if (SwitchEventsLive())
					if (auto* switchNode = node->AsSwitchNode(); switchNode && CatchUpSwitch(*switchNode))
						++delta.attachCatchUps;
				const Ineligible below = CombineParentReasons(reason, ParentReason(node));
				for (auto& child : node->GetChildren()) {
					if (child)
						stack.emplace_back(child.get(), below);
				}
			}
		}
	}

	void SceneStore::ValidateSlice()
	{
		// Safety net for missed detaches: a tracked geometry must still hang under the category node it
		// was found under. Checks a slice per frame so the cost stays flat.
		if (tracked.empty())
			return;
		std::vector<RE::BSGeometry*> stale;
		const std::size_t size = tracked.size();
		const std::size_t count = std::min(kValidationSlice, size);
		for (std::size_t i = 0; i < count; ++i) {
			validationCursor = (validationCursor + 1) % size;
			// The map's storage is a dense vector, so the cursor walks it without hashing.
			auto it = tracked.begin() + static_cast<std::ptrdiff_t>(validationCursor);
			Ineligible reason = Ineligible::None;
			if (FindCategoryNode(it->first, &reason) != it->second.categoryNode) {
				stale.push_back(it->first);
			} else if (it->second.parentReason != reason) {
				it->second.parentReason = reason;
				it->second.candidateFrame = 0;
				pendingEvaluation.push_back(it->first);
			}
		}
		for (auto* geometry : stale) {
			EraseTracked(geometry);
			++stats.validationDrops;
		}
	}

	bool SceneStore::IsLoadingScreenUp()
	{
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
	}

	void SceneStore::ProcessEvents()
	{
		switchEventThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		// Nothing here may walk the scene graph while a load screen is up. A load tears down and rebuilds
		// TES::objRoot and the cell 3D under it, and the attach events queued across it name subtrees that
		// are still being assembled; walking either gives a pointer that is stale or simply garbage. That
		// is what crashed in RefreshCategoryNodes' objRoot walk (a child that read back as
		// 0x0001000000020001) and, the day before, in AddSubtree.
		//
		// The queue is still drained, because it holds references to attached subtrees and the comment on
		// the caller is right that it must not grow while the world is not rendered - but the events are
		// discarded rather than applied, and the first frame after the load rebuilds the tracked set from
		// scratch. A load invalidates all of it anyway, so nothing is lost by not trying to track across it.
		auto& tracker = SceneTracker::Get();
		if (SceneStore::IsLoadingScreenUp()) {
			// Only the walking stops. The tracked set is deliberately left alone until the load is over:
			// dropping it here releases the game buffers the tables reference while the previous frame's
			// epoch is still in flight, and a draw then reads a freed device address. That is a
			// VK_ERROR_DEVICE_LOST on the teleport, which is exactly what happened when this branch cleared
			// eagerly. The entries hold NiPointers, so holding them across the load is the safe direction,
			// and the rescan below replaces them on a normal frame.
			rescanPending = true;
			// Claims do not survive a load. They name geometry from the cell being torn down, and a claim
			// withholds the pass from the native loop - so a stale one means nobody draws that object in
			// the new cell. The hole detector caught exactly this at a `coc`: six claimed objects went
			// undrawn on the first frame after the transition. Publishing an empty set hands everything
			// back to the native loop until DCLF has drawn it again and re-earned the claim.
			PassCapture::Get().PublishClaims(std::make_shared<const PassCapture::ClaimSet>());
			SceneTracker::FreeEvents(tracker.Drain());
			DrainFadeEvents(fadeChanged);
			fadeChanged.clear();
			DrainPropertyEvents(propertyChanged);
			propertyChanged.clear();
			DrainLodFadeEvents(propertyChanged);
			propertyChanged.clear();
			DrainNodeEvents(nodeChanged);
			nodeChanged.clear();
			// The switches are brought up to date by the rescan's walk (AddSubtree); PrimaryCull reads them all again.
			switchEvents.Discard();
			switchPending.clear();
			switchPendingIndex.clear();
			switchResync = true;
			return;
		}
		const bool rescanned = rescanPending;
		if (rescanPending) {
			// RefreshCategoryNodes treats every category node as newly appeared and walks it, which is
			// exactly the full rescan wanted here.
			rescanPending = false;
			for (auto& [geometry, entry] : tracked)
				ReleaseObjectSlot(entry);
			tracked.clear();
			categoryNodes.clear();
			validationCursor = 0;
			fullEvaluation = true;
			fadeDependents.clear();
			propertyDependents.clear();
			rootDependents.clear();
			dirtyRoots.clear();
			rootMotion.clear();
			DropSunCandidates();
			buckets = {};
		}

		// Drained before the category refresh, so a detach this frame can force it: a detach can take a
		// category node with it, and the signature cannot see that until the cell itself goes.
		SceneTracker::Event* events = tracker.Drain();
		bool sawDetach = false;
		for (const auto* event = events; event && !sawDetach; event = event->next)
			sawDetach = event->type == SceneTracker::EventType::Detached;
		addSource = rescanned ? TrackSource::Rescan : TrackSource::AttachEvent;
		RefreshCategoryNodes(sawDetach || rescanned);
		addSource = TrackSource::AttachEvent;

		for (auto* event = events; event; event = event->next) {
			if (event->type == SceneTracker::EventType::Attached) {
				++stats.attachedEvents;
				if (!categoryNodes.empty())
					AddSubtree(event->node.get());
			} else {
				++stats.detachedEvents;
				for (auto* geometry : event->removed)
					EraseTracked(geometry);
			}
		}
		SceneTracker::FreeEvents(events);

		ValidateSlice();
		// The fade nodes whose currentFade changed since the last drain (the delta walk re-evaluates their dependents).
		DrainFadeEvents(fadeChanged);
		if (fadeChanged.size() > kMaxFadeChanges) {
			fadeChanged.clear();
			fullEvaluation = true;
		}
		// The structural events (SceneEvents): properties whose flags, material or controllers changed, and nodes Havok
		// moved or gave a controller.
		DrainPropertyEvents(propertyChanged);
		DrainNodeEvents(nodeChanged);
		// The switch events, oldest first, one pending entry per switch: its index before the oldest event decides
		// whether the selection changed (ApplySwitchEvents).
		switchEvents.Drain([&](SwitchEvent&& a_event) {
			++delta.switchEvents;
			const auto [at, inserted] = switchPendingIndex.try_emplace(a_event.node.get(), static_cast<std::uint32_t>(switchPending.size()));
			if (inserted)
				switchPending.push_back({ std::move(a_event.node), a_event.before, a_event.structural });
			else
				switchPending[at->second].structural |= a_event.structural;
		});
		if (propertyChanged.size() > kMaxStructuralEvents || nodeChanged.size() > kMaxStructuralEvents) {
			propertyChanged.clear();
			nodeChanged.clear();
			fullEvaluation = true;
		}

		stats.tracked = static_cast<std::uint32_t>(tracked.size());
		stats.categoryNodes = static_cast<std::uint32_t>(categoryNodes.size());
	}

	void SceneStore::FindLightingShader()
	{
		// Any lighting render pass carries the BSLightingShader instance.
		for (auto& [geometry, entry] : tracked) {
			auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!property)
				continue;
			for (auto* pass = property->renderPassList.head; pass; pass = pass->next) {
				if (pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
					ConstantEvaluator::Get().SetLightingShader(pass->shader);
					return;
				}
			}
		}
	}

	bool SceneStore::CacheableVerdict(Ineligible a_reason)
	{
		// Only the verdicts that follow from the geometry's own shape - its type, its skin instance, its
		// renderer data and the *type* of its shader property. Those are exactly what the pointer
		// witnesses cover.
		//
		// Everything decided later is deliberately left out, because its inputs can change without any
		// witness noticing. AlphaBlend reads `materialAlpha`, which is animated and lives behind an
		// unchanged material pointer; the reasons DeriveLightingDescriptors produces (Decal, Technique,
		// ProjectedUV, Lod, Fading) read property *flags*, which Community Shaders' own features set in
		// place behind an unchanged property pointer. Caching those would trade a real correctness
		// surface for the cheapest third of the population.
		switch (a_reason) {
		case Ineligible::NotTriShape:
		case Ineligible::Skinned:
		case Ineligible::NoRendererData:
		case Ineligible::NotLightingShader:
			return true;
		default:
			return false;
		}
	}

	Ineligible SceneStore::ClassifyStatic(RE::BSGeometry& a_geometry, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated,
		bool a_wantDerived, RE::BSLightingShaderProperty** a_castCache)
	{
		// An NPC face shape (a dynamic shape under a BSFaceGenNiNode) takes the checks below like any shape. Its
		// positions are not in its buffers but in FaceSnapshots: the walk gives every record of one its stream
		// (SceneStore::Tracked::faceShape), whatever the verdict, and the draws bind it as the second stream.
		const auto type = a_geometry.GetType().get();
		const bool face = type == RE::BSGeometry::Type::kDynamicTriShape && FaceSnapshots::Enabled() && a_geometry.parent &&
		                  netimmerse_cast<RE::BSFaceGenNiNode*>(a_geometry.parent);
		if (type != RE::BSGeometry::Type::kTriShape) {
			// [TEMP] CS_DCLF_VOLUMETRIC_PROBE: the vertex descriptions of a face shape and its skin partitions.
			static std::atomic<std::uint32_t> faceShapesLogged{ 0 };
			if (VolumetricProbe::Enabled() && type == RE::BSGeometry::Type::kDynamicTriShape && a_geometry.parent &&
				netimmerse_cast<RE::BSFaceGenNiNode*>(a_geometry.parent) && faceShapesLogged.fetch_add(1) < 24) {
				auto& data = a_geometry.GetGeometryRuntimeData();
				auto& dynamic = static_cast<RE::BSDynamicTriShape&>(a_geometry).GetDynamicTrishapeRuntimeData();
				const auto* skin = data.skinInstance.get();
				const auto* partition = skin ? skin->skinPartition.get() : nullptr;
				std::string parts;
				for (std::uint32_t i = 0; partition && i < partition->numPartitions; ++i) {
					const auto& p = partition->partitions[i];
					parts += fmt::format(" [{}: desc {:016X} buff {:016X} vb {} verts {} bones {}]", i, std::bit_cast<std::uint64_t>(p.vertexDesc),
						p.buffData ? std::bit_cast<std::uint64_t>(p.buffData->vertexDesc) : 0ull, p.buffData ? static_cast<const void*>(p.buffData->vertexBuffer) : nullptr, p.vertices, p.numBones);
				}
				logger::info("[DCLF][TEMP] face shape '{}': desc {:016X} renderer {:016X} verts {} data {} size {} skin {} partitions {}{}",
					a_geometry.name.c_str() ? a_geometry.name.c_str() : "?", std::bit_cast<std::uint64_t>(data.vertexDesc),
					data.rendererData ? std::bit_cast<std::uint64_t>(data.rendererData->vertexDesc) : 0ull,
					static_cast<RE::BSTriShape&>(a_geometry).GetTrishapeRuntimeData().vertexCount, dynamic.dynamicData, dynamic.dataSize,
					skin ? skin->GetRTTI()->GetName() : "none", partition ? partition->numPartitions : 0u, parts);
			}
			if (!face)
				return Ineligible::NotTriShape;
		}

		auto& data = a_geometry.GetGeometryRuntimeData();
		if (auto* skin = data.skinInstance.get()) {
			if (!SkinnedEnabled())
				return Ineligible::Skinned;
			// The skinned path (engine notes: skinning): a NiSkinInstance, or with CS_DCLF_SKIN_PARTITIONS a
			// BSDismemberSkinInstance or several partitions, and a palette the native shader could index (240
			// rows). Each partition the engine draws is one draw of that PARTITION's buffer, not the geometry's
			// rendererData, which for a skinned shape is a different TriShape; which partitions it draws is
			// SkinPartitionMask's, per frame.
			static const REL::Relocation<const RE::NiRTTI*> niSkinInstance{ RE::NiSkinInstance::Ni_RTTI };
			static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
			const bool dismember = skin->GetRTTI() == dismemberSkinInstance.get();
			if (skin->GetRTTI() != niSkinInstance.get() && !dismember)
				return Ineligible::SkinShape;
			auto* partition = skin->skinPartition.get();
			auto* skinData = skin->skinData.get();
			if (!partition || !skinData || partition->numPartitions == 0 || skinData->GetBoneCount() == 0 || skinData->GetBoneCount() * 3 > 240)
				return Ineligible::SkinShape;
			if ((partition->numPartitions > 1 || dismember) && !SkinPartitionsEnabled())
				return Ineligible::SkinShape;
			if (partition->numPartitions > kMaxSkinPartitions)
				return Ineligible::SkinShape;
			// The engine indexes the dismember flags by partition; a flag array of another length is not one it
			// could have drawn from either.
			if (dismember) {
				const auto& flags = static_cast<const RE::BSDismemberSkinInstance*>(skin)->GetRuntimeData();
				if (flags.partitions && static_cast<std::uint32_t>(flags.numPartitions) != partition->numPartitions)
					return Ineligible::SkinShape;
			}
			// Every partition is drawn with the first one's pipeline, so they share its vertex layout; a LOD
			// byte past 2 would index the next row of the engine's table.
			const auto& first = partition->partitions[0];
			for (std::uint32_t i = 0; i < partition->numPartitions; ++i) {
				const auto& p = partition->partitions[i];
				if (!p.buffData || !p.buffData->vertexBuffer || !p.buffData->indexBuffer)
					return Ineligible::NoRendererData;
				if (!first.buffData || std::bit_cast<std::uint64_t>(p.buffData->vertexDesc) != std::bit_cast<std::uint64_t>(first.buffData->vertexDesc) ||
					(p.pad42 & 0xFF) > 2)
					return Ineligible::SkinShape;
			}
		} else if (!data.rendererData || !data.rendererData->vertexBuffer || !data.rendererData->indexBuffer) {
			return Ineligible::NoRendererData;
		}

		// The caller may already know what this property is (see Tracked::castProperty); the cast is a walk
		// of dependent RTTI pointer loads, which is the part of this function that neither a memo nor the
		// reads BuildFrame makes later can remove.
		auto* property = a_castCache ? *a_castCache : netimmerse_cast<RE::BSLightingShaderProperty*>(data.shaderProperty.get());
		if (!property)
			return Ineligible::NotLightingShader;

		// GetRenderPasses treats material alpha below one as transparent.
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
		if (material && material->materialAlpha < 1.0f)
			return Ineligible::AlphaBlend;

		LightingDescriptors descriptors;
		const Ineligible reason = DeriveLightingDescriptors(*property, a_geometry, a_accumulated, descriptors, a_wantDerived);
		if (a_descriptors) {
			if (reason == Ineligible::None) {
				*a_descriptors = descriptors;
			} else {
				// The rejection diagnostics have to survive the rejection. Copying the descriptors only on
				// success left rejectedTechnique at its default 0, and 0 is `none` - a SUPPORTED technique -
				// so the histogram read "none(0)=1337" and looked like a finding rather than an empty field.
				a_descriptors->rejectedTechnique = descriptors.rejectedTechnique;
			}
		}
		return reason;
	}

	bool SceneStore::ReadSwitch(const RE::NiSwitchNode& a_switch, SwitchState& a_out)
	{
		if (REL::Module::IsVR())
			return false;
		const auto* base = reinterpret_cast<const std::byte*>(&a_switch);
		a_out.flags = *reinterpret_cast<const std::uint16_t*>(base + 0x128);
		a_out.index = *reinterpret_cast<const std::int32_t*>(base + 0x12C);
		a_out.revID = *reinterpret_cast<const std::uint32_t*>(base + 0x134);
		// childRevID, an NiTPrimitiveArray at +0x138: its data pointer at +0x8, its capacity at +0x10.
		a_out.childRevID = *reinterpret_cast<const std::uint32_t* const*>(base + 0x140);
		a_out.childRevCapacity = *reinterpret_cast<const std::uint16_t*>(base + 0x148);
		return true;
	}

	bool SceneStore::SwitchSelects(const RE::NiSwitchNode& a_switch, const RE::NiAVObject* a_child)
	{
		// NiSwitchNode::OnVisible (AE 140d29700) culls children[index] and nothing else. A child that has
		// become the selected one since the last update pass is brought up to date there, in the cull
		// (childRevID[index] != revID), which is after this walk read its transforms: leave it native for
		// that frame.
		// Bounded by capacity, not size: a Gamebryo array is indexed by slot, and size counts the used ones.
		SwitchState state;
		if (!ReadSwitch(a_switch, state))
			return false;
		const auto& children = a_switch.GetChildren();
		if (state.index < 0 || static_cast<std::uint32_t>(state.index) >= children.capacity() || !state.childRevID ||
			static_cast<std::uint32_t>(state.index) >= state.childRevCapacity)
			return false;
		const auto index = static_cast<std::uint16_t>(state.index);
		return children[index].get() == a_child && state.childRevID[index] == state.revID;
	}

	void SceneStore::CaptureCullHiddenBits()
	{
		cullHiddenBits.clear();
		for (auto* sceneNode : RE::BSShaderManager::State::GetSingleton().shadowSceneNode) {
			const auto* graph = sceneNode ? sceneNode->GetRuntimeData().portalGraph : nullptr;
			if (!graph)
				continue;
			for (const auto& child : graph->alwaysRenderChildren)
				if (child)
					cullHiddenBits.emplace_back(child.get(), IsHidden(child.get()));
			if (graph->portalSharedNode)
				cullHiddenBits.emplace_back(graph->portalSharedNode.get(), IsHidden(graph->portalSharedNode.get()));
		}
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			// The third-person skeleton as it is now. TESWaterReflections::Update (AE 0x140520570), on the frames a
			// cube-map reflection updates, hides the player's 3D while it renders the faces and then restores it.
			// Main::Draw calls it between the main cull jobs' Begin and Finish, so it runs alongside the walk.
			const RE::NiAVObject* thirdPerson = player->Get3D(false);
			if (thirdPerson)
				cullHiddenBits.emplace_back(thirdPerson, IsHidden(thirdPerson));
			// The first-person skeleton: Main::Draw (AE 0x1406444b0) hides it right after the call the walk is
			// kicked from, keeps it hidden through the main camera's cull and the sun's shadow casters, and shows it
			// only to draw the first-person view with its own camera. For every view the walk serves it is hidden.
			const RE::NiAVObject* firstPerson = player->Get3D(true);
			if (firstPerson && firstPerson != thirdPerson)
				cullHiddenBits.emplace_back(firstPerson, true);
		}
		std::sort(cullHiddenBits.begin(), cullHiddenBits.end());
	}

	bool SceneStore::HiddenForWalk(const RE::NiAVObject* a_object) const
	{
		const auto it = std::lower_bound(cullHiddenBits.begin(), cullHiddenBits.end(), a_object, [](const auto& a_entry, const RE::NiAVObject* a_key) { return a_entry.first < a_key; });
		if (it != cullHiddenBits.end() && it->first == a_object)
			return it->second;
		return IsHidden(a_object);
	}

	Ineligible SceneStore::ClassifyFrame(const Tracked& a_tracked, const AccumulatedPass* a_accumulated) const
	{
		const bool underSwitch = a_tracked.parentReason == Ineligible::Switch;
		if (underSwitch && !SwitchNodesEnabled())
			return Ineligible::Switch;
		if (a_tracked.parentReason != Ineligible::None && !underSwitch)
			return a_tracked.parentReason;

		// App-culled or hidden anywhere between the leaf and its category node; part of an actor; under a
		// switch node that does not draw this branch.
		const bool actors = ActorsEnabled();
		const RE::NiAVObject* child = nullptr;
		for (const RE::NiAVObject* object = a_tracked.geometry.get(); object; child = object, object = object->parent) {
			if (HiddenForWalk(object))
				return Ineligible::Hidden;
			if (object == a_tracked.categoryNode)
				break;
			if (underSwitch && child) {
				if (auto* switchNode = const_cast<RE::NiAVObject*>(object)->AsSwitchNode(); switchNode && !SwitchSelects(*switchNode, child))
					return Ineligible::Switch;
			}
			if (!actors)
				if (auto* ref = object->GetUserData(); ref && ref->IsActor())
					return Ineligible::Actor;
		}

		// Fading: as the pass was registered when there is one (the withholding decided on that same value),
		// else as the fade node stands now.
		auto* property = a_tracked.geometry->GetGeometryRuntimeData().shaderProperty.get();
		// Without the pass (the scene phase, for the shadow views) a fade is only the native loop's when fades
		// are not DCLF's at all; the shadow views skip faded casters themselves (ShadowReject::Faded).
		const bool fading = a_accumulated ? a_accumulated->fading :
		                                    !FadingEnabled() && property && property->fadeNode && property->fadeNode->GetRuntimeData().currentFade < 1.0f;
		if (fading)
			return Ineligible::Fading;

		return Ineligible::None;
	}

	const std::vector<std::uint32_t>& SceneStore::GetMaterialPatchedFloats() const
	{
		return MaterialSources::FramePSFloats();
	}

	const std::vector<std::uint32_t>& SceneStore::GetMaterialPatchedVSFloats() const
	{
		return MaterialSources::FrameVSFloats();
	}

	namespace
	{
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
		bool CopyFrameGeometry(const GeometryConstants& a_sample, GeometryConstants& a_out)
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
		bool CopyEyePosition(const GeometryConstants& a_sample, GeometryConstants& a_out)
		{
			const auto& layout = LightingVSLayout();
			const std::uint32_t offset = layout.offset[kVSEyePosition];
			const std::size_t bytes = layout.size[kVSEyePosition] * sizeof(float);
			if (!a_out.vs.Written(offset) || !a_sample.vs.Written(offset) || std::memcmp(&a_out.vs.floats[offset], &a_sample.vs.floats[offset], bytes) == 0)
				return false;
			std::memcpy(&a_out.vs.floats[offset], &a_sample.vs.floats[offset], bytes);
			return true;
		}
	}

	void SceneStore::Tables::ListMaterialSlot(std::uint32_t a_slot, std::uint32_t a_frame)
	{
		const std::uint32_t signature = MaterialSources::Signature(materialSlotKey[a_slot].second);
		if (a_slot >= materialSignatureListed.size()) {
			materialSignatureListed.resize(materialSlotKey.size(), 0);
			transformWatchFrame.resize(materialSlotKey.size(), 0);
		}
		if (materialSignatureListed[a_slot] != signature + 1) {
			materialSignatureListed[a_slot] = signature + 1;
			frameSignatures[signature].slots.push_back(a_slot);
		}
		materialFramePending.push_back(a_slot);
		if (!transformWatchFrame[a_slot])
			transformWatch.push_back(a_slot);
		transformWatchFrame[a_slot] = a_frame;
	}

	void SceneStore::RefreshFrameMaterials()
	{
		stats.frameMaterialSamples = 0;
		auto& evaluator = ConstantEvaluator::Get();
		if (tables.materials.empty() || !evaluator.HasLightingShader())
			return;
		auto& m = materialFrameStats;
		++m.frames;
		auto apply = [&](std::uint32_t a_slot, const MaterialRecord& a_live) {
			bool floatsChanged = false;
			if (MaterialSources::ApplyFrameComponents(a_live, tables.materials[a_slot], tables.materialSlotKey[a_slot].second, &floatsChanged))
				tables.materialVersion[a_slot] = ++materialVersions;
			if (floatsChanged)
				tables.materialFrameVersion[a_slot] = tables.NextVersion();
		};
		auto keyed = [&](std::uint32_t a_slot, std::uint32_t a_signature) {
			return a_slot < tables.materials.size() && tables.materialSlots.Alive(a_slot) && tables.materialSlotKey[a_slot].first &&
			       MaterialSources::Signature(tables.materialSlotKey[a_slot].second) == a_signature;
		};
		std::optional<ScopedScan> scanLive(std::in_place, Scan::MaterialLive);
		for (auto& [signature, entry] : tables.frameSignatures) {
			// The live sample, from a slot of the signature drawn this frame (the last one's while it still is). With none
			// drawn nothing of the signature is: the slots take the sample when one is.
			auto& slots = entry.slots;
			if (!(keyed(entry.representative, signature) && tables.materialLastUsed[entry.representative] == frame)) {
				entry.representative = ~0u;
				for (const std::uint32_t slot : slots)
					if (keyed(slot, signature) && tables.materialLastUsed[slot] == frame) {
						entry.representative = slot;
						break;
					}
			}
			if (entry.representative == ~0u)
				continue;
			const auto key = tables.materialSlotKey[entry.representative];
			MaterialRecord live;
			if (!evaluator.EvaluateMaterial(key.first, key.second, live))
				continue;
			++stats.frameMaterialSamples;
			++m.samples;
			if (entry.appliedValid) {
				MaterialRecord probe = entry.applied;
				bool floatsChanged = false;
				if (!MaterialSources::ApplyFrameComponents(live, probe, key.second, &floatsChanged) && !floatsChanged)
					continue;
			}
			entry.applied = live;
			entry.appliedValid = true;
			++m.applications;
			for (std::size_t i = 0; i < slots.size();) {
				const std::uint32_t slot = slots[i];
				if (!keyed(slot, signature)) {
					if (slot < tables.materialSignatureListed.size() && tables.materialSignatureListed[slot] == signature + 1)
						tables.materialSignatureListed[slot] = 0;
					slots[i] = slots.back();
					slots.pop_back();
					continue;
				}
				apply(slot, live);
				++m.slotsApplied;
				++i;
			}
		}
		scanLive.reset();
		ScopedScan scanApply(Scan::MaterialApply);
		// The slots keyed or rewritten since the last application: the signature's sample regardless.
		m.pending += tables.materialFramePending.size();
		for (const std::uint32_t slot : tables.materialFramePending) {
			if (slot >= tables.materials.size() || !tables.materialSlots.Alive(slot) || !tables.materialSlotKey[slot].first)
				continue;
			const auto it = tables.frameSignatures.find(MaterialSources::Signature(tables.materialSlotKey[slot].second));
			if (it != tables.frameSignatures.end() && it->second.appliedValid)
				apply(slot, it->second.applied);
		}
		tables.materialFramePending.clear();
	}

	void SceneStore::RefreshTextureTransforms()
	{
		ScopedScan scan(Scan::TextureTransforms);
		auto& list = tables.transformWatch;
		materialFrameStats.transformsWatched += list.size();
		for (std::size_t i = 0; i < list.size();) {
			const std::uint32_t slot = list[i];
			bool keep = slot < tables.materials.size() && tables.materialSlots.Alive(slot) && tables.materialSlotKey[slot].first;
			// The material is read only while a slot drawn this frame holds it.
			if (keep && tables.materialLastUsed[slot] == frame) {
				const auto* material = tables.materialSlotKey[slot].first;
				if (MaterialSources::ApplyTextureTransform(material, tables.materials[slot]))
					tables.materialFrameVersion[slot] = tables.NextVersion();
				const auto* base = static_cast<const RE::BSLightingShaderMaterialBase*>(material);
				keep = frame - tables.transformWatchFrame[slot] <= 2 || base->texCoordOffset[0] != base->texCoordOffset[1] ||
				       base->texCoordScale[0] != base->texCoordScale[1];
			}
			if (!keep) {
				tables.transformWatchFrame[slot] = 0;
				list[i] = list.back();
				list.pop_back();
				continue;
			}
			++i;
		}
	}

	void SceneStore::CheckMaterialFrame()
	{
		// CS_DCLF_PERSISTENT_PARITY: every slot drawn this frame against its signature's sample and its material's transform,
		// as the per-frame loops applied them.
		static const bool enabled = SwitchEnabled("CS_DCLF_PERSISTENT_PARITY");
		if (!enabled || frame % 60 != 45)
			return;
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		auto& m = materialFrameStats;
		++m.checks;
		ankerl::unordered_dense::map<std::uint32_t, MaterialRecord> live;
		for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot) {
			if (tables.materialLastUsed[slot] != frame || !tables.materialSlotKey[slot].first)
				continue;
			const auto key = tables.materialSlotKey[slot];
			const std::uint32_t signature = MaterialSources::Signature(key.second);
			auto it = live.find(signature);
			if (it == live.end()) {
				MaterialRecord record;
				if (!evaluator.EvaluateMaterial(key.first, key.second, record))
					continue;
				it = live.emplace(signature, record).first;
			}
			++m.slotsChecked;
			MaterialRecord probe = tables.materials[slot];
			bool floatsChanged = false;
			if (MaterialSources::ApplyFrameComponents(it->second, probe, key.second, &floatsChanged) || floatsChanged) {
				if (m.componentsDiffer++ == 0 && m.first.empty())
					m.first = fmt::format("slot {} (pass {:X}): frame components", slot, key.second);
			}
			if (MaterialSources::ApplyTextureTransform(key.first, probe)) {
				if (m.transformsDiffer++ == 0 && m.first.empty())
					m.first = fmt::format("slot {} (pass {:X}): texture transform", slot, key.second);
			}
		}
	}

	void SceneStore::ProcessMaterialWrites()
	{
		ScopedScan scan(Scan::MaterialWrites);
		writtenMaterials.clear();
		const bool complete = MaterialSources::Drain(writtenMaterials);
		stats.materialWrites = static_cast<std::uint32_t>(writtenMaterials.size());
		stats.materialsRewritten = stats.materialsDropped = 0;
		if (complete && writtenMaterials.empty())
			return;
		if (!complete)
			logger::warn("[DCLF] material write queue overflowed: every material record is re-evaluated");
		auto& evaluator = ConstantEvaluator::Get();
		const bool canEvaluate = evaluator.HasLightingShader();
		auto written = [&](const RE::BSShaderMaterial* a_material) { return !complete || writtenMaterials.contains(a_material); };
		for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot) {
			if (!tables.materialSlots.Alive(slot))
				continue;
			const auto key = tables.materialSlotKey[slot];
			if (!key.first || !written(key.first))
				continue;
			MaterialRecord live;
			if (tables.materialLastUsed[slot] == frame && canEvaluate && evaluator.EvaluateMaterial(key.first, key.second, live)) {
				if (!(live == tables.materials[slot])) {
					tables.materials[slot] = live;
					tables.materialVersion[slot] = ++materialVersions;
					++stats.materialsRewritten;
				}
				tables.ListMaterialSlot(slot, frame);
				if (auto it = materialCache.find(key); it != materialCache.end())
					it->second.record = live;
				continue;
			}
			// Not drawn this frame (or not evaluable): dropped, so that its next use evaluates it afresh. An
			// object's cached derivation checks its slot is still allocated to the same key.
			materialIndex.erase(key);
			materialCache.erase(key);
			tables.materialSlotKey[slot] = { nullptr, 0u };
			tables.materialLastUsed[slot] = Tables::kSlotFree;
			tables.materialSlots.Free(slot);
			++stats.materialsDropped;
			slotsFreedThisFrame = true;
		}
		// Cache entries without a slot.
		std::erase_if(materialCache, [&](const auto& a_entry) {
			return written(a_entry.first.first) && !materialIndex.contains(a_entry.first);
		});
	}

	void SceneStore::RefreshFrameConstants()
	{
		{
			ScopedScan scan(Scan::FrameMaterials);
			RefreshFrameMaterials();
		}
		CheckMaterialFrame();
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		std::optional<ScopedScan> scanPipelines(std::in_place, Scan::PipelineConstants);
		struct
		{
			GeometryConstants constants;
			bool valid = false;
		} frameSample, eyeSample;
		// The frame lighting, merged from the frame's evaluations (MergeFrameLighting) over what was last published (a
		// component none of them writes keeps its value) and published after them; versioned only when it differs. On a
		// parity frame each pipeline's reference is kept to compare with what was published.
		FrameLighting frameLighting;
		std::memcpy(frameLighting.data(), tables.frameLighting.data(), sizeof(frameLighting));
		std::uint32_t lightingWritten = 0;
		auto publishLighting = [&](const GeometryConstants& a_constants) { MergeFrameLighting(a_constants.ps, frameLighting, lightingWritten); };
		std::vector<std::pair<std::uint32_t, GeometryConstants>> lightingReferences;
		static const bool geometryParityEnabled = SwitchEnabled("CS_DCLF_PERSISTENT_PARITY");
		const bool geometryParityFrame = geometryParityEnabled && frame % 60 == 30;
		geometryStats.checks += geometryParityFrame ? 1u : 0u;
		for (std::size_t i = 0; i < tables.pipelines.size() && i < tables.geometryTemplate.size(); ++i) {
			if (!tables.PipelineUsed(i, frame))
				continue;
			// The technique constants are the frame's (fog, settings, and the shadow mask's view), so a
			// pipeline slot that outlives the frame takes them fresh here, as it did when the pipeline
			// table was rebuilt every frame. Serving the slot's first evaluation instead was both a parity
			// regression and, at startup, a stale view pointer handed to the render graph. They are its technique
			// row's (Tables::TechniqueRow): evaluated once a frame for all the pipelines of its key, and written, and
			// versioned, only where the values differ.
			auto sameFloats = [](const ConstantBlock& a, const ConstantBlock& b) { return std::memcmp(a.floats.data(), b.floats.data(), sizeof(a.floats)) == 0; };
			std::optional<ScopedScan> scanTechnique(std::in_place, Scan::Technique);
			auto& row = tables.techniques[tables.pipelineTechnique[i]];
			if (row.evaluated != frame) {
				row.evaluated = frame;
				TechniqueConstants now;
				EvaluateTechnique(tables.pipelines[i].passDescriptor, now);
				const bool floats = !sameFloats(now.vs, row.value.vs) || !sameFloats(now.ps, row.value.ps);
				const bool binding = now.filterModes != row.value.filterModes || now.shadowMask != row.value.shadowMask ||
				                     now.shadowMaskTexture != row.value.shadowMaskTexture;
				if (floats || binding)
					row.value = now;
				if (floats)
					row.constantsVersion = tables.NextVersion();
				if (binding)
					row.bindingVersion = tables.NextVersion();
				// CS_DCLF_PERSISTENT_PARITY: the row against a second evaluation, once a frame.
				if (geometryParityFrame) {
					TechniqueConstants reference;
					EvaluateTechnique(tables.pipelines[i].passDescriptor, reference);
					++geometryStats.techniquesChecked;
					if (!sameFloats(reference.vs, row.value.vs) || !sameFloats(reference.ps, row.value.ps) || reference.filterModes != row.value.filterModes ||
						reference.shadowMask != row.value.shadowMask || reference.shadowMaskTexture != row.value.shadowMaskTexture)
						++geometryStats.techniquesDiffer;
				}
			}
			scanTechnique.reset();
			ScopedScan scanGeometry(Scan::Geometry);
			// A pipeline's PerGeometry block is evaluated in full once (and again when the render flags change); what of it
			// changes afterwards is either overridden per object (ObjectGeometryConstants) or one of the frame's globals
			// (kPSFrameGeometry: the sun's direction and colour, the ambient terms), which every pipeline that writes them
			// shares and which are copied from the frame's one sample. SetupGeometry writes EyePosition only for Envmap, Eye
			// and technique 0x10, and the same for all three (the camera less posAdjust in world space, 0x1414dd040): their
			// pipelines take it from the frame's one evaluation of such a pipeline (eyeSample). The frame's globals are kept
			// in the block for the constant-buffer path and the parity checks, but no DCLF_BINDLESS draw reads them there
			// (frameLighting; kPSBindlessGeometryUnread), and neither the template object's own values, so only what such a
			// draw reads versions the pipeline (SameBindlessGeometry). The template's pass is looked up only to evaluate.
			auto* property = tables.geometryTemplate[i];
			auto templatePassOf = [&]() { return property ? FindLightingPass(property) : nullptr; };
			const std::uint32_t geometryTechnique = (tables.pipelines[i].passDescriptor >> 24) & 0x3f;
			const bool writesEye = geometryTechnique == 1 || geometryTechnique == 0xb || geometryTechnique == 0x10;
			const bool full = !tables.geometryConstantsValid[i] || geometryEvaluatedFlags != mainPassRenderFlags || (writesEye && !eyeSample.valid);
			auto& held = tables.geometryConstants[i];
			bool ownChanged = false;
			if (!full && writesEye) {
				CopyFrameGeometry(eyeSample.constants, held);
				CopyEyePosition(eyeSample.constants, held);
			} else if (full) {
				const auto* templatePass = templatePassOf();
				if (!templatePass)
					continue;  // keep what BuildFrame evaluated rather than blanking it
				GeometryConstants constants;
				if (!evaluator.EvaluateGeometry(*templatePass, tables.pipelines[i].passDescriptor, mainPassRenderFlags, constants))
					continue;
				++geometryStats.full;
				publishLighting(constants);
				// Its own values: what a bindless draw reads from the block (PackGeometryTemplate's mask).
				ownChanged = !tables.geometryConstantsValid[i] || !SameBindlessGeometry(constants, held);
				held = constants;
				if (writesEye && !eyeSample.valid) {
					eyeSample.constants = constants;
					eyeSample.valid = true;
				}
			} else {
				if (!frameSample.valid) {
					if (const auto* templatePass = templatePassOf()) {
						frameSample.valid = evaluator.EvaluateGeometry(*templatePass, tables.pipelines[i].passDescriptor, mainPassRenderFlags, frameSample.constants);
						++geometryStats.samples;
						if (frameSample.valid)
							publishLighting(frameSample.constants);
					}
				}
				if (!frameSample.valid)
					continue;
				CopyFrameGeometry(frameSample.constants, held);
			}
			if (ownChanged) {
				tables.pipelineConstantsVersion[i] = tables.NextVersion();
				++geometryStats.changed;
			}
			tables.geometryConstantsValid[i] = 1;
			// CS_DCLF_PERSISTENT_PARITY: the block against a full evaluation, in what no object overrides; the frame
			// lighting is checked against each reference after the loop, where the reference writes it.
			if (geometryParityFrame) {
				GeometryConstants reference;
				const auto* templatePass = templatePassOf();
				if (templatePass && evaluator.EvaluateGeometry(*templatePass, tables.pipelines[i].passDescriptor, mainPassRenderFlags, reference)) {
					CheckFrameGeometry(static_cast<std::uint32_t>(i), reference, held);
					lightingReferences.emplace_back(static_cast<std::uint32_t>(i), reference);
				}
			}
		}
		if (lightingWritten && std::memcmp(frameLighting.data(), tables.frameLighting.data(), sizeof(frameLighting)) != 0) {
			std::memcpy(tables.frameLighting.data(), frameLighting.data(), sizeof(frameLighting));
			tables.frameLightingVersion = tables.NextVersion();
			++geometryStats.lightingVersions;
		}
		for (const auto& [pipeline, reference] : lightingReferences) {
			++geometryStats.lightingChecked;
			std::string first;
			if (MatchesFrameLighting(reference.ps, frameLighting, geometryStats.lightingFirst.empty() ? &first : nullptr))
				continue;
			++geometryStats.lightingDiffer;
			if (!first.empty())
				geometryStats.lightingFirst = fmt::format("pipeline {} (pass {:X}) {}", pipeline, tables.pipelines[pipeline].passDescriptor, first);
		}
		geometryEvaluatedFlags = mainPassRenderFlags;
		++geometryStats.frames;

		// Per-object shading is resampled here too, for the slots whose inputs change with no event (Tables::watchList).
		// The property's alpha, emissive colour and multiplier are animated by controllers: candle and chandelier
		// emissives flicker, and sampling them at EarlyPrepass instead of here put them far enough from the draw that
		// capture parity's 0.1% tolerance on EmitColor stopped covering the difference. The LOD fades GetRenderPasses
		// leaves on the property change only when the engine registers the object, which is a patch, and the patch
		// samples them. An actor's alpha fades with the actor. ProjectedUV's and land blend's extras rows follow the
		// eye and a clock.
		scanPipelines.reset();
		std::optional<ScopedScan> scanShading(std::in_place, Scan::ShadingResample);
		auto refreshExtras = [&](std::uint32_t o) {
			if (tables.objects[o].flags & (kObjectProjectedUV | kObjectLandBlend)) {
				const auto* geometry = tables.objectGeometry[o];
				auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				if (property && !(tables.objects[o].flags & kObjectNoBindings))
					RefreshObjectExtras(o, *static_cast<RE::BSLightingShaderProperty*>(property), *geometry);
			}
		};
		std::uint64_t resampled = 0;
		if (resampledRenderFlags != mainPassRenderFlags || !lodFadeEventsInstalled) {
			resampledRenderFlags = mainPassRenderFlags;
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				ResampleShading(o, true);
				refreshExtras(o);
			}
			resampled += tables.objects.size();
		} else {
			auto& list = tables.watchList;
			for (std::size_t i = 0; i < list.size();) {
				const std::uint32_t o = list[i];
				auto& bits = tables.shadingWatch[o];
				if (o >= tables.objects.size() || (tables.objects[o].flags & kObjectFree) || !(bits & ~Tables::kWatchListed)) {
					bits = 0;
					list[i] = list.back();
					list.pop_back();
					continue;
				}
				if (bits & Tables::kWatchShading)
					ResampleShading(o, true);
				if (bits & Tables::kWatchExtras)
					refreshExtras(o);
				++resampled;
				++i;
			}
			for (const std::uint32_t o : tables.actorObjects) {
				ResampleShading(o, true);
				++resampled;
			}
			lodFadeChanged.clear();
			DrainLodFadeEvents(lodFadeChanged);
			std::sort(lodFadeChanged.begin(), lodFadeChanged.end());
			lodFadeChanged.erase(std::unique(lodFadeChanged.begin(), lodFadeChanged.end()), lodFadeChanged.end());
			shadingParity.lodFadeEvents += lodFadeChanged.size();
			for (const void* key : lodFadeChanged) {
				const auto dependents = propertyDependents.find(key);
				if (dependents == propertyDependents.end())
					continue;
				for (auto* geometry : dependents->second) {
					const auto it = tracked.find(geometry);
					if (it == tracked.end() || it->second.slot == kNoObjectSlot || it->second.objectStamp != objectStamp)
						continue;
					ResampleShading(it->second.slot, true);
					++resampled;
				}
			}
		}
		// The watch's completeness: every slot sampled against what the tables hold now.
		static const bool shadingParityEnabled = SwitchEnabled("CS_DCLF_PERSISTENT_PARITY");
		auto& sp = shadingParity;
		++sp.frames;
		sp.watched += tables.watchList.size();
		sp.resampled += resampled;
		if (shadingParityEnabled && frame % 60 == 0) {
			++sp.checks;
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				if ((tables.objects[o].flags & (kObjectFree | kObjectNoBindings)) || !tables.objectGeometry[o])
					continue;
				++sp.slots;
				if (ResampleShading(o, false) && sp.missing++ == 0) {
					const auto* geometry = tables.objectGeometry[o];
					const auto& held = tables.shading[o];
					float emissiveMult = 0.0f;
					LightingDescriptors descriptors;
					descriptors.pass = tables.pipelines[tables.objects[o].pipelineIndex].passDescriptor;
					descriptors.technique = (descriptors.pass >> 24) & 0x3f;
					const auto& lighting = *static_cast<RE::BSLightingShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
					descriptors.specularLODFade = lighting.specularLODFade;
					descriptors.envmapLODFade = lighting.envmapLODFade;
					const auto now = MakeShading(lighting, descriptors, mainPassRenderFlags, emissiveMult);
					sp.first = fmt::format("slot {} '{}' (watch {:#x}, flags {:#x}, patched {} frames ago): material data ({} {} {}) against ({} {} {}), emit ({} {} {}) against ({} {} {}), mult {} against {}",
						o, geometry->name.c_str() ? geometry->name.c_str() : "", o < tables.shadingWatch.size() ? tables.shadingWatch[o] : 0, tables.objects[o].flags,
						o < patchedFrame.size() ? frame - patchedFrame[o] : ~0u, now.materialData[0], now.materialData[1], now.materialData[2], held.materialData[0],
						held.materialData[1], held.materialData[2], now.emitColor[0], now.emitColor[1], now.emitColor[2], held.emitColor[0], held.emitColor[1],
						held.emitColor[2], emissiveMult, tables.emissiveMult[o]);
				}
			}
		}

		// Advanced Skin's wetness, per actor-owned object. Skin::GetWetness keeps each actor's fading state and
		// computes it once a frame (the first call; later ones, including its own SetupGeometry hook for the draws
		// the engine still makes this frame, return the same value), so calling it here for every actor in the
		// tables advances every actor's fade once a frame, whether or not the engine draws it.
		scanShading.reset();
		ScopedScan scanWetness(Scan::Wetness);
		auto& skin = globals::features::skin;
		const bool wetness = skin.loaded && skin.settings.EnableSkin;
		for (const std::uint32_t o : tables.actorObjects) {
			auto* geometry = o < tables.objectGeometry.size() ? tables.objectGeometry[o] : nullptr;
			if (o < tables.skinWetness.size()) {
				const float4 value = wetness && geometry ? skin.GetWetness(geometry) : float4{};
				const std::array<float, 4> row{ value.x, value.y, value.z, value.w };
				if (std::memcmp(row.data(), tables.skinWetness[o].data(), sizeof(row)) != 0) {
					tables.skinWetness[o] = row;
					tables.NoteChange(o, kChangeShading);
				}
			}
		}
	}

	void SceneStore::CheckFrameGeometry(std::uint32_t a_pipeline, const GeometryConstants& a_reference, const GeometryConstants& a_held)
	{
		auto& g = geometryStats;
		++g.pipelinesChecked;
		for (std::uint32_t stage = 0; stage < 2; ++stage) {
			const auto& layout = stage ? LightingPSLayout() : LightingVSLayout();
			const auto& a = stage ? a_reference.ps : a_reference.vs;
			const auto& b = stage ? a_held.ps : a_held.vs;
			const std::uint32_t technique = (tables.pipelines[a_pipeline].passDescriptor >> 24) & 0x3f;
			const bool writesEye = technique == 1 || technique == 0xb || technique == 0x10;
			const std::uint64_t skip = stage ? kObjectGeometryPS : (kObjectGeometryVS | (writesEye ? 0ull : 1ull << kVSEyePosition));
			const std::uint64_t perGeometry = stage ? kPSGroups[kPerGeometry] : kVSGroups[kPerGeometry];
			for (std::uint32_t v = 0; v < layout.count; ++v) {
				if ((skip & (1ull << v)) || !(perGeometry & (1ull << v)))
					continue;
				if (std::memcmp(&a.floats[layout.offset[v]], &b.floats[layout.offset[v]], layout.size[v] * sizeof(float)) == 0)
					continue;
				++g.differ[stage][v];
				if (g.first.empty())
					g.first = fmt::format("pipeline {} (pass {:X}) {}{}: {} against {}", a_pipeline, tables.pipelines[a_pipeline].passDescriptor, stage ? "PS" : "VS", v,
						a.floats[layout.offset[v]], b.floats[layout.offset[v]]);
			}
		}
	}

	bool SceneStore::ResampleShading(std::uint32_t a_slot, bool a_write)
	{
		if (a_slot >= tables.objects.size() || a_slot >= tables.objectGeometry.size())
			return false;
		const auto* geometry = tables.objectGeometry[a_slot];
		auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		if (!property || (tables.objects[a_slot].flags & (kObjectNoBindings | kObjectFree)))
			return false;
		const std::uint32_t passDescriptor = tables.pipelines[tables.objects[a_slot].pipelineIndex].passDescriptor;
		LightingDescriptors descriptors;
		descriptors.pass = passDescriptor;
		descriptors.technique = (passDescriptor >> 24) & 0x3f;
		const auto& lighting = *static_cast<RE::BSLightingShaderProperty*>(property);
		descriptors.specularLODFade = lighting.specularLODFade;
		descriptors.envmapLODFade = lighting.envmapLODFade;
		float emissiveMult = tables.emissiveMult[a_slot];
		const auto shading = MakeShading(lighting, descriptors, mainPassRenderFlags, emissiveMult);
		if (std::memcmp(&shading, &tables.shading[a_slot], sizeof(shading)) == 0 && std::bit_cast<std::uint32_t>(emissiveMult) == std::bit_cast<std::uint32_t>(tables.emissiveMult[a_slot]))
			return false;
		if (a_write) {
			tables.shading[a_slot] = shading;
			tables.emissiveMult[a_slot] = emissiveMult;
			tables.NoteChange(a_slot, kChangeShading);
		}
		return true;
	}

	namespace
	{
		float GlobalFloatAt(const REL::Relocation<std::uintptr_t>& a_at)
		{
			return *reinterpret_cast<const float*>(a_at.address());
		}
	}

	void SceneStore::RefreshObjectExtras(std::size_t a_object, const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry)
	{
		if (a_object >= tables.extraOffset.size() || tables.extraOffset[a_object] == kNoExtraRows)
			return;
		float* rows = &tables.extraRows[std::size_t(tables.extraOffset[a_object]) * 4];
		std::array<float, kExtraRows * 4> before;
		std::memcpy(before.data(), rows, sizeof(before));
		// Noted when the rows come out different (on every return below).
		struct Note
		{
			Tables& tables;
			std::uint32_t slot;
			const float* rows;
			const std::array<float, kExtraRows * 4>& before;
			~Note()
			{
				if (std::memcmp(before.data(), rows, sizeof(before)) != 0)
					tables.NoteChange(slot, kChangeExtras);
			}
		} note{ tables, static_cast<std::uint32_t>(a_object), rows, before };
		const auto& object = tables.objects[a_object];
		auto& state = globals::game::shadowState->GetRuntimeData();
		const auto eye = state.posAdjust.getEye();

		if (object.flags & kObjectLandBlend) {
			// BSLightingShader::SetupGeometry, techniques 8 and 19 (engine notes: per-object constants):
			// xy from the landscape material, zw a blend between two BSShaderManager::State positions by
			// a clock the same state holds, minus the geometry's world translation. Module-relative reads.
			static const REL::Relocation<std::uintptr_t> blendClock{ REL::Offset(0x2033080) };
			static const REL::Relocation<std::uintptr_t> blendClockStart{ REL::Offset(0x2033118) };
			static const REL::Relocation<std::uintptr_t> blendDuration{ REL::Offset(0x20330f8) };
			static const REL::Relocation<std::uintptr_t> blendRate{ REL::Offset(0x1ad2840) };
			static const REL::Relocation<std::uintptr_t> blendFromX{ REL::Offset(0x2033108) };
			static const REL::Relocation<std::uintptr_t> blendFromY{ REL::Offset(0x203310c) };
			static const REL::Relocation<std::uintptr_t> blendToX{ REL::Offset(0x2033110) };
			static const REL::Relocation<std::uintptr_t> blendToY{ REL::Offset(0x2033114) };
			float t = (GlobalFloatAt(blendClock) - GlobalFloatAt(blendClockStart)) * (GlobalFloatAt(blendRate) / GlobalFloatAt(blendDuration));
			if (t <= 0.0f)
				t = 0.0f;
			if (1.0f <= t)
				t = 1.0f;
			const float x = (GlobalFloatAt(blendToX) - GlobalFloatAt(blendFromX)) * t + GlobalFloatAt(blendFromX);
			const float y = (GlobalFloatAt(blendToY) - GlobalFloatAt(blendFromY)) * t + GlobalFloatAt(blendFromY);
			const auto* material = static_cast<const RE::BSLightingShaderMaterialLandscape*>(a_property.material);
			float* land = rows + kExtraRowLandBlend * 4;
			land[0] = material ? material->landBlendParams.red : 0.0f;
			land[1] = material ? material->landBlendParams.green : 0.0f;
			land[2] = x - a_geometry.world.translate.x;
			land[3] = y - a_geometry.world.translate.y;
		}

		if (object.flags & kObjectProjectedUV) {
			// The texture matrix, as SetupGeometry builds it for a ProjectedUV pass (engine notes): the
			// projection is a fixed rotation about Z placed at posAdjust, converted with the engine's own
			// NiTransform-to-matrix routine (which subtracts posAdjust, so its translation is zero); for
			// every technique but Envmap it is multiplied onto the geometry's world matrix, converted the
			// same way and with posAdjust added back. The engine's two routines are called so that the
			// result is the native one to the bit, and this runs at Prepass so posAdjust is the main
			// camera's. TextureProj's rows are the product's columns.
			using ToMatrix = void (*)(float*, const RE::NiTransform*);
			using Multiply = void* (*)(float*, const float*, const float*);
			static const REL::Relocation<ToMatrix> toMatrix{ REL::Offset(0x14aaf10) };
			static const REL::Relocation<Multiply> multiply{ REL::Offset(0x153d3c8) };
			RE::NiTransform projection;
			projection.rotate.entry[0][0] = 0.0f;
			projection.rotate.entry[0][1] = 1.0f;
			projection.rotate.entry[0][2] = 0.0f;
			projection.rotate.entry[1][0] = -1.0f;
			projection.rotate.entry[1][1] = 0.0f;
			projection.rotate.entry[1][2] = 0.0f;
			projection.rotate.entry[2][0] = 0.0f;
			projection.rotate.entry[2][1] = 0.0f;
			projection.rotate.entry[2][2] = 1.0f;
			projection.translate = eye;
			projection.scale = 1.0f;
			float p[16], m[16];
			toMatrix(p, &projection);
			const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
			if (technique == 1) {
				std::memcpy(m, p, sizeof(m));
			} else {
				float w[16];
				toMatrix(w, &a_geometry.world);
				w[12] += eye.x;
				w[13] += eye.y;
				w[14] += eye.z;
				multiply(m, w, p);
			}
			float* proj = rows + kExtraRowTextureProj * 4;
			for (std::uint32_t r = 0; r < 3; ++r) {
				proj[r * 4 + 0] = m[0 + r];
				proj[r * 4 + 1] = m[4 + r];
				proj[r * 4 + 2] = m[8 + r];
				proj[r * 4 + 3] = m[12 + r];
			}
			// The pixel parameters (FUN_1414e00c0): the property's projectedUVParams folded by its w, its
			// projectedUVColor, and the two tiling globals with the projected-normals switch.
			static const REL::Relocation<std::uintptr_t> tilingDiffuse{ REL::Offset(0x2035560) };
			static const REL::Relocation<std::uintptr_t> tilingDetail{ REL::Offset(0x2035578) };
			static const REL::Relocation<std::uintptr_t> projectedNormals{ REL::Offset(0x2035518) };
			const auto& params = a_property.projectedUVParams;
			const auto& colour = a_property.projectedUVColor;
			float* out = rows + kExtraRowProjectedParams * 4;
			const float fade = 1.0f - params.alpha;
			out[0] = fade * params.red;
			out[1] = 0.0f;  // never written by the engine
			out[2] = params.blue;
			out[3] = fade * params.green + params.alpha;
			out[4] = colour.red;
			out[5] = colour.green;
			out[6] = colour.blue;
			out[7] = colour.alpha;
			out[8] = GlobalFloatAt(tilingDiffuse);
			out[9] = GlobalFloatAt(tilingDetail);
			out[10] = 0.0f;
			out[11] = *reinterpret_cast<const std::uint8_t*>(projectedNormals.address()) ? 1.0f : 0.0f;
		}
	}

	void SceneStore::NoteProjectedTextures()
	{
		auto& state = globals::game::shadowState->GetRuntimeData();
		ProjectedTextures seen{};
		for (std::size_t i = 0; i < ProjectedTextures::kSlots.size(); ++i) {
			seen.views[i] = reinterpret_cast<ID3D11ShaderResourceView*>(state.PSTexture[ProjectedTextures::kSlots[i]]);
			if (!seen.views[i])
				return;
		}
		seen.valid = true;
		projectedTextures = seen;
	}

	void SceneStore::LatchAccumulator()
	{
		auto* accumulator = *globals::game::currentAccumulator.get();
		if (!accumulator || accumulator == latchedAccumulator)
			return;
		if (latchedAccumulator) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				logger::warn("[DCLF] the main camera's accumulator changed ({} -> {}); the tables follow it",
					static_cast<const void*>(latchedAccumulator), static_cast<const void*>(accumulator));
			}
		}
		latchedAccumulator = accumulator;
	}

	bool SceneStore::RefreshMainBatchRenderers()
	{
		auto* accumulator = latchedAccumulator ? latchedAccumulator : *globals::game::currentAccumulator.get();
		auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
		if (!batch)
			return false;  // before the first latch, i.e. the first frame only
		mainBatchRenderers.clear();
		mainBatchRenderers.insert(batch);
		for (auto* group : batch->geometryGroups) {
			if (group && group->batchRenderer)
				mainBatchRenderers.insert(group->batchRenderer);
		}
		// Published for the registration hook, which runs before this and so uses the previous frame's
		// set. These pointers are stable across frames, and an empty set on the first frame simply means
		// nothing is withheld yet.
		PassCapture::Get().SetMainBatchRenderers(
			std::make_shared<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>>(mainBatchRenderers));
		return true;
	}

	void SceneStore::CollectAccumulatedPasses()
	{
		accumulatedPasses.clear();
		// The latched accumulator, not `currentAccumulator`. BuildFrame now runs at EarlyPrepass, before
		// the depth pass, where `currentAccumulator` is still null because nothing is being rendered yet -
		// but the accumulator has held its passes since the cull job finished, which `Main::Draw` does
		// before the shadow maps. Measured: 612 passes at EarlyPrepass, at the end of the depth pass and
		// at Prepass alike, against 0 from `currentAccumulator` at the first two.
		auto* accumulator = latchedAccumulator ? latchedAccumulator : *globals::game::currentAccumulator.get();
		auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
		if (!batch)
			return;  // before the first latch, i.e. the first frame only

		// BSBatchRenderer::renderPass holds PassGroup structs inline (the engine indexes it as
		// data + (pass + group * 6) * 8), not the PassGroup pointers CommonLib declares; each of the five
		// entries heads a list chained through passGroupNext. renderPassMap maps each group's technique
		// (what SetupTechnique receives) to its index; the engine reads it as buckets of
		// { key, value, next } at +0x48, bucket count at +0x2C (engine notes: batch renderer).
		struct MapEntry
		{
			std::uint32_t key;
			std::uint32_t value;
			const MapEntry* next;
		};
		auto addBatch = [&](const RE::BSBatchRenderer* a_batch) {
			const auto* base = reinterpret_cast<const std::uint8_t*>(a_batch);
			const auto* buckets = *reinterpret_cast<const MapEntry* const*>(base + 0x48);
			const std::uint32_t bucketCount = *reinterpret_cast<const std::uint32_t*>(base + 0x2c);
			const auto* groups = reinterpret_cast<const RE::BSBatchRenderer::PassGroup*>(a_batch->renderPass.data());
			const std::uint32_t groupCount = a_batch->renderPass.size();
			for (std::uint32_t b = 0; buckets && groups && b < bucketCount; ++b) {
				const auto& entry = buckets[b];
				if (!entry.next || entry.value >= groupCount)
					continue;  // empty bucket
				const std::uint32_t technique = entry.key;
				const auto& group = groups[entry.value];
				for (std::uint32_t subPass = 0; subPass < 5; ++subPass) {
					std::uint32_t chainIndex = 0;
					for (auto* pass = group.passes[subPass]; pass; pass = pass->passGroupNext, ++chainIndex) {
						if (pass->geometry && pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Lighting)
							AddAccumulatedPass(pass->geometry, AccumulatedPass{ pass, DrawnPassDescriptor(PassDescriptorOf(technique), subPass), subPass,
																   pass->passEnum, pass->accumulationHint, chainIndex, PassCapture::FadingAtRegistration(pass), LodRowOf(*pass) });
					}
				}
			}
		};
		addBatch(batch);
		// Geometry groups sort their passes in batch renderers of their own.
		for (auto* group : batch->geometryGroups) {
			if (group && group->batchRenderer)
				addBatch(group->batchRenderer);
		}
	}

	void SceneStore::CompareCapturedPasses(bool a_compare)
	{
		auto& capture = PassCapture::Get();
		if (!capture.Installed())
			return;
		// Always drained, whether or not anything is compared: the capture buffer is fixed-capacity and a
		// frame that does not drain it overflows.
		const auto entries = capture.Drain();
		auto& captureStats = capture.MutableStats();
		captureStats.compared = captureStats.missing = captureStats.extra = captureStats.techniqueDiffers = captureStats.subPassDiffers = 0;
		static const bool fromAccumulator = SwitchValue("CS_DCLF_PASS_SOURCE") == "accumulator";
		if (!a_compare && !fromAccumulator) {
			// The normal path needs only the first registration per geometry. Insert directly into the
			// retained table instead of allocating a second map and hashing every geometry twice.
			bool foundMain = false;
			for (const auto& entry : entries) {
				if (!mainBatchRenderers.contains(entry.batch))
					continue;
				if (!foundMain) {
					accumulatedPasses.clear();
					foundMain = true;
				}
				// Preserve the capture's first-registration rule, including duplicate hint-10 passes.
				accumulatedPasses.try_emplace(entry.geometry,
					AccumulatedPass{ entry.pass, DrawnPassDescriptor(PassDescriptorOf(entry.technique), entry.subPass), entry.subPass, entry.passEnum,
						entry.pass ? static_cast<std::uint32_t>(entry.pass->accumulationHint) : 0u,
						0xFFFFFFu - static_cast<std::uint32_t>(std::min<std::size_t>(static_cast<std::size_t>(&entry - entries.data()), 0xFFFFFFu)),
						entry.fading, entry.pass ? LodRowOf(*entry.pass) : 3u });
			}
			return;  // An empty capture preserves the caller's accumulator fallback.
		}

		// Only the main camera's registrations; the shadow cameras register into their own renderers.
		ankerl::unordered_dense::map<const RE::BSGeometry*, const PassCapture::Entry*> captured;
		for (const auto& entry : entries) {
			if (mainBatchRenderers.contains(entry.batch))
				captured.try_emplace(entry.geometry, &entry);
		}

		for (const auto& [geometry, accumulated] : a_compare ? accumulatedPasses : decltype(accumulatedPasses){}) {
			++captureStats.compared;
			const auto it = captured.find(geometry);
			if (it == captured.end()) {
				++captureStats.missing;
				continue;
			}
			if (DrawnPassDescriptor(PassDescriptorOf(it->second->technique), it->second->subPass) != accumulated.technique)
				++captureStats.techniqueDiffers;
			if (it->second->subPass != accumulated.subPass)
				++captureStats.subPassDiffers;
		}
		if (a_compare) {
			for (const auto& [geometry, entry] : captured) {
				if (!accumulatedPasses.contains(geometry))
					++captureStats.extra;
			}
		}

		// The tables are built from the capture rather than the accumulator walk once the two agree. The
		// walk stops working the moment a pass is withheld from the batch renderer, which is the whole
		// point of static ownership; the capture sees the registration regardless of what happens to it
		// afterwards. Falling back when the capture is empty keeps the first frame and any unexpected
		// path working.
		if (fromAccumulator || captured.empty())
			return;
		accumulatedPasses.clear();
		for (const auto& [geometry, entry] : captured) {
			// The hint is read off the pass now, on the render thread, while the pass is alive for the
			// frame. RegisterPass PREPENDS to its list (Ghidra: passGroupNext = head; head = pass), so the
			// engine draws a bucket in reverse registration order; the chain position is reversed here so
			// that an ascending sort on it is the draw order, as it is for the accumulator walk.
			AddAccumulatedPass(geometry,
				AccumulatedPass{ entry->pass, DrawnPassDescriptor(PassDescriptorOf(entry->technique), entry->subPass), entry->subPass, entry->passEnum,
					entry->pass ? static_cast<std::uint32_t>(entry->pass->accumulationHint) : 0u,
					0xFFFFFFu - static_cast<std::uint32_t>(std::min<std::size_t>(static_cast<std::size_t>(entry - entries.data()), 0xFFFFFFu)),
					entry->fading, entry->pass ? LodRowOf(*entry->pass) : 3u });
		}
	}

	void SceneStore::AddAccumulatedPass(const RE::BSGeometry* a_geometry, const AccumulatedPass& a_pass)
	{
		// One pass per object, the first registered - except that a hint-10 pass (a LOD cross-fade's copy of the
		// old level, the native loop's) never stands for an object that also has a pass of its own.
		const auto [it, inserted] = accumulatedPasses.try_emplace(a_geometry, a_pass);
		if (!inserted && it->second.hint == 10 && a_pass.hint != 10)
			it->second = a_pass;
	}

	const AccumulatedPass* SceneStore::FindAccumulatedPass(const RE::BSGeometry* a_geometry) const
	{
		auto it = accumulatedPasses.find(a_geometry);
		return it == accumulatedPasses.end() ? nullptr : &it->second;
	}

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
		struct PartTimer
		{
			std::array<double, static_cast<std::size_t>(BuildPart::Count)>* parts = nullptr;
			std::chrono::steady_clock::time_point last;

			explicit PartTimer(std::array<double, static_cast<std::size_t>(BuildPart::Count)>& a_parts)
			{
				if (SceneStore::ProfileEnabled()) {
					parts = &a_parts;
					last = std::chrono::steady_clock::now();
				}
			}

			void Add(BuildPart a_part)
			{
				if (!parts)
					return;
				const auto now = std::chrono::steady_clock::now();
				(*parts)[static_cast<std::size_t>(a_part)] += std::chrono::duration<double, std::milli>(now - last).count();
				last = now;
			}
	};

	void SceneStore::MaterialReference::reset(RE::BSShaderMaterial* a_material)
	{
		// BSIntrusiveRefCounted's count (+0x8), incremented as the engine takes a reference on a property's material
		// (FUN_1414ac820); a material being read off a live property has one already, so it cannot be at zero here.
		if (a_material)
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(reinterpret_cast<std::byte*>(a_material) + 0x8));
		if (material) {
			using Release = void(void*, RE::BSShaderMaterial*);
			static REL::Relocation<Release*> release{ REL::Offset(0x14f7a40) };  // AE ID 107720
			static REL::Relocation<void**> manager{ REL::Offset(0x3187758) };     // AE ID 403555
			release(*manager, material);
		}
		material = a_material;
	}

	bool SceneStore::MaterialCacheEnabled()
	{
		// Default ON. It is not merely parity-neutral, it is parity-*better* than evaluating every
		// material: because RefreshFrameMaterials resamples the frame's lighting floats at Prepass rather
		// than at EarlyPrepass, the cache fixes a pre-existing mismatch the uncached path has. Measured
		// over the same route, mismatched draws per report interval:
		//
		//                       cache off   cache on
		//   cell change (coc)      253239      20572
		//   `set gamehour` step    248670          0
		//   steady state                0          0
		// AE only: its reference goes through the engine's material database (MaterialReference).
		static const std::string mode = SwitchValue("CS_DCLF_MATERIAL_CACHE");
		static const bool enabled = mode != "off" && REL::Module::IsAE();
		return enabled;
	}

	bool SceneStore::ProfileEnabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_PROFILE");
		return enabled;
	}

	bool SceneStore::EvaluateMaterialForSlot(const RE::BSShaderMaterial* a_material, std::uint32_t a_pass,
		bool a_cacheOn, bool a_probeAll, MaterialRecord& a_record)
	{
		// The cross-frame material cache.
		//
		// EvaluateMaterial is the single most expensive call in this loop: a heap allocation, a full
		// RendererShadowState memcpy, six ~1 KB ConstantBlock resets, 28 COM releases and the engine's real
		// SetupMaterial with every CS hook on it. A record is a function of the material's own fields, which
		// change only through a write event (MaterialSources; ProcessMaterialWrites drops or re-evaluates the
		// material then), and of frame-sourced components refreshed every frame (RefreshTextureTransforms,
		// RefreshFrameMaterials). So a cached record is served as it is. CS_DCLF_MATERIAL_CACHE=probe
		// re-evaluates every one anyway and compares: a difference outside the frame-sourced components is a
		// writer the events do not cover.
		const std::pair cacheKey{ a_material, a_pass };
		auto cached = materialCache.find(cacheKey);
		const bool canServe = a_cacheOn && cached != materialCache.end();
		if (canServe && !a_probeAll) {
			a_record = cached->second.record;
			++stats.materialsFromCache;
		} else {
			if (!ConstantEvaluator::Get().EvaluateMaterial(a_material, a_pass, a_record)) {
				// No shader instance yet (nothing drawn so far): stay native this frame.
				++stats.ineligible[static_cast<std::size_t>(Ineligible::NotLightingShader)];
				--stats.ineligible[static_cast<std::size_t>(Ineligible::None)];
				return false;
			}
			++stats.materialsEvaluated;
			if (canServe) {
				// The probe: the frame behaves as it would with the cache on, the live record is the yardstick.
				++stats.materialsValidated;
				MaterialRecord served = cached->second.record;
				MaterialSources::CopyFrameComponents(a_record, served, a_pass);
				if (!(served == a_record))
					NoteStaleMaterial(~0u, cacheKey, served, a_record);
				a_record = served;
			} else {
				auto& cacheEntry = materialCache[cacheKey];
				// The reference is what makes the key safe: BSShaderMaterial is intrusively ref-counted, so
				// holding one means a freed material cannot be mistaken for a new allocation at the same
				// address - which is the one way this cache could hand an object another material's state.
				if (!cacheEntry.material)
					cacheEntry.material.reset(const_cast<RE::BSShaderMaterial*>(a_material));
				cacheEntry.record = a_record;
			}
		}
		if (auto it = materialCache.find(cacheKey); it != materialCache.end())
			it->second.lastUsed = frame;
		return true;
	}

	void SceneStore::NoteStaleMaterial(std::uint32_t a_slot, const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key,
		const MaterialRecord& a_served, const MaterialRecord& a_live)
	{
		++stats.materialCacheStale;
		if (a_served.vs.floats != a_live.vs.floats)
			stats.materialDiffMask |= 1u << 0;
		if (a_served.ps.floats != a_live.ps.floats)
			stats.materialDiffMask |= 1u << 1;
		if (a_served.textures != a_live.textures)
			stats.materialDiffMask |= 1u << 2;
		if (a_served.addressModes != a_live.addressModes)
			stats.materialDiffMask |= 1u << 3;
		if (a_served.filterModes != a_live.filterModes)
			stats.materialDiffMask |= 1u << 4;
		if (a_served.textureWritten != a_live.textureWritten)
			stats.materialDiffMask |= 1u << 5;
		static std::uint32_t logged = 0;
		if (logged++ >= 16)
			return;
		std::string what;
		for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f) {
			if (a_served.vs.floats[f] != a_live.vs.floats[f])
				what += fmt::format(" vs[{}] {}->{}", f, a_served.vs.floats[f], a_live.vs.floats[f]);
			if (a_served.ps.floats[f] != a_live.ps.floats[f])
				what += fmt::format(" ps[{}] {}->{}", f, a_served.ps.floats[f], a_live.ps.floats[f]);
		}
		for (std::size_t t = 0; t < a_served.textures.size(); ++t)
			if (a_served.textures[t] != a_live.textures[t] || a_served.addressModes[t] != a_live.addressModes[t] || a_served.filterModes[t] != a_live.filterModes[t])
				what += fmt::format(" texture[{}] {}/{}/{} -> {}/{}/{}", t, static_cast<const void*>(a_served.textures[t]), a_served.addressModes[t],
					a_served.filterModes[t], static_cast<const void*>(a_live.textures[t]), a_live.addressModes[t], a_live.filterModes[t]);
		if (a_served.textureWritten != a_live.textureWritten)
			what += fmt::format(" written {:X}->{:X}", a_served.textureWritten, a_live.textureWritten);
		logger::warn("[DCLF] STALE material record{} (material {}, pass {:X}): a writer the material events do not cover:{}",
			a_slot == ~0u ? std::string() : fmt::format(" in slot {}", a_slot), fmt::ptr(a_key.first), a_key.second, what.substr(0, 600));
	}

	std::uint32_t SceneStore::TechniqueRowFor(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t key = TechniqueKey(a_passDescriptor);
		const auto [it, fresh] = tables.techniqueRow.try_emplace(key, static_cast<std::uint32_t>(tables.techniques.size()));
		if (fresh) {
			auto& row = tables.techniques.emplace_back();
			row.key = key;
			EvaluateTechnique(a_passDescriptor, row.value);
			row.evaluated = frame;
			row.constantsVersion = tables.NextVersion();
			row.bindingVersion = tables.NextVersion();
		}
		return it->second;
	}

	namespace
	{
		// Grows each column of a table by one slot (Tables::GeometryColumns and the like).
		constexpr auto kGrowColumn = [](auto& a_column, auto&&... a_initial) { a_column.emplace_back(a_initial...); };
	}

	std::uint32_t SceneStore::AllocateGeometrySlot()
	{
		const auto allocation = tables.geometrySlots.Allocate(frame);
		if (allocation.grown) {
			tables.GeometryColumns(kGrowColumn);
			tables.NoteGeometry(allocation.slot);
		}
		return allocation.slot;
	}

	std::uint32_t SceneStore::AllocatePipelineSlot()
	{
		const auto allocation = tables.pipelineSlots.Allocate(frame);
		if (allocation.grown)
			tables.PipelineColumns(kGrowColumn);
		return allocation.slot;
	}

	std::uint32_t SceneStore::AllocateMaterialSlot()
	{
		const auto allocation = tables.materialSlots.Allocate(frame);
		if (allocation.grown)
			tables.MaterialColumns(kGrowColumn);
		return allocation.slot;
	}

	namespace
	{
		/**
		 * @brief Whether an ineligibility verdict is one the scene phase cannot reach on its own.
		 *
		 * The decal rule is the only one: the group a decal draws in comes from its accumulated pass's
		 * accumulation hint (LightingDescriptors, the engine's GetRenderPasses), which does not exist
		 * before the shadow maps. Rejecting on it in the scene phase would leave every decal the main
		 * pass draws without a record, so the record is built and the verdict is taken again, with the
		 * pass, by the accumulate phase.
		 */
		bool DeferredToAccumulate(Ineligible a_reason)
		{
			return a_reason == Ineligible::Decal;
		}

		/**
		 * @brief Whether an object the main pass cannot take is still a shadow caster DCLF can draw: its reason is
		 * one the shadow views are indifferent to, and the engine would draw it into a shadow map. Measured
		 * (CS_DCLF_VOLUMETRIC_PROBE, the engine's cascade draws with DCLF on) and reverse engineered per reason:
		 * - Technique: the lighting technique is outside the main pass's set (the terrain's landscape blocks);
		 *   a shadow view draws the Utility technique, which ShadowUtilityTechnique derives from the property.
		 * - UnsupportedParent: under a BSOrderedNode (hay), which orders blended draws and nothing else.
		 * Not a billboard: NiBillboardNode turns to the culling camera, which for a shadow view is the light's.
		 */
		bool ShadowOnlyReason(Ineligible a_reason)
		{
			return a_reason == Ineligible::Technique || a_reason == Ineligible::UnsupportedParent;
		}

		// CS_DCLF_SHADOW_ONLY=0 turns shadow-only objects off (default on).
		bool ShadowOnlyEnabled()
		{
			static const bool enabled = SwitchValue("CS_DCLF_SHADOW_ONLY") != "0";
			return enabled;
		}

		bool ShadowOnlyCaster(Ineligible a_reason, RE::BSGeometry& a_geometry)
		{
			if (!ShadowOnlyEnabled() || !ShadowOnlyReason(a_reason))
				return false;
			const auto reject = ShadowCasterReject(a_geometry.GetGeometryRuntimeData().shaderProperty.get(), &a_geometry);
			return reject == ShadowReject::None || (reject == ShadowReject::VolumetricOnly && PassCapture::VolumetricClaimsAvailable());
		}
	}

	void SceneStore::BuildFrame(Phase a_phase)
	{
		if (a_phase == Phase::Scene)
			BuildScenePhase();
		else
			BuildAccumulatePhase();
	}

	/**
	 * @brief The scene half of the frame, from Main::Draw's early hook (else at BeforeShadowMaps).
	 *
	 * Everything that does not depend on the main camera's accumulator: the tracked walk, eligibility, the
	 * geometry slots and their buffer resolve, the transforms and bounds, the bone palettes, and one object
	 * record per eligible object. It runs before the shadow maps are drawn - the scene graph is final from
	 * Main::Draw on, and a shadow epoch reads these records. The early hook is ahead of the main camera's
	 * cull; what moves before BeforeShadowMaps is listed at DrawcallLimitFix::BeginSceneFrame.
	 *
	 * The records leave the accumulator's half unset: no pipeline, no material, kObjectNoBindings, and
	 * kObjectNativeVisible clear. BuildAccumulatePhase patches them in place by object index, which is
	 * fixed for the frame from here on.
	 */
	void SceneStore::BuildScenePhase()
	{
		// A walk still pending here was never joined (no consumer of the tables ran): it is dropped, and
		// there are never two.
		AbandonSceneJob();
		++frame;
		// The change log keeps its tail (Tables::changeLog): a reader that has not read past the trimmed half reads every
		// slot again.
		tables.changeLog.Trim(1u << 17);
		tables.geometryLog.Trim(1u << 16);
		CheckChangeLog();
		// Nothing is drawn while a load screen is up, and nothing here may touch the tracked geometry
		// either. The load frees the renderer data and the vertex and index buffers of the cell being
		// unloaded, while the NiPointers in `tracked` keep only the NiAVObjects alive; classifying those
		// entries reads freed BSGraphics::TriShape data and hands the render graph device addresses that no
		// longer belong to anything, which the GPU answers with VK_ERROR_DEVICE_LOST a few frames later.
		//
		// Before ProcessEvents stopped walking the scene graph across loads, this did not arise: the
		// category-node refresh pruned those entries as the cell's nodes vanished, so they never reached
		// this loop. Leaving the tables empty is both the safe and the obviously correct thing to draw
		// during a load screen.
		sceneBuilt = false;
		if (IsLoadingScreenUp()) {
			ResetSlotTables();
			InvalidateObjectIndices();
			accumulatedPasses.clear();
			stats.objects = 0;
			stats.nativeVisible = stats.nativeShadowMasked = stats.derivedDescriptors = 0;
			stats.geometries = 0;
			stats.pipelines = 0;
			stats.materials = 0;
			skinnedLastFrame.clear();
			return;
		}
		PartTimer timer(stats.partMs);
		RefreshLodFadeSettings();
		auto& gpu = GpuResources::Get();
		gpu.BeginFrame(frame);
		const bool resolveBuffers = gpu.Enabled();
		frameResolveBuffers = resolveBuffers;
		if (resolveBuffers != graphWasActive) {
			logger::info("[DCLF] tables frame {}: the render graph is {} (slots alive {} geometries / {} pipelines / {} materials)", frame,
				resolveBuffers ? "active from this frame" : "inactive from this frame", stats.geometriesAlive, stats.pipelinesAlive, stats.materialsAlive);
			graphWasActive = resolveBuffers;
		}
		// Frame-globals that were being read per object. ShouldSuppress in particular is three terms, and
		// only one of them is per object - the interior test is the same answer for every object in the
		// frame. The depth-bias mode of each decal group is frame state (a console toggle and whether sun
		// shadows are off), read once here rather than per decal.
		frameInterior = Util::IsInterior();
		frameDecalBias = { 0u, DecalDepthBiasMode(1), DecalDepthBiasMode(2) };
		timer.Add(BuildPart::Walk);
		// The slot tables and their maps persist across frames; a slot no object has referenced for SlotTable::kIdleFrames
		// is retired here (CS_DCLF_DERIVED_CACHE).
		SweepSlots();
		auto& evaluator = ConstantEvaluator::Get();
		ConstantEvaluator::ResetFrameAudits();
		if (!evaluator.HasLightingShader())
			FindLightingShader();

		geometryIndex.reserve(tracked.size());

		CaptureCullHiddenBits();

		// CS_DCLF_WALK_PARITY=1: every 60 frames the walk runs here on the render thread, and a dense rebuild is
		// compared with its slot tables object by object.
		static const bool walkParityOn = SwitchEnabled("CS_DCLF_WALK_PARITY");
		const bool parityFrame = walkParityOn && ObjectSlotsEnabled() && frame % 60 == 0;

		// CS_DCLF_SCENE_DELTA: only what can have changed, here on the render thread; nothing is left for a worker.
		if (SceneDeltaEnabled()) {
			DeltaWalk();
			if (parityFrame)
				CheckWalkParity();
			skinnedLastFrame = skinnedObjects;
			stats.objects = tables.liveObjects;
			sceneBuilt = true;
			return;
		}

		// The whole tracked set, in whatever order the map holds; BuildAccumulatePhase walks the same vector.
		BuildFullOrder();
		if (parityFrame) {
			SceneWalk(true);
			CheckWalkParity();
			skinnedLastFrame = skinnedObjects;
			stats.objects = tables.liveObjects;
			sceneBuilt = true;
			return;
		}

		// CS_DCLF_ASYNC: the walk on the worker, from here to AfterShadowMaps - the engine's main cull and its
		// whole shadow-map pass. Nothing reads the per-object tables in between (ExecuteShadowView does not;
		// the shadow probe keeps the walk here), the tracked set changes only at Present, and the engine data
		// the walk reads is the data the render thread reads here (see BuildScenePhase for what the cull
		// writes). The shadow build queued behind it reads what it writes.
		if (AsyncJobEnabled("scene") && !ShadowProbe::Enabled()) {
			PrepareSceneJob();
			++sceneAsync.kicked;
			sceneJob = AsyncWorker::Get().Submit("scene", [this](std::stop_token) { sceneJobResult = SceneWalk(false); });
			return;
		}
		SceneWalk(true);
		skinnedLastFrame = skinnedObjects;
		stats.objects = tables.liveObjects;
		sceneBuilt = true;
	}

	std::uint32_t SceneStore::FaceRegionOf(const RE::BSGeometry* a_geometry, std::uint32_t a_vertexCount)
	{
		auto& region = faceRegions[a_geometry];
		if (region.count != a_vertexCount) {
			if (region.count)
				faceRegionFree.push_back({ region.first, region.count });
			region = {};
			// First fit among the freed ranges, else the top of the buffer.
			for (auto it = faceRegionFree.begin(); it != faceRegionFree.end(); ++it) {
				if (it->second < a_vertexCount)
					continue;
				region.first = it->first;
				region.count = a_vertexCount;
				it->first += a_vertexCount;
				it->second -= a_vertexCount;
				if (it->second == 0)
					faceRegionFree.erase(it);
				break;
			}
			if (!region.count) {
				if (faceRegionTop + a_vertexCount > kFacePositionVertices) {
					faceRegions.erase(a_geometry);
					return kNoFaceRegion;
				}
				region.first = faceRegionTop;
				region.count = a_vertexCount;
				faceRegionTop += a_vertexCount;
			}
		}
		region.seenWalk = faceWalk;
		return region.first;
	}

	void SceneStore::EndFaceWalk()
	{
		for (auto it = faceRegions.begin(); it != faceRegions.end();) {
			if (it->second.seenWalk == faceWalk) {
				++it;
				continue;
			}
			faceRegionFree.push_back({ it->second.first, it->second.count });
			it = faceRegions.erase(it);
		}
		// Sorted and coalesced, so a run of freed shapes is one range again.
		std::sort(faceRegionFree.begin(), faceRegionFree.end());
		std::size_t out = 0;
		for (std::size_t i = 0; i < faceRegionFree.size(); ++i) {
			if (out && faceRegionFree[out - 1].first + faceRegionFree[out - 1].second == faceRegionFree[i].first)
				faceRegionFree[out - 1].second += faceRegionFree[i].second;
			else
				faceRegionFree[out++] = faceRegionFree[i];
		}
		faceRegionFree.resize(out);
		if (!faceRegionFree.empty() && faceRegionFree.back().first + faceRegionFree.back().second == faceRegionTop) {
			faceRegionTop = faceRegionFree.back().first;
			faceRegionFree.pop_back();
		}
		if (FaceSnapshots::Enabled())
			FaceSnapshots::Get().EndWalk();
	}

	void SceneStore::BeginWalk(bool a_keepIndices)
	{
		const bool keepObjects = ObjectSlotsEnabled() && !denseWalk;
		tables.ClearFrame(keepObjects);
		if (!a_keepIndices)
			InvalidateObjectIndices();
		++walkSerial;
		++faceWalk;
		if (FaceSnapshots::Enabled())
			FaceSnapshots::Get().BeginWalk();
		stats.ineligible.fill(0);
		stats.ineligibleDrawn.fill(0);
		stats.techniqueRejects.fill(0);
		stats.propertyRejects.clear();
		stats.rejectedBlended = stats.rejectedOpaque = stats.rejectedOpaqueAlphaTest = 0;
		stats.shadowMaskPipelines = 0;
		stats.derivationChecked = stats.derivationDiffers = stats.derivationBits = stats.derivationNative = 0;
		stats.derivationRuntimeDiffers = stats.derivationRuntimeBits = 0;
		stats.derivationBitCounts.fill(0);
		stats.materialsEvaluated = stats.materialsSkipped = 0;
		stats.materialsUnchanged = stats.materialsChanged = stats.materialDiffMask = 0;
		stats.materialsFromCache = stats.materialsValidated = stats.materialCacheStale = 0;
		stats.templateUpgrades = stats.templateDefects = stats.pipelinesCulledOnly = 0;
		stats.nativeVisible = stats.nativeShadowMasked = stats.derivedDescriptors = 0;
		stats.classifyHits = stats.classifyChecked = stats.classifyDiffers = stats.castResolved = 0;
		stats.derivedHits = stats.derivedChecked = stats.derivedDiffers = 0;
		stats.accumulatedWithoutRecord = 0;
		stats.decals = {};
		stats.skinned = stats.boneRows = 0;
		stats.projectedUV = stats.landBlend = 0;
		stats.shadowCasters = 0;
		stats.shadowRejects = {};
		decalOrder.clear();

		skinnedObjects.clear();

		// Every per-object container, not only three of them. The three that were left out reallocated
		// their way back up every frame, and objectIndex - cleared just above - rehashed its way up to
		// ~2900 entries in the Whiterun exterior, which the profile billed to `record`.
		if (!keepObjects) {
			tables.objects.reserve(tracked.size());
			tables.objectGeometry.reserve(tracked.size());
			tables.draws.reserve(tracked.size());
			tables.shading.reserve(tracked.size());
			tables.emissiveMult.reserve(tracked.size());
			tables.lights.reserve(tracked.size());
			tables.treeAnim.reserve(tracked.size());
			tables.skinPartitions.reserve(tracked.size());
		}

	}

	SceneStore::WalkResult SceneStore::SceneWalk(bool a_renderThread)
	{
		WalkResult result;
		BeginWalk();
		PartTimer timer(stats.partMs);
		timer.Add(BuildPart::PassLookup);
		for (auto& [geometry, trackedEntry, unused] : order) {
			// Per TRACKED object: for a rejected one this is its `continue` and the iteration itself.
			timer.Add(BuildPart::LoopTail);
			Ineligible bucket = Ineligible::Count;
			WriteObject(geometry, *trackedEntry, timer, result, a_renderThread, bucket);
			if (!denseWalk)
				MoveBucket(*trackedEntry, bucket);
		}

		SweepObjectSlots();

		EndFaceWalk();
		tables.InvalidateChangeLog();
		return result;
	}

	bool SceneStore::WriteObject(RE::BSGeometry* geometry, Tracked& a_tracked, PartTimer& timer, WalkResult& result, bool a_renderThread, Ineligible& a_bucket)
	{
		// CS_DCLF_CLASSIFY_CACHE=off|on|probe. `probe` uses the cached verdict and *also* recomputes it,
		// comparing the two; it is the gate, and it costs more than either path alone.
		static const std::string classifyCacheMode = SwitchValue("CS_DCLF_CLASSIFY_CACHE");
		static const bool classifyCache = classifyCacheMode != "off";
		static const bool classifyProbe = classifyCacheMode == "probe";
		// CS_DCLF_COVERAGE_PROBE=1: which shader the uncovered objects actually use.
		static const bool coverageProbe = SwitchEnabled("CS_DCLF_COVERAGE_PROBE");
		auto* trackedEntry = &a_tracked;
		const auto& entry = a_tracked;
		// A resident's record written again: an event changed it, and its patch is gone with the write.
		if (!denseWalk && a_tracked.slot != kNoObjectSlot && IsResidentSlot(a_tracked.slot)) {
			DropResidentSlot(a_tracked.slot, true, false);
			++residentStats.rewritten;
		}

		// Eligibility, and nothing else. This phase needs no lighting descriptors - the pipeline and
		// material belong to the accumulator's half - so the verdict is taken from Tracked's own
		// cache (refreshed every kCandidateRefreshFrames by the full walk, and by an event with the
		// delta walk's) rather than recomputed per object per
		// frame: that cache is what made the cull-only path cheap, and here it covers every object.
		//
		// A verdict that is stale in the "eligible" direction costs nothing: the accumulate phase
		// classifies again before it hands an object any bindings. A verdict stale the other way
		// would leave an accumulated object without a record, so that phase clears the cache for it
		// and counts it (stats.accumulatedWithoutRecord, the gate: 0 in steady state).
		// An NPC face shape (FaceSnapshots), resolved once: a tracked geometry's type and parent do not change.
		if (!trackedEntry->faceShapeResolved) {
			trackedEntry->faceShapeResolved = true;
			trackedEntry->faceShape = geometry->GetType().get() == RE::BSGeometry::Type::kDynamicTriShape && geometry->parent &&
			                          netimmerse_cast<RE::BSFaceGenNiNode*>(geometry->parent);
		}
		const bool faceShape = trackedEntry->faceShape && FaceSnapshots::Enabled();
		// Owned by an actor, as Skin::GetWetness decides it; resolved once, like the face shape.
		if (!trackedEntry->actorOwnedResolved) {
			trackedEntry->actorOwnedResolved = true;
			const auto* owner = geometry->GetUserData();
			trackedEntry->actorOwned = owner && owner->GetFormType() == RE::FormType::ActorCharacter;
		}
		Ineligible reason;
		bool shadowOnly = false;  // not the main pass's, but a caster the shadow epochs draw (kObjectShadowOnly)
		// A face shape is classified every frame: its record also depends on its head's snapshot, and the
		// verdicts it can take (hidden, fading, a decal group) change as the actor does.
		// The dense rebuild (walk parity's reference) classifies from scratch and leaves the caches alone. With
		// CS_DCLF_SCENE_DELTA a classification stands until an event takes it again, except a per-frame entry's written in
		// full (an actor's): its inputs are re-read every frame (Tracked::classifyInputs), as its record is.
		const bool sceneDelta = SceneDeltaEnabled();
		const bool rereads = sceneDelta && trackedEntry->perFrame && !trackedEntry->lightTraits;
		bool cached = !denseWalk && !faceShape && trackedEntry->candidateFrame != 0 &&
		              (sceneDelta || frame - trackedEntry->candidateFrame < Tracked::kCandidateRefreshFrames);
		if (cached && rereads && ClassifyInputsOf(*geometry) != trackedEntry->classifyInputs) {
			cached = false;
			++delta.reread;
		}
		if (cached) {
			reason = trackedEntry->candidateReason;
			// Under a switch node the verdict follows the switch's selection, which changes (a harvested
			// plant, a tree's variant) without anything the cache witnesses: the shadow views draw what
			// this phase admits, so it is taken again every frame. Static verdicts other than None stand.
			// A re-read entry's hidden and actor verdicts are the frame's too.
			const bool frameVerdict = reason == Ineligible::None || reason == Ineligible::Switch ||
			                          (rereads && (reason == Ineligible::Hidden || reason == Ineligible::Actor));
			if ((entry.parentReason == Ineligible::Switch || rereads) && frameVerdict) {
				reason = ClassifyFrame(entry);
				trackedEntry->candidateReason = reason;
			}
			++stats.ineligible[static_cast<std::size_t>(reason)];
			a_bucket = reason;
			shadowOnly = reason != Ineligible::None && !DeferredToAccumulate(reason) && ShadowOnlyCaster(reason, *geometry);
			if (reason != Ineligible::None && !DeferredToAccumulate(reason) && !shadowOnly)
				return false;
		} else {
			LightingDescriptors descriptors;
			auto& verdict = trackedEntry->verdict;
			auto& runtime = geometry->GetGeometryRuntimeData();
			auto* witnessProperty = runtime.shaderProperty.get();
			const auto* witnessMaterial = witnessProperty ? witnessProperty->material : nullptr;
			const std::uint8_t fadeState = FadeStateOf(witnessProperty);
			// Resolve the RTTI cast once per property pointer rather than once per frame.
			if (trackedEntry->castProperty != witnessProperty) {
				trackedEntry->castProperty = witnessProperty;
				trackedEntry->castResult = netimmerse_cast<RE::BSLightingShaderProperty*>(witnessProperty);
				trackedEntry->castRtti = witnessProperty ? witnessProperty->GetRTTI() : nullptr;
				++stats.castResolved;
			}
			RE::BSLightingShaderProperty* castCache = trackedEntry->castResult;
			const bool hit = classifyCache && !denseWalk && verdict.cached && verdict.rendererData == runtime.rendererData &&
			                 verdict.property == witnessProperty && verdict.material == witnessMaterial &&
			                 verdict.fadeState == fadeState;
			if (hit && !classifyProbe) {
				reason = verdict.reason;
				++stats.classifyHits;
			} else {
				reason = ClassifyStatic(*geometry, &descriptors, nullptr, false, &castCache);
				if (hit) {
					// probe: the cache said one thing and the computation another, which is a defect.
					++stats.classifyChecked;
					if (reason != verdict.reason) {
						++stats.classifyDiffers;
						if (stats.classifyDiffers == 1)
							logger::warn("[DCLF] classify cache: '{}' is cached as {} but recomputes as {}",
								geometry->name.c_str() ? geometry->name.c_str() : "?",
								kIneligibleNames[static_cast<std::size_t>(verdict.reason)], kIneligibleNames[static_cast<std::size_t>(reason)]);
					}
					reason = verdict.reason;  // the cache is what the frame would have used
				} else if (denseWalk) {
				} else if (classifyCache && CacheableVerdict(reason)) {
					verdict = { true, reason, runtime.rendererData, witnessProperty, witnessMaterial, fadeState };
				} else if (classifyCache) {
					verdict.cached = false;
				}
			}
			timer.Add(BuildPart::ClassifyStatic);
			if (reason == Ineligible::None)
				reason = ClassifyFrame(entry);
			timer.Add(BuildPart::ClassifyFrame);
			if (denseWalk) {
				referenceReasons[geometry] = reason;
			} else {
				trackedEntry->candidateFrame = frame;
				trackedEntry->candidateReason = reason;
				trackedEntry->classifyInputs = ClassifyInputsOf(*geometry);
			}
			++stats.ineligible[static_cast<std::size_t>(reason)];
			a_bucket = reason;
			if (reason == Ineligible::Technique)
				++stats.techniqueRejects[descriptors.rejectedTechnique & 63];
			if (coverageProbe && reason == Ineligible::NotLightingShader) {
				++stats.propertyRejects[trackedEntry->castRtti];
				const auto* rejectedAlpha = geometry->GetGeometryRuntimeData().alphaProperty.get();
				if (rejectedAlpha && rejectedAlpha->GetAlphaBlending()) {
					++stats.rejectedBlended;
				} else {
					++stats.rejectedOpaque;
					if (rejectedAlpha && rejectedAlpha->GetAlphaTesting())
						++stats.rejectedOpaqueAlphaTest;
				}
			}
			shadowOnly = reason != Ineligible::None && !DeferredToAccumulate(reason) && ShadowOnlyCaster(reason, *geometry);
			if (reason != Ineligible::None && !DeferredToAccumulate(reason) && !shadowOnly)
				return false;
		}

		auto& data = geometry->GetGeometryRuntimeData();
		// A face shape's positions: its head's snapshot (FaceSnapshots), and the region of the positions buffer
		// the epochs upload them to. Every record of a face shape has them, whatever its verdict (a deferred decal
		// included); without a snapshot the shape gets no record, and the engine draws it - and, the snapshot
		// being the head's, every other shape of its head too.
		FaceSnapshots::ShapeView face{};
		std::uint32_t faceRegion = kNoFaceRegion;
		if (faceShape) {
			auto* head = geometry->parent ? netimmerse_cast<RE::BSFaceGenNiNode*>(geometry->parent) : nullptr;
			if (head)
				face = FaceSnapshots::Get().Shape(static_cast<RE::BSDynamicTriShape&>(*geometry), *head);
			if (face.positions)
				faceRegion = FaceRegionOf(geometry, face.vertexCount);
			if (faceRegion == kNoFaceRegion)
				return false;
		}
		// Geometry, shared between every object drawing the same TriShape. A skinned shape draws its
		// skin partitions' own buffers, one draw each (ClassifyStatic has checked every one).
		const auto* skinPartitions = data.skinInstance ? data.skinInstance->skinPartition.get() : nullptr;
		const RE::NiSkinPartition::Partition* skinPartition = skinPartitions ? &skinPartitions->partitions[0] : nullptr;
		// Which of them the engine draws: for the shadow views, from the fade node's LOD level as both of
		// its pass builders read it; the accumulate phase takes the main camera's from its pass. A skin
		// the engine draws no partition of is not drawn at all.
		std::uint32_t partitionMask = 0;
		if (skinPartitions) {
			partitionMask = SkinPartitionMask(*data.skinInstance, LodRowOf(*geometry, data.shaderProperty.get()));
			if (!partitionMask) {
				--stats.ineligible[static_cast<std::size_t>(reason)];
				++stats.ineligible[static_cast<std::size_t>(Ineligible::Hidden)];
				a_bucket = Ineligible::Hidden;
				return false;
			}
		}
		auto* triShape = skinPartition ? skinPartition->buffData : data.rendererData;
		bool geometryMiss = false;
		const std::uint32_t geometrySlot = ResolveGeometrySlot(*geometry, triShape, skinPartition, timer, a_renderThread, geometryMiss);
		if (geometryMiss) {
			++result.geometryMisses;
			return false;
		}
		if (geometrySlot == Tables::kSlotFree) {
			--stats.ineligible[static_cast<std::size_t>(reason)];
			++stats.ineligible[static_cast<std::size_t>(Ineligible::UnstableBuffer)];
			a_bucket = Ineligible::UnstableBuffer;
			return false;
		}
		// The other partitions' slots, linked from the first so a draw can walk them. Every partition is
		// resolved and linked whatever this frame's mask, because the main camera's may differ.
		if (skinPartitions && skinPartitions->numPartitions > 1) {
			std::uint32_t previous = geometrySlot;
			bool unstable = false;
			for (std::uint32_t i = 1; i < skinPartitions->numPartitions && !geometryMiss && !unstable; ++i) {
				const auto& part = skinPartitions->partitions[i];
				const std::uint32_t slot = ResolveGeometrySlot(*geometry, part.buffData, &part, timer, a_renderThread, geometryMiss);
				if (geometryMiss)
					break;
				if (slot == Tables::kSlotFree) {
					unstable = true;
					break;
				}
				if (tables.geometries[previous].nextPartition != slot) {
					tables.geometries[previous].nextPartition = slot;
					tables.NoteGeometry(previous);
				}
				previous = slot;
			}
			if (geometryMiss) {
				++result.geometryMisses;
				return false;
			}
			if (unstable) {
				--stats.ineligible[static_cast<std::size_t>(reason)];
				++stats.ineligible[static_cast<std::size_t>(Ineligible::UnstableBuffer)];
			a_bucket = Ineligible::UnstableBuffer;
				return false;
			}
			if (tables.geometries[previous].nextPartition != kNoPartition) {
				tables.geometries[previous].nextPartition = kNoPartition;
				tables.NoteGeometry(previous);
			}
		}

		ObjectRecord object{};
		// CS_DCLF_TRANSFORM_PROBE=skip: a COST BOUND, not a mode. It renders wrong - every object
		// collapses to the origin - and exists only to answer how much of `record` the two transform
		// stores are.
		static const bool skipTransforms = SwitchValue("CS_DCLF_TRANSFORM_PROBE") == "skip";
		if (!skipTransforms) {
			StoreTransform(geometry->world, object.world);
			StoreTransform(geometry->previousWorld, object.previousWorld);
		}
		object.boundCenter[0] = geometry->worldBound.center.x;
		object.boundCenter[1] = geometry->worldBound.center.y;
		object.boundCenter[2] = geometry->worldBound.center.z;
		object.boundRadius = geometry->worldBound.radius;
		object.geometryIndex = geometrySlot;
		// The accumulator's half is not known yet: no bindings, and not native-visible. Both are
		// patched by BuildAccumulatePhase, and nothing between the two phases reads them -
		// BuildDrawsCS rejects an object with kObjectNoBindings before it looks at the indices.
		object.materialIndex = 0;
		object.pipelineIndex = 0;
		object.flags = kObjectNoBindings | (shadowOnly ? kObjectShadowOnly : 0u);
		// Two-sidedness is the property's, not the pass's: the shadow epochs key their pipelines on it
		// before the accumulate phase has computed the static flags, and that phase derives the same bit.
		if (auto* sceneProperty = data.shaderProperty.get(); sceneProperty && sceneProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided))
			object.flags |= kObjectTwoSided;
		// Likewise the alpha test and its threshold: a shadow caster's alpha reference comes from the
		// object record (Utility.hlsl, DCLFAlphaTestRef), which is packed before the accumulate phase.
		if (const auto* sceneAlpha = data.alphaProperty.get(); sceneAlpha && sceneAlpha->GetAlphaTesting())
			object.flags |= kObjectAlphaTest | (static_cast<std::uint32_t>(sceneAlpha->alphaThreshold) << kObjectAlphaThresholdShift);

		// Skinning: the engine's own palette. Its per-frame update (AE FUN_140e4ff90) is what the bone
		// setter runs from the native draw this object no longer gets; it is idempotent within a frame
		// (frameID), copies the current palette to the previous one first, and writes three float4 rows
		// a bone in absolute world space - which is what the shader indexes, so the rows are copied as
		// they are and made eye-relative by the epoch, like World.
		std::uint32_t objectBoneRows = 0;
		const float* boneCurrent = nullptr;
		const float* bonePrevious = nullptr;
		if (auto* skin = data.skinInstance.get(); skin && SkinnedEnabled()) {
			timer.Add(BuildPart::Record);
			if (trackedEntry->skinUpdatedFrame != frame) {
				// The palette update is the render thread's: off it, a skin PrepareSceneJob did not update
				// ahead (a newly eligible skinned object) is a miss, and the join rebuilds inline.
				if (!a_renderThread) {
					++result.skinMisses;
					return false;
				}
				UpdateSkin(skin, geometry->world);
				trackedEntry->skinUpdatedFrame = frame;
			}
			skinnedObjects.push_back(geometry);
			const std::uint32_t rows = skin->numMatrices * 3;
			if (rows && skin->boneMatrices && skin->prevBoneMatrices && rows <= 240) {
				objectBoneRows = rows;
				boneCurrent = static_cast<const float*>(skin->boneMatrices);
				bonePrevious = static_cast<const float*>(skin->prevBoneMatrices);
				object.flags |= kObjectSkinned;
				++stats.skinned;
				stats.boneRows += rows;
			}
			timer.Add(BuildPart::Skinning);
		}
		// The object's slot: past the last `continue`, so a slot is only ever taken by a record that is written.
		const std::uint32_t slotBefore = trackedEntry->slot;
		const std::uint32_t objectId = AcquireObjectSlot(*trackedEntry, geometry);
		// The accumulated half is the accumulate phase's: a record written again for the same object keeps the patch the
		// last phase gave it, which the next renews or lets lapse (LapseAccumulated). An entry written every frame (a face,
		// an actor's part) would otherwise lose its patch here and take it back there, every frame.
		const bool keepHalf = !denseWalk && objectId == slotBefore && objectId < patchedFrame.size() && patchedFrame[objectId] + 1 == frame &&
		                      !(tables.objects[objectId].flags & kObjectFree);
		tables.objectSeen[objectId] = walkSerial;
		if (objectBoneRows) {
			const std::size_t at = std::size_t(tables.PlaceBones(objectId, objectBoneRows)) * 4;
			const std::size_t bytes = std::size_t(objectBoneRows) * 4 * sizeof(float);
			// Noted only when the rows differ (a skin that stands still uploads nothing).
			if (std::memcmp(&tables.bones[at], boneCurrent, bytes) != 0 || std::memcmp(&tables.previousBones[at], bonePrevious, bytes) != 0) {
				std::memcpy(&tables.bones[at], boneCurrent, bytes);
				std::memcpy(&tables.previousBones[at], bonePrevious, bytes);
				tables.NoteChange(objectId, kChangePalette);
			}
		} else {
			tables.FreeBones(objectId);
		}
		// Whether the engine would draw this object into a shadow map, and with which Utility
		// technique. It belongs here and nowhere else: every shadow view is rendered between this
		// phase and the next, so a verdict taken later would arrive after the views that need it.
		const auto* shadowProperty = data.shaderProperty.get();
		const auto shadowReject = ShadowCasterReject(shadowProperty, geometry);
		static_assert(static_cast<std::size_t>(ShadowReject::Count) <= std::tuple_size_v<decltype(stats.shadowRejects)>);
		++stats.shadowRejects[static_cast<std::size_t>(shadowReject)];
		ID3D11ShaderResourceView* shadowDiffuse = nullptr;
		const RE::BSShaderMaterial* shadowMaterial = nullptr;
		// A volumetric-only caster is one too, for the views of the volumetric lighting copy alone, once the
		// copy's passes can be withheld (PassCapture::VolumetricClaimsAvailable); until then it is the engine's.
		const bool volumetricOnly = shadowReject == ShadowReject::VolumetricOnly && PassCapture::VolumetricClaimsAvailable();
		if (shadowReject == ShadowReject::None || volumetricOnly) {
			++stats.shadowCasters;
			if (volumetricOnly)
				object.flags |= kObjectVolumetricOnly;
			const std::uint32_t shadowTechnique = ShadowUtilityTechnique(shadowProperty, geometry);
			tables.shadowTechnique[objectId] = shadowTechnique;
			// An alpha-tested caster samples its diffuse: what the shadow epoch's binding record names, read
			// here so the epoch's build never touches the property.
			if (shadowTechnique & 0x80) {
				if (const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(shadowProperty->material)) {
					shadowMaterial = material;
					auto* texture = material->diffuseTexture ? material->diffuseTexture->rendererTexture : nullptr;
					shadowDiffuse = texture ? texture->resourceView : nullptr;
					if (shadowDiffuse && tables.shadowTextureSeen.insert(shadowDiffuse).second)
						tables.shadowTextureSet.push_back(shadowDiffuse);
				}
			}
			const ShadowPipelineKey shadowKey{ shadowTechnique,
				shadowProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided) ? kRasterTwoSided : 0u,
				VertexLayoutOf(tables.geometries[geometrySlot].vertexDesc) };
			if (std::find(tables.shadowKeysUsed.begin(), tables.shadowKeysUsed.end(), shadowKey) == tables.shadowKeysUsed.end())
				tables.shadowKeysUsed.push_back(shadowKey);
		} else {
			object.flags |= kObjectNoShadow;
			tables.shadowTechnique[objectId] = 0;
		}
		tables.shadowReject[objectId] = static_cast<std::uint8_t>(shadowReject);
		// Skylighting's occlusion map, when DCLF draws it: whether and how this object draws into it.
		std::uint32_t skyTechnique = 0;
		if (SkyOcclusionEnabled())
			if (const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(shadowProperty))
				skyTechnique = Skylighting::OcclusionTechnique(lighting, geometry, true);
		tables.skyTechnique[objectId] = skyTechnique;
		if (skyTechnique) {
			if ((skyTechnique & 0x80) && !shadowMaterial) {
				if (const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(shadowProperty->material)) {
					shadowMaterial = material;
					auto* texture = material->diffuseTexture ? material->diffuseTexture->rendererTexture : nullptr;
					shadowDiffuse = texture ? texture->resourceView : nullptr;
					if (shadowDiffuse && tables.shadowTextureSeen.insert(shadowDiffuse).second)
						tables.shadowTextureSet.push_back(shadowDiffuse);
				}
			}
			const ShadowPipelineKey skyKey{ skyTechnique,
				shadowProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided) ? kRasterTwoSided : 0u,
				VertexLayoutOf(tables.geometries[geometrySlot].vertexDesc) };
			if (std::find(tables.skyKeysUsed.begin(), tables.skyKeysUsed.end(), skyKey) == tables.skyKeysUsed.end())
				tables.skyKeysUsed.push_back(skyKey);
		}
		tables.sunEntry[objectId] = SunEntryOf(*trackedEntry, *geometry);
		tables.faceStream[objectId] = face.positions ? static_cast<std::uint32_t>(tables.faceStreams.size()) : kNoFaceStream;
		if (face.positions)
			tables.faceStreams.push_back({ objectId, faceRegion, face.vertexCount, face.generation, face.positions });
		tables.shadowDiffuse[objectId] = shadowDiffuse;
		tables.shadowMaterial[objectId] = shadowMaterial;
		tables.sceneFlags[objectId] = object.flags;
		tables.objectGeometry[objectId] = geometry;
		const std::uint32_t keptPipeline = tables.draws[objectId].pipelineIndex;
		if (keepHalf) {
			// The scene bits the accumulate phase keeps are this write's; everything else is the patch's.
			const auto& kept = tables.objects[objectId];
			object.flags = (object.flags & kSceneKeptFlags) | (kept.flags & ~kSceneKeptFlags);
			object.materialIndex = kept.materialIndex;
			object.pipelineIndex = kept.pipelineIndex;
			tables.objects[objectId] = object;
		} else {
			// The extras rows are allocated by the accumulate phase, which is where the descriptors that
			// decide whether an object needs them are derived.
			tables.FreeExtras(objectId);
			tables.objects[objectId] = object;
			tables.shading[objectId] = ObjectShading{};
			tables.emissiveMult[objectId] = 1.0f;
			tables.lights[objectId] = ObjectLights{};
			tables.treeAnim[objectId] = ObjectTreeAnim{};
			tables.skinWetness[objectId] = {};
			tables.fadeDistance[objectId] = 0.0f;
		}
		if (trackedEntry->actorOwned)
			tables.actorObjects.push_back(objectId);
		{
			const auto& runtime = geometry->GetGeometryRuntimeData();
			const auto* shaderProperty = runtime.shaderProperty.get();
			const auto* alphaProperty = runtime.alphaProperty.get();
			tables.SetWatch(objectId, Tables::kWatchShading,
				(shaderProperty && shaderProperty->GetControllers()) || (alphaProperty && alphaProperty->GetControllers()));
		}
		tables.skinPartitions[objectId] = static_cast<std::uint8_t>(skinPartitions && skinPartitions->numPartitions > 1 ? partitionMask : 0);
		if (!denseWalk) {
			trackedEntry->objectStamp = objectStamp;
			trackedEntry->objectId = objectId;
		}

		const auto& geometryRecord = tables.geometries[geometrySlot];
		DrawSequence draw{};
		draw.pipelineIndex = keepHalf ? keptPipeline : 0;
		draw.vertexBufferAddress = geometryRecord.vertexAddress;
		draw.vertexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(geometryRecord.vertexBytes, UINT32_MAX));
		draw.vertexStride = geometryRecord.vertexStride;
		// The second stream repeats the first; the epochs replace it with a face shape's positions.
		draw.streamBufferAddress = draw.vertexBufferAddress;
		draw.streamBufferSize = draw.vertexBufferSize;
		draw.streamStride = draw.vertexStride;
		draw.indexBufferAddress = geometryRecord.indexAddress;
		draw.indexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(geometryRecord.indexBytes, UINT32_MAX));
		draw.indexFormat = kIndexFormatR16;
		draw.indexCount = geometryRecord.indexCount;
		draw.instanceCount = 1;
		draw.firstIndex = geometryRecord.firstIndex;
		draw.vertexOffset = 0;
		draw.firstInstance = 0;
		tables.draws[objectId] = draw;
		timer.Add(BuildPart::Record);
		return true;
	}

	void SceneStore::PrepareSceneJob()
	{
		// The two things the walk does that only the render thread may: keep the geometry slots' buffer
		// references alive (GpuResources::Touch, which also finds an eviction) and run the engine's palette
		// update. Both are done here for what last frame used - in steady state exactly what this frame uses -
		// and the walk counts anything else as a miss.
		if (frameResolveBuffers) {
			ScopedScan scan(Scan::SceneJobTouch);
			auto& gpu = GpuResources::Get();
			geometryTouched.resize(tables.geometries.size(), 0);
			for (std::size_t slot = 0; slot < tables.geometries.size() && slot < tables.geometryLastUsed.size(); ++slot) {
				if (tables.geometryLastUsed[slot] != frame - 1)
					continue;
				const auto& record = tables.geometries[slot];
				if (gpu.Touch(record.vertexBuffer) && gpu.Touch(record.indexBuffer))
					geometryTouched[slot] = frame;
				else
					++sceneAsync.touchFailures;
			}
		}
		if (SkinnedEnabled()) {
			for (auto* geometry : skinnedLastFrame) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end())
					continue;  // detached at Present
				if (auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get()) {
					UpdateSkin(skin, geometry->world);
					it->second.skinUpdatedFrame = frame;
				}
			}
		}
	}

	void SceneStore::JoinScenePhase()
	{
		if (!sceneJob)
			return;
		auto& worker = AsyncWorker::Get();
		const auto start = std::chrono::steady_clock::now();
		// Unbounded in effect: the frame cannot go on without its tables, and a walk cannot be restarted for
		// less than it costs to finish it.
		const auto joined = worker.Wait(sceneJob, std::chrono::seconds(10));
		if (joined == AsyncWorker::WaitResult::Late)
			worker.Cancel(sceneJob);
		sceneJob = {};
		const double waitedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		sceneAsync.waitMs += waitedMs;
		sceneAsync.waitMaxMs = std::max(sceneAsync.waitMaxMs, waitedMs);
		const WalkResult walked = sceneJobResult;
		const bool usable = joined == AsyncWorker::WaitResult::Done && !walked.geometryMisses && !walked.skinMisses;
		const bool probe = usable && AsyncModeSetting() == AsyncMode::Probe;
		// A job queued behind the walk (the shadow build) reads the tables: it finishes before they are
		// rewritten, and a rebuild makes it stale (GetSceneRebuilds).
		if (!usable || probe)
			worker.WaitIdle();
		if (!usable) {
			if (joined != AsyncWorker::WaitResult::Done)
				++sceneAsync.failed;
			sceneAsync.geometryMisses += walked.geometryMisses;
			sceneAsync.skinMisses += walked.skinMisses;
			++sceneAsync.rebuilt;
			++sceneRebuilds;
			SceneWalk(true);
		} else {
			++sceneAsync.used;
			if (probe) {
				// The worker's arrays against the same walk on the render thread, now. A difference is either a
				// walk that is not a function of its inputs or engine data that changed during the shadow maps.
				const Tables worker_ = tables;
				const std::size_t workerObjects = tables.objects.size();
				SceneWalk(true);
				++sceneAsync.probeCompared;
				std::string difference;
				if (!SameSceneTables(worker_, tables, difference) || workerObjects != tables.objects.size()) {
					if (sceneAsync.probeDiffer++ == 0)
						logger::warn("[DCLF] async scene probe: the worker's walk differs from the render thread's: {}", difference.empty() ? "the object index" : difference);
				}
			}
		}
		skinnedLastFrame = skinnedObjects;
		stats.objects = tables.liveObjects;
		sceneBuilt = true;
	}

	void SceneStore::AbandonSceneJob()
	{
		if (!sceneJob)
			return;
		auto& worker = AsyncWorker::Get();
		worker.Cancel(sceneJob);
		sceneJob = {};
		worker.WaitIdle();
		// The walk's partial output is nobody's frame. It may have taken slots it never swept, so every slot goes
		// and the next walk lays the objects out again.
		tables.ClearFrame(false);
		for (auto& [geometry, entry] : tracked)
			entry.slot = kNoObjectSlot;
		InvalidateObjectIndices();
		sceneBuilt = false;
		fullEvaluation = true;
	}

	std::string SceneStore::SceneAsyncReport()
	{
		std::string text;
		// The change log (Tables::changeLog): notes per frame by cause, and its completeness check.
		if (const double n = delta.walks) {
			std::uint64_t total = 0;
			std::string causes;
			for (std::uint32_t i = 0; i < kChangeCauseCount; ++i) {
				const auto notes = tables.changeCounts[i] - reportedChangeCounts[i];
				total += notes;
				if (notes)
					causes += fmt::format("{}{} {:.1f}", causes.empty() ? "" : ", ", kChangeCauseNames[i], notes / n);
			}
			reportedChangeCounts = tables.changeCounts;
			text += fmt::format("[DCLF] change log: {:.1f} changes a frame, by column: {} ({} entries held)\n", static_cast<double>(total) / n,
				causes.empty() ? "-" : causes, tables.changeLog.Size());
		}
		if (const double n = delta.walks) {
			auto& t = ScanTimes::Get();
			std::string scans;
			for (std::size_t i = 0; i < t.ns.size(); ++i) {
				const auto ns = t.ns[i].exchange(0);
				const auto calls = t.calls[i].exchange(0);
				if (calls)
					scans += fmt::format("{}{} {:.1f} us ({:.1f} calls)", scans.empty() ? "" : "; ", kScanNames[i], ns / 1000.0 / n, calls / n);
			}
			text += fmt::format("[DCLF][TEMP] scans a frame: {}\n", scans);
		}
		if (auto& m = materialFrameStats; m.frames) {
			const double n = static_cast<double>(m.frames);
			const auto differ = m.componentsDiffer + m.transformsDiffer;
			text += fmt::format("[DCLF] material frame components: {:.1f} samples, {:.2f} applications to {:.1f} slots, {:.1f} pending slots and {:.1f} watched transforms a frame; parity {} checks, {} slots, {} frame components and {} transforms differ{}{}\n",
				m.samples / n, m.applications / n, m.slotsApplied / n, m.pending / n, m.transformsWatched / n, m.checks, m.slotsChecked, m.componentsDiffer, m.transformsDiffer,
				m.checks ? (differ ? " <- DIFFER; first: " : " <- OK") : "", differ ? m.first : std::string());
			m = {};
		}
		if (auto& g = geometryStats; g.frames) {
			std::string differ;
			std::uint64_t total = 0;
			for (std::uint32_t stage = 0; stage < 2; ++stage)
				for (std::uint32_t v = 0; v < 64; ++v)
					if (g.differ[stage][v]) {
						total += g.differ[stage][v];
						differ += fmt::format(" {}{}={}", stage ? "PS" : "VS", v, g.differ[stage][v]);
					}
			text += fmt::format("[DCLF] pipeline constants: {:.2f} full geometry evaluations and {:.2f} frame samples a frame, {:.1f} pipelines changed, frame lighting changed {:.2f}; parity {} checks, {} pipelines, {} geometry variables, {} of {} technique blocks and {} of {} frame lightings differ{}{}{}\n",
				static_cast<double>(g.full) / g.frames, static_cast<double>(g.samples) / g.frames, static_cast<double>(g.changed) / g.frames,
				static_cast<double>(g.lightingVersions) / g.frames, g.checks, g.pipelinesChecked, total, g.techniquesDiffer, g.techniquesChecked, g.lightingDiffer, g.lightingChecked,
				g.checks ? (total || g.techniquesDiffer || g.lightingDiffer ? " <- DIFFER:" : " <- OK") : "", differ,
				(total ? "; first: " + g.first : std::string()) + (g.lightingDiffer ? "; lighting: " + g.lightingFirst : std::string()));
			g = {};
		}
		if (auto& sp = shadingParity; sp.frames) {
			text += fmt::format("[DCLF] shading resample: {:.1f} slots watched, {:.1f} LOD fade events, {:.1f} resampled a frame; parity {} checks, {} slots compared, {} changed unsampled{}{}\n",
				static_cast<double>(sp.watched) / sp.frames, static_cast<double>(sp.lodFadeEvents) / sp.frames, static_cast<double>(sp.resampled) / sp.frames, sp.checks, sp.slots, sp.missing,
				sp.checks ? (sp.missing ? " <- MISSED; first: " : " <- OK") : "", sp.first);
			sp = {};
		}
		if (auto& c = changeParity; c.checks || c.skipped) {
			text += fmt::format("[DCLF] change log parity: {} checks ({} skipped), {} slots compared, {} changed, {} changed with no log entry{}{}\n", c.checks, c.skipped, c.slots,
				c.changed, c.missing, c.missing ? " <- MISSING; first: " : " <- OK", c.first);
			c = { std::move(c.snapshot), c.cursor };
		}
		if (auto& t = delta; t.walks) {
			const double n = t.walks;
			text += fmt::format("[DCLF] scene delta: {} walks ({} full), evaluated {:.0f}/frame (max {}; per-frame {:.0f}, of them {:.0f} kept by the light path and {:.0f} moved by it; pending {:.0f}, fade {:.1f}, property {:.1f}, node {:.1f}, sun entry node {:.1f}, geometry {:.1f}), settling {:.1f}, lapsed {:.0f}, {:.0f} live slots; events per frame: {:.1f} property, {:.1f} node; {:.2f} inputs re-read changed; switch events {:.2f}/frame: {:.2f} changed, {:.2f} caught up ({:.2f} at attach), {:.1f} entries classified again\n",
				t.walks, t.full, t.evaluated / n, t.evaluatedMax, t.perFrame / n, t.kept / n, t.moved / n, t.pending / n, t.fade / n, t.property / n, t.node / n, t.roots / n,
				t.geometryDirty / n, t.settling / n, t.restored / n, t.live / n, t.propertyEvents / n, t.nodeEvents / n, t.reread / n,
				t.switchEvents / n, t.switchChanges / n, t.switchCatchUps / n, t.attachCatchUps / n, t.switchReclassified / n);
			if (rootMotion.size() > (1u << 16))
				rootMotion.clear();
			t = {};
		}
		auto& a = sceneAsync;
		if (!a.kicked)
			return text;
		text += fmt::format("[DCLF] async scene: {} walks kicked, {} used, {} rebuilt inline ({} geometry misses, {} skin misses, {} failed), {} slot touches failed ahead; join waited {:.3f}/{:.3f} ms; probe: {} compared, {} differ\n",
			a.kicked, a.used, a.rebuilt, a.geometryMisses, a.skinMisses, a.failed, a.touchFailures, a.waitMs / a.kicked, a.waitMaxMs, a.probeCompared, a.probeDiffer);
		a = {};
		return text;
	}

	/**
	 * @brief The geometry slot for a TriShape: found, refreshed in place, or newly resolved.
	 *
	 * @return the slot, or Tables::kSlotFree when the buffers cannot be made stable for the render graph.
	 */
	std::uint32_t SceneStore::ResolveGeometrySlot(RE::BSGeometry& a_geometry, const RE::BSGraphics::TriShape* a_triShape,
		const RE::NiSkinPartition::Partition* a_skinPartition, PartTimer& a_timer, bool a_renderThread, bool& a_miss)
	{
		if (!a_triShape)
			return Tables::kSlotFree;
		auto& gpu = GpuResources::Get();
		const bool resolveBuffers = frameResolveBuffers;
		auto geometryIt = geometryIndex.find(a_triShape);
		const bool newGeometry = geometryIt == geometryIndex.end();
		// First use of the slot this frame needs a Touch, unless PrepareSceneJob already touched it.
		const bool touchedAhead = !newGeometry && geometryIt->second < geometryTouched.size() && geometryTouched[geometryIt->second] == frame;
		const bool needsTouch = !newGeometry && resolveBuffers && tables.geometryLastUsed[geometryIt->second] != frame && !touchedAhead;
		if (!a_renderThread) {
			// The worker may neither resolve nor touch: anything that would need either is a miss.
			const auto& known = newGeometry ? GeometryRecord{} : tables.geometries[geometryIt->second];
			const bool changed = !newGeometry && ((resolveBuffers && known.vertexAddress == 0) ||
													 known.vertexBuffer != reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer) ||
													 known.indexBuffer != reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer));
			if (newGeometry || changed || needsTouch) {
				a_miss = true;
				return Tables::kSlotFree;
			}
		}
		// A slot found by address but describing other buffers is a TriShape reallocated at the same
		// address: it is resolved again into the same slot. A slot whose buffer references were
		// evicted (nothing touched them for kEvictFrames) is resolved again the same way.
		const bool staleGeometry = !newGeometry &&
		                           ((resolveBuffers && tables.geometries[geometryIt->second].vertexAddress == 0) ||
		                               tables.geometries[geometryIt->second].vertexBuffer != reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer) ||
		                               tables.geometries[geometryIt->second].indexBuffer != reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer) ||
		                               (needsTouch &&
		                                   (!gpu.Touch(tables.geometries[geometryIt->second].vertexBuffer) || !gpu.Touch(tables.geometries[geometryIt->second].indexBuffer))));
		if (staleGeometry)
			++stats.geometriesRefreshed;
		if (newGeometry || staleGeometry) {
			// The buffers are resolved once per TRISHAPE, not once per object.
			//
			// Skipping the call altogether is NOT safe and is why this is a move rather than a cache:
			// GpuResources holds a reference on each buffer so its address cannot be reused, and it
			// drops that reference when an entry goes kEvictFrames without a Resolve. Resolving per
			// TriShape still touches every entry DCLF depends on every frame, so nothing is evicted
			// out from under the tables.
			std::optional<GpuResources::Buffer> vertexBuffer;
			std::optional<GpuResources::Buffer> indexBuffer;
			if (resolveBuffers) {
				// The render graph reads the game's buffers in place; they must never move (GpuResources).
				vertexBuffer = gpu.Resolve(reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer));
				indexBuffer = gpu.Resolve(reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer));
				if (!vertexBuffer || !indexBuffer) {
					// Nothing was inserted, so the next object sharing this TriShape retries, exactly
					// as it did when every object resolved for itself. A stale slot is freed: its
					// record no longer describes anything.
					if (staleGeometry) {
						const std::uint32_t slot = geometryIt->second;
						FreeGeometrySlot(slot);
						freedGeometry.push_back(slot);
						slotsFreedThisFrame = true;
					}
					a_timer.Add(BuildPart::Resolve);
					return Tables::kSlotFree;
				}
			}
			const auto& shape = static_cast<RE::BSTriShape&>(a_geometry).GetTrishapeRuntimeData();
			GeometryRecord record;
			record.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer);
			record.indexBuffer = reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer);
			record.vertexDesc = std::bit_cast<std::uint64_t>(a_triShape->vertexDesc);
			// The stride the engine binds is the desc's low nibble in dwords (engine notes: vertex input).
			record.vertexStride = static_cast<std::uint32_t>(record.vertexDesc & 0xFu) * 4u;
			record.vertexCount = a_skinPartition ? a_skinPartition->vertices : shape.vertexCount;
			record.indexCount = static_cast<std::uint32_t>(a_skinPartition ? a_skinPartition->triangles : shape.triangleCount) * 3;
			record.firstIndex = 0;
			if (vertexBuffer && indexBuffer) {
				record.vertexAddress = vertexBuffer->address;
				record.vertexBytes = vertexBuffer->size;
				record.indexAddress = indexBuffer->address;
				record.indexBytes = indexBuffer->size;
			}
			const std::uint32_t slot = staleGeometry ? geometryIt->second : AllocateGeometrySlot();
			// Other objects' draws copied the old record: the delta walk re-evaluates the ones it kept.
			if (staleGeometry)
				refreshedGeometry.push_back(slot);
			tables.geometries[slot] = record;
			tables.NoteGeometry(slot);
			tables.geometrySlotKey[slot] = a_triShape;
			if (!staleGeometry)
				geometryIt = geometryIndex.emplace(a_triShape, slot).first;
			a_timer.Add(BuildPart::Resolve);
		} else if (resolveBuffers && tables.geometryLastUsed[geometryIt->second] != frame) {
			// First use of the slot this frame: the Touch above already kept the references alive.
			a_timer.Add(BuildPart::DedupHit);
		}
		tables.geometryLastUsed[geometryIt->second] = frame;
		return geometryIt->second;
	}

	namespace
	{
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

	/**
	 * @brief The accumulator half of the frame, at EarlyPrepass.
	 *
	 * The main camera's passes are complete only once Main_RenderShadowMaps returns, so everything that
	 * depends on them is here: the capture drain, the pipeline and material slots, the per-frame lighting
	 * template, the shading and light lists, the decal order and kObjectNativeVisible. It patches the
	 * records BuildScenePhase appended, in place and by object index.
	 */
	void SceneStore::BuildAccumulatePhase()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Tables");
		if (!sceneBuilt) {
			// A load screen, or the feature installed mid-frame: nothing to patch.
			CompareCapturedPasses(false);
			return;
		}
		PartTimer timer(stats.partMs);
		std::uint32_t fadingThisFrame = 0;
		TracyCZoneN(captureZone, "CS.DCLF.Accumulate.Capture", true);
		// The pass table is filled from the capture, which is the source that keeps working once passes
		// are withheld from the batch renderer. The accumulator walk is the cross-check.
		static const bool passParity = SwitchEnabled("CS_DCLF_PASS_PARITY");
		static const bool fromAccumulator = SwitchValue("CS_DCLF_PASS_SOURCE") == "accumulator";
		const bool haveAccumulator = RefreshMainBatchRenderers();
		if (fromAccumulator || passParity)
			CollectAccumulatedPasses();
		else
			accumulatedPasses.clear();
		CompareCapturedPasses(passParity || fromAccumulator);
		// The capture had nothing and the walk was skipped: take the walk after all, so the first frame
		// after a latch is not empty.
		if (accumulatedPasses.empty() && haveAccumulator && !fromAccumulator && !passParity)
			CollectAccumulatedPasses();
		// The objects the primary's cull left out this frame (PrimaryCull): their main passes, built without a
		// registration, stand where the engine's would have.
		for (const auto& [geometry, pass] : PrimaryCull::Get().BuildSyntheticPasses())
			AddAccumulatedPass(geometry, pass);
		// Resident records (PrimaryCull): a frame the primary's cut did not keep them, every one ends; the entries joining
		// this frame have their passes patched once below.
		const bool residentsLive = PrimaryCull::Get().TakeResidentsLive();
		if (!residentsLive && (!residents.empty() || PrimaryCull::Get().HasResidents()))
			PrimaryCull::Get().EndAllResidents();
		residentJoining.clear();
		if (residentsLive)
			for (const auto& [geometry, pass] : PrimaryCull::Get().ResidentPasses()) {
				residentJoining.insert(geometry);
				AddAccumulatedPass(geometry, pass);
			}
		// The engine's passes for what PrimaryCull draws synthetically take the same sun bits, so both sources of an
		// object's pass need one pipeline (and the probe still compares against the engine's own bits).
		if (PrimaryCull::SunOnGpu() && !PrimaryCull::Probe())
			for (auto& [geometry, pass] : accumulatedPasses)
				if (pass.pass)
					PrimaryCull::Get().UnifySunBits(geometry, pass);
		timer.Add(BuildPart::Walk);

		TracyCZoneEnd(captureZone);
		// [TEMP] CS_DCLF_VOLUMETRIC_PROBE: the face parts' main-camera passes - the technique their flags select, the
		// decal and alpha flags, and the pass the table holds (technique, hint, list), or none - every 600 frames.
		if (VolumetricProbe::Enabled() && frame % 600 == 0) {
			std::map<std::string, std::uint32_t> seen;
			for (const auto& stream : tables.faceStreams) {
				auto* geometry = stream.object < tables.objectGeometry.size() ? tables.objectGeometry[stream.object] : nullptr;
				if (!geometry)
					continue;
				const auto& runtime = geometry->GetGeometryRuntimeData();
				const auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.shaderProperty.get());
				const std::uint64_t flags = property ? property->flags.underlying() : 0;
				const auto* alpha = runtime.alphaProperty.get();
				const auto* pass = FindAccumulatedPass(geometry);
				++seen[fmt::format("'{}' flags technique {} decal {}{} alpha test {} blend {} | pass {}", geometry->name.c_str() ? geometry->name.c_str() : "?",
					SelectLightingTechnique(flags), (flags >> 26) & 1, (flags >> 27) & 1, alpha && alpha->GetAlphaTesting(), alpha && alpha->GetAlphaBlending(),
					pass ? fmt::format("technique {} hint {} list {}", (pass->technique >> 24) & 0x3f, pass->hint, pass->subPass) : std::string("none"))];
			}
			for (const auto& [line, count] : seen)
				logger::info("[DCLF][TEMP] face part {} x{}", line, count);
		}

		auto& evaluator = ConstantEvaluator::Get();
		auto& gpu = GpuResources::Get();
		const bool resolveBuffers = frameResolveBuffers;
		const bool interior = frameInterior;
		const auto& decalBiasMode = frameDecalBias;
		const std::uint32_t biasWitness = decalBiasMode[1] | (decalBiasMode[2] << 8);
		const bool lightLimitFixLoaded = globals::features::lightLimitFix.loaded;
		// CS_DCLF_DERIVED_CACHE=off|on|probe: `probe` serves the cache and recomputes, comparing the two.
		static const std::string derivedCacheMode = SwitchValue("CS_DCLF_DERIVED_CACHE");
		static const bool derivedCache = derivedCacheMode != "off";
		static const bool derivedProbe = derivedCacheMode == "probe";
		// An object the engine did not accumulate cannot be drawn while the draws are gated on the
		// engine's visibility (CS_DCLF_CULL_INPUT=native, the default): it is a culling candidate and
		// nothing more, and its scene record already carries everything the culling reads.
		const bool drawCulledCandidates = Toggles::Get().Active().cullTracked;
		// CS_DCLF_TABLES=accumulated: bindings only for what the engine's accumulator kept, which is the
		// default in any case; it remains as the switch that also keeps culled candidates out of the
		// culling's input.
		static const bool accumulatedOnly = SwitchValue("CS_DCLF_TABLES") == "accumulated";
		const bool materialCacheOn = MaterialCacheEnabled();
		static const bool materialProbeAll = SwitchValue("CS_DCLF_MATERIAL_CACHE") == "probe";
		static const bool derivationStats = SwitchEnabled("CS_DCLF_DERIVE_PROBE");
		// CS_DCLF_PRIMARY_EXCLUDE=probe: what the objects under the primary's candidate entries take from their
		// registration, against what DCLF derives (PrimaryCull::NoteDerived).
		const bool primaryProbe = PrimaryCull::Probe() && PrimaryCull::Get().Installed();
		static const bool classifyProbe = SwitchValue("CS_DCLF_CLASSIFY_CACHE") == "probe";

		// What this phase has anything to do with. Off the =tracked culling input that is the engine's
		// accumulated passes alone - ~1,700 of the exterior's 10,000 tracked objects - so the phase does
		// not walk the tracked set a second time merely to find them; the scene phase has already given
		// every other object everything it gets this frame.
		accumulateOrder.clear();
		TracyCZoneN(orderZone, "CS.DCLF.Accumulate.Order", true);
		if (drawCulledCandidates && !accumulatedOnly) {
			// The delta walk's `order` holds only what it evaluated.
			if (SceneDeltaEnabled())
				BuildFullOrder();
			accumulateOrder.reserve(order.size());
			for (const auto& entry : order)
				accumulateOrder.push_back({ entry.geometry, entry.tracked, FindAccumulatedPass(entry.geometry) });
		} else {
			accumulateOrder.reserve(accumulatedPasses.size());
			for (auto& [passGeometry, pass] : accumulatedPasses) {
				auto* mutableGeometry = const_cast<RE::BSGeometry*>(passGeometry);
				auto trackedIt = tracked.find(mutableGeometry);
				if (trackedIt != tracked.end())
					accumulateOrder.push_back({ mutableGeometry, &trackedIt->second, &pass });
			}
		}
		timer.Add(BuildPart::PassLookup);
		TracyCZoneEnd(orderZone);
		TracyCZoneN(objectsZone, "CS.DCLF.Accumulate.Objects", true);
		for (auto& [geometry, trackedEntry, accumulated] : accumulateOrder) {
			timer.Add(BuildPart::LoopTail);
			if (!accumulated && !(drawCulledCandidates && !accumulatedOnly))
				continue;
			if (trackedEntry->objectStamp != objectStamp) {
				// No record: the scene phase found it ineligible, which is where the histogram's "drawn"
				// column comes from. A verdict of None here means the record itself failed (an unstable
				// buffer) or the cached verdict was stale, so that one is cleared and reported.
				if (accumulated) {
					++stats.ineligibleDrawn[static_cast<std::size_t>(trackedEntry->candidateReason)];
					if (trackedEntry->candidateReason == Ineligible::None) {
						++stats.accumulatedWithoutRecord;
						trackedEntry->candidateFrame = 0;
						pendingEvaluation.push_back(geometry);
					} else if (SceneDeltaEnabled() && (trackedEntry->candidateReason == Ineligible::Hidden || trackedEntry->candidateReason == Ineligible::Switch)) {
						// The engine drew what the kept verdict calls hidden or unselected: shown since. A static's hidden
						// bit has no event of its own, and this is the engine's cull saying so.
						trackedEntry->candidateFrame = 0;
						pendingEvaluation.push_back(geometry);
					}
				}
				continue;
			}
			const std::uint32_t objectId = trackedEntry->objectId;
			// Resident: patched once, kept; a pass the engine registered for it anyway (its root was entry 0 of a list,
			// which Process2 culls) is withheld by static ownership and changes nothing here.
			if (IsResidentSlot(objectId)) {
				++residentStats.registered;
				continue;
			}
			auto& object = tables.objects[objectId];
			const std::uint32_t geometrySlot = object.geometryIndex;
			// A shadow-only record: the main pass cannot take it, which the scene phase has already decided.
			if (object.flags & kObjectShadowOnly) {
				if (accumulated)
					++stats.ineligibleDrawn[static_cast<std::size_t>(trackedEntry->candidateReason)];
				continue;
			}

			auto& data = geometry->GetGeometryRuntimeData();
			auto* property = data.shaderProperty.get();
			auto* witnessProperty = property;
			const auto* witnessMaterial = witnessProperty ? witnessProperty->material : nullptr;
			const std::uint8_t fadeState = FadeStateOf(witnessProperty);
			RE::BSLightingShaderProperty* castCache = trackedEntry->castProperty == witnessProperty ? trackedEntry->castResult : nullptr;
			const bool alphaBelowOne = witnessMaterial && static_cast<const RE::BSLightingShaderMaterialBase*>(witnessMaterial)->materialAlpha < 1.0f;

			// The positive derivation, cached (Tracked::Derived): for an accumulated object whose
			// witnesses all match and whose slots still carry the keys they were derived for, the
			// classification and the whole derived section are skipped.
			auto& derived = trackedEntry->derived;
			bool derivedHit = derivedCache && accumulated && derived.valid && derived.generation == tablesGeneration &&
			                  derived.geometrySlot == geometrySlot && derived.property == witnessProperty &&
			                  derived.material == witnessMaterial && derived.fadeState == fadeState && derived.technique == accumulated->technique &&
			                  derived.subPass == accumulated->subPass && derived.hint == accumulated->hint && derived.interior == interior &&
			                  derived.alphaBelowOne == alphaBelowOne && derived.biasWitness == biasWitness;
			if (derivedHit) {
				derivedHit = derived.pipelineSlot < tables.pipelines.size() && tables.pipelineSlots.Alive(derived.pipelineSlot) &&
				             tables.pipelines[derived.pipelineSlot] == derived.key &&
				             derived.materialSlot < tables.materialSlotKey.size() && tables.materialSlots.Alive(derived.materialSlot) &&
				             tables.materialSlotKey[derived.materialSlot] == std::pair{ derived.material, derived.descriptors.pass };
			}

			LightingDescriptors descriptors;
			Ineligible reason = Ineligible::None;
			if (derivedHit && !derivedProbe) {
				descriptors = derived.descriptors;
				// The LOD fades are this frame's GetRenderPasses' (the property's fields, as ClassifyStatic reads them for an
				// accumulated object), not the cached derivation's.
				const auto& lightingProperty = *static_cast<const RE::BSLightingShaderProperty*>(property);
				descriptors.specularLODFade = lightingProperty.specularLODFade;
				descriptors.envmapLODFade = lightingProperty.envmapLODFade;
				++stats.derivedHits;
			} else {
				reason = ClassifyStatic(*geometry, &descriptors, accumulated, derivationStats || primaryProbe, &castCache);
			}
			timer.Add(BuildPart::ClassifyStatic);
			const bool primaryCandidate = primaryProbe && accumulated && PrimaryCull::Get().UnderListedCandidate(geometry);
			// Per frame whether or not the derivation was cached: hidden, part of an actor and fading are
			// states of this frame, and the scene phase's verdict for them is the last classification's
			// (for a static, the last event's). An object that has just been hidden must lose its bindings now,
			// or DCLF keeps drawing what the engine has stopped drawing.
			if (reason == Ineligible::None)
				reason = ClassifyFrame(*trackedEntry, accumulated);
			// The skin partitions the main camera draws, from its registered pass's LODMode rather than the fade
			// node the scene phase read for the shadow views. None is not drawn at all.
			if (reason == Ineligible::None && accumulated && data.skinInstance && data.skinInstance->skinPartition) {
				const std::uint32_t mask = SkinPartitionMask(*data.skinInstance, accumulated->lodRow);
				if (!mask)
					reason = Ineligible::Hidden;
				else if (objectId < tables.skinPartitions.size()) {
					// The walk writes the scene's mask again (every skin is written every walk); a resident's is the kept skin's,
					// from the same LOD row.
					const auto partitionMask = static_cast<std::uint8_t>(data.skinInstance->skinPartition->numPartitions > 1 ? mask : 0);
					if (tables.skinPartitions[objectId] != partitionMask) {
						tables.skinPartitions[objectId] = partitionMask;
						tables.NoteChange(objectId, kChangeSkin);
					}
				}
			}
			// A pass in an alpha-test list is drawn with DoAlphaTest whatever it was registered with
			// (DrawnPassDescriptor, applied where the passes are taken).
			// The histogram is the scene phase's, taken over the whole tracked set; where this phase -
			// which has the accumulated pass, and so the decal group - reaches a different verdict, the
			// object is moved between the buckets so the report reads as it did before the split.
			if (primaryCandidate)
				PrimaryCull::Get().NoteDerived(*geometry, descriptors, *accumulated, reason, LodRowOf(*geometry, property));
			if (reason != trackedEntry->candidateReason) {
				--stats.ineligible[static_cast<std::size_t>(trackedEntry->candidateReason)];
				++stats.ineligible[static_cast<std::size_t>(reason)];
			}
			if (reason != Ineligible::None) {
				trackedEntry->accumulateReason = reason;
				trackedEntry->accumulateReasonFrame = frame;
				// Eligible for a record but not for bindings: it stays native this frame, which is what
				// its scene record already says (kObjectNoBindings, not native-visible).
				if (accumulated)
					++stats.ineligibleDrawn[static_cast<std::size_t>(reason)];
				derived.valid = false;
				continue;
			}
			timer.Add(BuildPart::ClassifyFrame);

			// Only computed when something will report them: this whole block exists to feed one log line.
			if (derivationStats && accumulated && descriptors.derivedPass != kNotDerived) {
				++stats.derivationChecked;
				const std::uint32_t differing = descriptors.derivedPass ^ descriptors.pass;
				if (const std::uint32_t bits = differing & ~kRuntimePassBits) {
					++stats.derivationDiffers;
					stats.derivationBits |= bits;
				}
				if (const std::uint32_t bits = differing & kRuntimePassBits) {
					++stats.derivationRuntimeDiffers;
					stats.derivationRuntimeBits |= bits;
				}
				for (std::uint32_t remaining = differing; remaining;) {
					const std::uint32_t bit = std::countr_zero(remaining);
					++stats.derivationBitCounts[bit];
					remaining &= remaining - 1;
				}
			} else if (derivationStats && descriptors.derivedPass == kNotDerived) {
				++stats.derivationNative;
			}
			timer.Add(BuildPart::Diagnostics);

			std::uint32_t pipelineSlot = Tables::kSlotFree, materialSlot = Tables::kSlotFree, staticFlags = 0;
			PipelineKey key{};
			if (derivedHit && !derivedProbe) {
				pipelineSlot = derived.pipelineSlot;
				materialSlot = derived.materialSlot;
				staticFlags = derived.staticFlags;
				key = derived.key;
				tables.materialLastUsed[materialSlot] = frame;
				// The per-frame template: a pipeline's template is always a property of an object of this
				// frame (the first to use the slot, upgraded to a native-visible one by the election), so a
				// persistent slot never points at a property the game has since freed. The constants are
				// evaluated from it at Prepass (RefreshFrameConstants).
				if (tables.pipelineLastUsed[pipelineSlot] != frame) {
					tables.pipelineLastUsed[pipelineSlot] = frame;
					tables.geometryTemplate[pipelineSlot] = property;
					tables.geometryTemplateNative[pipelineSlot] = accumulated ? 1 : 0;
				} else if (accumulated && !tables.geometryTemplateNative[pipelineSlot]) {
					tables.geometryTemplate[pipelineSlot] = property;
					tables.geometryTemplateNative[pipelineSlot] = 1;
					++stats.templateUpgrades;
				}
				timer.Add(BuildPart::DedupHit);
			} else {
				const bool twoSided = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
				const auto* alpha = data.alphaProperty.get();
				const bool alphaTest = alpha && alpha->GetAlphaTesting();

				// A decal's key carries the engine's fixed-function state indices as well (Records.h): the
				// depth-bias mode from the frame, blend and write modes from the alpha property (derived with
				// the descriptors). Zero for everything else, so an opaque key is exactly what it was.
				std::uint32_t rasterFlags = twoSided ? kRasterTwoSided : 0u;
				if (descriptors.decalGroup)
					rasterFlags |= PackDecalRasterFlags(descriptors.decalGroup, decalBiasMode[descriptors.decalGroup & 3], descriptors.decalBlendMode, descriptors.decalWriteMode);
				if (globals::features::extendedTranslucency.loaded)
					rasterFlags |= ((ExtendedTranslucency::MaterialModel::DescriptorDisabled ^ ExtendedTranslucency::MaterialModelOf(geometry)) & 7u) << kRasterTranslucencyShift;
				key = PipelineKey{ descriptors.vertex, descriptors.pixel, rasterFlags, descriptors.pass,
					VertexLayoutOf(tables.geometries[geometrySlot].vertexDesc) };
				auto pipelineIt = pipelineIndex.find(key);
				const bool newPipeline = pipelineIt == pipelineIndex.end();
				if (!newPipeline && tables.pipelineLastUsed[pipelineIt->second] != frame) {
					// The slot's first use this frame: this object's property is the template until the
					// election finds a native-visible one (see the cached path).
					tables.pipelineLastUsed[pipelineIt->second] = frame;
					tables.geometryTemplate[pipelineIt->second] = property;
					tables.geometryTemplateNative[pipelineIt->second] = accumulated ? 1 : 0;
				}
				if (newPipeline) {
					timer.Add(BuildPart::Dedup);
					const std::uint32_t slot = AllocatePipelineSlot();
					tables.pipelines[slot] = key;
					// Per-frame PerGeometry values for this pass descriptor, from any object's lighting pass
					// (it supplies the scene light list the engine reads the sun from).
					GeometryConstants constants{};
					const auto* templatePass = FindLightingPass(property);
					const bool valid = templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, mainPassRenderFlags, constants);
					tables.geometryConstants[slot] = constants;
					tables.geometryConstantsValid[slot] = valid ? 1 : 0;
					tables.geometryTemplate[slot] = property;
					tables.geometryTemplateNative[slot] = accumulated ? 1 : 0;

					tables.pipelineTechnique[slot] = TechniqueRowFor(descriptors.pass);
					stats.shadowMaskPipelines += tables.TechniqueOf(slot).shadowMask ? 1 : 0;

					PipelinePermutation permutation;
					permutation.vertexShaderDescriptor = descriptors.rawVertex;
					permutation.pixelShaderDescriptor = descriptors.rawPixel & ~descriptors.pixel;
					permutation.extraShaderDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::InWorld);
					// Extended Translucency's material model, as its SetupGeometry hook sets it (the key carries it):
					// disabled for opaque geometry, the default or the mesh's own for blended geometry.
					permutation.extraFeatureDescriptor = globals::features::extendedTranslucency.loaded ?
					                                         (ExtendedTranslucency::MaterialModel::DescriptorDisabled ^ RasterTranslucency(key.rasterFlags))
					                                             << ExtendedTranslucency::ExtraFeatureDescriptorShift :
					                                         0u;
					tables.permutations[slot] = permutation;
					tables.pipelineConstantsVersion[slot] = tables.NextVersion();
					tables.pipelineBindingVersion[slot] = tables.NextVersion();
					pipelineIt = pipelineIndex.emplace(key, slot).first;
					timer.Add(BuildPart::PipelineEval);
				} else if (accumulated && !tables.geometryTemplateNative[pipelineIt->second]) {
					// The election. This pipeline's per-frame lighting template belongs to an object the
					// engine culled, and here is one it kept: take the template over. An object the engine
					// kept is by definition in the lighting situation being drawn, so it is the correct
					// template, and this is the ordering guarantee stated as a rule about the objects rather
					// than as a rule about the order they are visited in.
					const auto slot = pipelineIt->second;
					GeometryConstants constants;
					const auto* templatePass = FindLightingPass(property);
					if (templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, mainPassRenderFlags, constants)) {
						tables.geometryConstants[slot] = constants;
						tables.geometryConstantsValid[slot] = 1;
						tables.pipelineConstantsVersion[slot] = tables.NextVersion();
					}
					tables.geometryTemplate[slot] = property;
					tables.geometryTemplateNative[slot] = 1;
					++stats.templateUpgrades;
					timer.Add(BuildPart::PipelineEval);
				}
				pipelineSlot = pipelineIt->second;
				tables.pipelineLastUsed[pipelineSlot] = frame;

				// Material state as the engine's SetupMaterial produces it for this pass descriptor.
				const auto* material = property->material;
				auto materialIt = materialIndex.find(std::pair{ material, descriptors.pass });
				if (materialIt == materialIndex.end()) {
					timer.Add(BuildPart::Dedup);
					MaterialRecord record;
					if (!EvaluateMaterialForSlot(material, descriptors.pass, materialCacheOn, materialProbeAll, record)) {
						// No shader instance yet (nothing drawn so far): stay native this frame.
						derived.valid = false;
						continue;
					}
					const std::uint32_t slot = AllocateMaterialSlot();
					tables.materials[slot] = record;
					tables.materialVersion[slot] = ++materialVersions;
					tables.materialSlotKey[slot] = std::pair{ material, descriptors.pass };
					tables.ListMaterialSlot(slot, frame);
					materialIt = materialIndex.emplace(std::pair{ material, descriptors.pass }, slot).first;
					timer.Add(BuildPart::MaterialEval);
				}
				materialSlot = materialIt->second;
				tables.materialLastUsed[materialSlot] = frame;
				timer.Add(BuildPart::DedupHit);

				staticFlags = (alphaTest ? kObjectAlphaTest : 0u) | (twoSided ? kObjectTwoSided : 0u) |
				              (ExternalEmittance::ShouldSuppress(interior, property, geometry) ? kObjectSuppressExternalEmittance : 0u) |
				              (descriptors.technique == kTechniqueTreeAnim ? kObjectTreeAnim : 0u) |
				              (alphaTest ? static_cast<std::uint32_t>(alpha->alphaThreshold) << kObjectAlphaThresholdShift : 0u) |
				              (descriptors.decalGroup ? kObjectDecal | (descriptors.decalGroup << kObjectDecalGroupShift) : 0u);
				if (derivedCache && accumulated) {
					if (derivedHit && derivedProbe) {
						++stats.derivedChecked;
						const bool same = derived.pipelineSlot == pipelineSlot && derived.materialSlot == materialSlot &&
						                  derived.staticFlags == staticFlags && derived.key == key && derived.descriptors.vertex == descriptors.vertex &&
						                  derived.descriptors.pixel == descriptors.pixel && derived.descriptors.pass == descriptors.pass;
						if (!same && stats.derivedDiffers++ == 0)
							logger::warn("[DCLF] derived cache: '{}' differs on recompute (slots {}/{} vs {}/{}, flags {:X} vs {:X})",
								geometry->name.c_str() ? geometry->name.c_str() : "?", derived.pipelineSlot, derived.materialSlot,
								pipelineSlot, materialSlot, derived.staticFlags, staticFlags);
					}
					derived.valid = true;
					derived.generation = tablesGeneration;
					derived.triShape = tables.geometrySlotKey[geometrySlot];
					derived.property = witnessProperty;
					derived.material = witnessMaterial;
					derived.fadeState = fadeState;
					derived.interior = interior;
					derived.alphaBelowOne = alphaBelowOne;
					derived.technique = accumulated->technique;
					derived.subPass = accumulated->subPass;
					derived.hint = accumulated->hint;
					derived.biasWitness = biasWitness;
					derived.descriptors = descriptors;
					derived.staticFlags = staticFlags;
					derived.key = key;
					derived.geometrySlot = geometrySlot;
					derived.pipelineSlot = pipelineSlot;
					derived.materialSlot = materialSlot;
				} else {
					derived.valid = false;
				}
			}

			// The patch. Everything above decided what this object draws with; here it goes into the
			// record the scene phase appended, at the index that phase fixed.
			// A resident pass is patched once and kept, unless the record needs this frame's extras rows.
			const bool landBlendRecord = descriptors.technique == 8 || descriptors.technique == 19;
			const bool resident = accumulated && accumulated->resident && !descriptors.projectedUV && !landBlendRecord && residentJoining.contains(geometry);
			// The patch is kept until a phase does not renew it (LapseAccumulated): written, and noted, where it differs.
			const AccumulateSnapshot columnsBefore(tables, objectId);
			if (!resident) {
				accumulatePatched.push_back(objectId);
				if (patchedFrame.size() < tables.objects.size())
					patchedFrame.resize(tables.objects.size(), 0);
				patchedFrame[objectId] = frame;
			}
			object.materialIndex = materialSlot;
			object.pipelineIndex = pipelineSlot;
			object.flags = (object.flags & kSceneKeptFlags) | staticFlags | (accumulated ? kObjectNativeVisible : 0u) |
			               (accumulated && accumulated->sunTest ? kObjectSunTest : 0u) | (resident && accumulated->fadeDistance != 0.0f ? kObjectFadeTest : 0u) |
			               (resident && accumulated->heightTest ? kObjectHeightTest : 0u);
			tables.fadeDistance[objectId] = resident ? accumulated->fadeDistance : 0.0f;
			tables.SetWatch(objectId, Tables::kWatchExtras, (object.flags & (kObjectProjectedUV | kObjectLandBlend)) != 0);
			tables.draws[objectId].pipelineIndex = pipelineSlot;
			float emissiveMult = 1.0f;
			tables.shading[objectId] = MakeShading(*static_cast<RE::BSLightingShaderProperty*>(property), descriptors, mainPassRenderFlags, emissiveMult);
			tables.emissiveMult[objectId] = emissiveMult;
			if (descriptors.pass & kPassAdditionalAlphaMask) {
				if (fadingThisFrame++ == 0)
					++stats.fadingFrames;
				++stats.fadingDrawn;
			}
			ObjectLights lights;
			if (lightLimitFixLoaded) {
				lights.roomIndex = globals::features::lightLimitFix.GetRoomIndex(geometry);
				if (accumulated)
					lights.shadowBitMask = accumulated->pass ? LightLimitFix::GetShadowBitMask(accumulated->pass) : 0u;  // a synthetic pass has no point-light shadow (PrimaryCull)
			}
			tables.lights[objectId] = lights;
			// Per object, and only for trees: everything else keeps the pipeline template's values.
			ObjectTreeAnim tree{};
			if (object.flags & kObjectTreeAnim)
				DeriveTreeAnim(*property, tree);
			tables.treeAnim[objectId] = tree;
			// Extras rows for the objects that need them; filled at Prepass (RefreshObjectExtras), where
			// the main camera's state is current for the projection matrix.
			const bool landBlend = descriptors.technique == 8 || descriptors.technique == 19;
			if (descriptors.projectedUV || landBlend) {
				object.flags |= (descriptors.projectedUV ? kObjectProjectedUV : 0u) | (landBlend ? kObjectLandBlend : 0u);
				stats.projectedUV += descriptors.projectedUV ? 1 : 0;
				stats.landBlend += landBlend ? 1 : 0;
				if (tables.extraOffset[objectId] == kNoExtraRows)
					tables.extraOffset[objectId] = tables.AllocateExtras();
			} else {
				tables.FreeExtras(objectId);
			}
			if (resident) {
				MarkResidentSlot(objectId, { *accumulated, pipelineSlot, materialSlot });
				residentJoining.erase(geometry);
				++residentStats.joined;
			}
			columnsBefore.NoteWrite(tables, objectId);
			stats.nativeVisible += accumulated && !resident ? 1 : 0;
			stats.nativeShadowMasked += accumulated && (descriptors.pass & 0x6000u) == 0x6000u ? 1 : 0;
			stats.derivedDescriptors += accumulated ? 0 : 1;
			if (descriptors.decalGroup && accumulated) {
				// The engine's draw order for decals: the opaque group first, then within a group the
				// technique buckets in ascending order, each bucket's lists 0-4, each list's chain.
				++stats.decals[(descriptors.decalGroup - 1) & 1];
				decalOrder.push_back({ (std::uint64_t(descriptors.decalGroup) << 60) | (std::uint64_t(accumulated->technique & 0x3FFFFFFF) << 28) |
										   (std::uint64_t(accumulated->subPass & 7) << 24) | (accumulated->chainIndex & 0xFFFFFF),
					objectId });
			}
			timer.Add(BuildPart::Record);
			(void)classifyProbe;
			(void)resolveBuffers;
			(void)gpu;
		}

		TracyCZoneEnd(objectsZone);
		TracyCZoneN(residentsZone, "CS.DCLF.Accumulate.Residents", true);
		// Resident passes that were not patched (no record, a verdict of the frame, a material not ready, extras rows, or
		// the engine's own pass for the object): their entries leave residency.
		for (const auto* geometry : residentJoining) {
			residentEvictions.push_back(geometry);
			++residentStats.failed;
			const auto* pass = FindAccumulatedPass(geometry);
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
			const auto cause = pass && !pass->resident ? 0u :                                                             // the engine's pass took the object
			                   entry == tracked.end() || entry->second.objectStamp != objectStamp ? 1u :                  // no record
			                   entry->second.accumulateReasonFrame == frame ? 2u :                                          // a verdict of the frame
			                   3u;                                                                                          // material, or extras rows
			++residentStats.failedBy[cause];
			// [TEMP] which entries the engine registers although their job returned at once.
			static std::uint32_t loggedEngine = 0;
			if (cause == 0 && loggedEngine < 12 && entry != tracked.end()) {
				++loggedEngine;
				const auto* root = entry->second.sunEntryNode;
				std::string chain;
				for (const RE::NiAVObject* node = root ? root->parent : nullptr; node && chain.size() < 200; node = node->parent)
					chain += fmt::format(" <- '{}' ({})", node->name.c_str() ? node->name.c_str() : "", node->GetRTTI() ? node->GetRTTI()->name : "?");
				logger::info("[DCLF][TEMP] resident join lost to the engine's pass: '{}' under root '{}' ({}){}; pass hint {} technique {:X}",
					geometry->name.c_str() ? geometry->name.c_str() : "?", root && root->name.c_str() ? root->name.c_str() : "?",
					root && root->GetRTTI() ? root->GetRTTI()->name : "?", chain, pass->hint, pass->technique);
			}
		}
		residentJoining.clear();
		LapseAccumulated();
		KeepResidentsAlive();
		++residentStats.frames;
		residentStats.resident += residents.size();
		if (ResidentParityEnabled() && frame % 60 == 0 && !residents.empty())
			CheckResidentParity();
		{
			// A bound object can reference a slot that is not live only after a slot was freed (the sweep, a material
			// drop, a geometry slot that could not be resolved again).
			static const bool slotParity = SwitchEnabled("CS_DCLF_PERSISTENT_PARITY");
			ScopedScan scan(Scan::CheckSlots);
			if (slotsFreedThisFrame || slotParity)
				CheckObjectSlots(frameResolveBuffers);
			slotsFreedThisFrame = false;
		}
		std::optional<ScopedScan> scanStats(std::in_place, Scan::AccumulateStats);
		TracyCZoneEnd(residentsZone);
		TracyCZoneN(statsZone, "CS.DCLF.Accumulate.StatsAndDecals", true);
		static const bool slotProbe = SwitchValue("CS_DCLF_SLOT_PROBE") == "1";
		if (slotProbe)
			ProbeSlots(frameResolveBuffers);
		// The slot counts, for the reports: the frame's users every 16th frame, held in between; the live and referenced
		// counts are the slot tables'.
		const bool countSlots = frame % 16 == 0;
		if (countSlots) {
			stats.pipelines = 0;
			for (const auto used : tables.pipelineLastUsed)
				stats.pipelines += used == frame ? 1u : 0u;
		}
		// Decal draw order: sort the frame's decals by the engine's key and hand each its slot in its
		// group. Tens to a few hundred entries; the sort is the whole cost.
		// Last frame's ordinals are reset, not the whole column.
		if (tables.decalOrdinal.size() != tables.objects.size())
			tables.decalOrdinal.resize(tables.objects.size(), ~0u);
		for (const std::uint32_t o : decalOrdered)
			if (o < tables.decalOrdinal.size())
				tables.decalOrdinal[o] = ~0u;
		decalOrdered.clear();
		tables.decalCount = {};
		if (!decalOrder.empty()) {
			std::sort(decalOrder.begin(), decalOrder.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
			for (const auto& entry : decalOrder) {
				const std::uint32_t group = static_cast<std::uint32_t>(entry.key >> 60) - 1;
				tables.decalOrdinal[entry.object] = tables.decalCount[group & 1]++;
				decalOrdered.push_back(entry.object);
			}
		}
		// The 4c gate, checked over the finished tables rather than asserted from the election: no
		// native-visible object may draw on a pipeline whose lighting template came from a culled one.
		stats.templateDefects = 0;
		stats.pipelinesCulledOnly = 0;
		for (std::size_t p = 0; p < tables.geometryTemplateNative.size(); ++p)
			stats.pipelinesCulledOnly += (tables.PipelineUsed(p, frame) && !tables.geometryTemplateNative[p]) ? 1u : 0u;
		if (stats.pipelinesCulledOnly) {
			for (const auto& object : tables.objects) {
				if (!(object.flags & kObjectNativeVisible) || (object.flags & kObjectNoBindings))
					continue;
				if (object.pipelineIndex < tables.geometryTemplateNative.size() && !tables.geometryTemplateNative[object.pipelineIndex])
					++stats.templateDefects;
			}
		}
		if (countSlots) {
			stats.materials = 0;
			for (const auto used : tables.materialLastUsed)
				stats.materials += used == frame ? 1u : 0u;
		}
		stats.geometries = static_cast<std::uint32_t>(tables.geometrySlots.ReferencedCount());
		stats.geometriesAlive = static_cast<std::uint32_t>(tables.geometrySlots.AliveCount());
		stats.pipelinesAlive = static_cast<std::uint32_t>(tables.pipelineSlots.AliveCount());
		stats.materialsAlive = static_cast<std::uint32_t>(tables.materialSlots.AliveCount());
		// The material cache is evicted with the material slots (SweepSlots).
		stats.materialCacheEntries = static_cast<std::uint32_t>(materialCache.size());
		scanStats.reset();
		TracyCZoneEnd(statsZone);
		ZoneNamedN(materialTailZone, "CS.DCLF.Accumulate.MaterialTail", true);
		ScopedScan scanTail(Scan::MaterialTail);
		ProcessMaterialWrites();
		RefreshTextureTransforms();
		ValidateMaterialSlice();
	}

	void SceneStore::InvalidateVerdicts()
	{
		AbandonSceneJob();
		// Every cached negative, candidate verdict and positive derivation: they witness the object, not
		// the toggles, so a toggle that enters the classification leaves all of them stale at once.
		for (auto& [geometry, entry] : tracked) {
			entry.verdict.cached = false;
			entry.candidateFrame = 0;
			entry.derived.valid = false;
		}
		++tablesGeneration;
		fullEvaluation = true;
	}

	void SceneStore::ResetSlotTables()
	{
		AbandonSceneJob();
		for (const std::uint32_t slot : residents)
			if (slot < tables.objectGeometry.size() && tables.objectGeometry[slot])
				residentEvictions.push_back(tables.objectGeometry[slot]);
		residents.clear();
		residentPatches.clear();
		residentPos.clear();
		residentMaintenanceDirty = true;
		tables.Clear();
		for (auto& [geometry, entry] : tracked)
			entry.slot = kNoObjectSlot;
		geometryTouched.clear();
		geometryIndex.clear();
		pipelineIndex.clear();
		materialIndex.clear();
		++tablesGeneration;
		fullEvaluation = true;
		accumulatePatched.clear();
		lastPatched.clear();
	}

	void SceneStore::CheckObjectSlots(bool a_resolveBuffers)
	{
		stats.slotViolations = 0;
		for (std::size_t o = 0; o < tables.objects.size(); ++o) {
			auto& object = tables.objects[o];
			if (object.flags & kObjectNoBindings)
				continue;
			const char* what = nullptr;
			if (!tables.geometrySlots.Alive(object.geometryIndex))
				what = "geometry slot free";
			else if (a_resolveBuffers && (!tables.geometries[object.geometryIndex].vertexAddress || !tables.geometries[object.geometryIndex].indexAddress))
				what = "geometry slot unresolved";
			else if (object.pipelineIndex >= tables.pipelines.size() || tables.pipelineLastUsed[object.pipelineIndex] != frame)
				what = "pipeline slot not of this frame";
			else if (object.materialIndex >= tables.materials.size() || tables.materialLastUsed[object.materialIndex] != frame)
				what = "material slot not of this frame";
			else if (!tables.geometryTemplate[object.pipelineIndex])
				what = "pipeline without a template";
			if (!what)
				continue;
			if (stats.slotViolations++ == 0) {
				const auto* geometry = tables.objectGeometry[o];
				logger::error("[DCLF] slot check: object {} '{}' {} (geometry {} used {}, pipeline {} used {}, material {} used {}, frame {})",
					o, geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", what,
					object.geometryIndex, object.geometryIndex < tables.geometryLastUsed.size() ? tables.geometryLastUsed[object.geometryIndex] : ~0u,
					object.pipelineIndex, object.pipelineIndex < tables.pipelineLastUsed.size() ? tables.pipelineLastUsed[object.pipelineIndex] : ~0u,
					object.materialIndex, object.materialIndex < tables.materialLastUsed.size() ? tables.materialLastUsed[object.materialIndex] : ~0u, frame);
			}
			// Neutralised: nothing downstream may draw from slots that are not this frame's. Its record is no
			// longer the scene phase's, so the next delta walk writes it again.
			pendingEvaluation.push_back(tables.objectGeometry[o]);
			DropResidentSlot(static_cast<std::uint32_t>(o), true, false);
			object.flags = (object.flags & ~(kObjectNativeVisible | kObjectSunTest)) | kObjectNoBindings;
			object.geometryIndex = object.pipelineIndex = object.materialIndex = 0;
			tables.NoteChange(static_cast<std::uint32_t>(o), kChangeBindings | kChangeGeometry);
		}
	}

	void SceneStore::ProbeSlots(bool a_resolveBuffers)
	{
		static std::uint32_t logged = 0;
		if (logged >= 40)
			return;
		auto& evaluator = ConstantEvaluator::Get();
		auto& gpu = GpuResources::Get();
		std::uint32_t materialDiffers = 0, geometryDiffers = 0, materialsProbed = 0, geometriesProbed = 0;
		std::string first;
		if (evaluator.HasLightingShader()) {
			for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot) {
				if (tables.materialLastUsed[slot] != frame)
					continue;
				const auto key = tables.materialSlotKey[slot];
				MaterialRecord live;
				if (!key.first || !evaluator.EvaluateMaterial(key.first, key.second, live))
					continue;
				++materialsProbed;
				const auto& served = tables.materials[slot];
				for (std::size_t t = 0; t < served.textures.size(); ++t) {
					if (served.textures[t] != live.textures[t]) {
						if (materialDiffers++ == 0)
							first = fmt::format("material slot {} (pass {:X}) texture[{}] {} -> {}", slot, key.second, t,
								static_cast<const void*>(served.textures[t]), static_cast<const void*>(live.textures[t]));
						break;
					}
				}
			}
		}
		if (a_resolveBuffers) {
			for (std::uint32_t slot = 0; slot < tables.geometries.size(); ++slot) {
				if (tables.geometryLastUsed[slot] != frame)
					continue;
				++geometriesProbed;
				const auto& record = tables.geometries[slot];
				const auto* triShape = tables.geometrySlotKey[slot];
				const auto vertex = gpu.Resolve(record.vertexBuffer);
				const auto index = gpu.Resolve(record.indexBuffer);
				const bool same = triShape && vertex && index && vertex->address == record.vertexAddress && index->address == record.indexAddress &&
				                  reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer) == record.vertexBuffer &&
				                  reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer) == record.indexBuffer;
				if (!same && geometryDiffers++ == 0 && first.empty())
					first = fmt::format("geometry slot {} vb {:#x} -> {:#x} ib {:#x} -> {:#x} (trishape {} buffers {}/{} vs {}/{})", slot,
						record.vertexAddress, vertex ? vertex->address : 0ull, record.indexAddress, index ? index->address : 0ull,
						static_cast<const void*>(triShape), static_cast<const void*>(record.vertexBuffer), static_cast<const void*>(record.indexBuffer),
						triShape ? static_cast<const void*>(triShape->vertexBuffer) : nullptr, triShape ? static_cast<const void*>(triShape->indexBuffer) : nullptr);
			}
		}
		if (materialDiffers || geometryDiffers || frame < 12) {
			++logged;
			logger::info("[DCLF] slot probe frame {}: {} of {} used materials differ, {} of {} used geometries differ{}{}", frame, materialDiffers, materialsProbed,
				geometryDiffers, geometriesProbed, first.empty() ? "" : "; first: ", first);
		}
	}

	void SceneStore::FreeGeometrySlot(std::uint32_t a_slot)
	{
		auto& link = slotReferences.link;
		if (a_slot < link.size()) {
			tables.geometrySlots.Release(link[a_slot].slot, link[a_slot].generation, frame);
			link[a_slot] = {};
		}
		geometryIndex.erase(tables.geometrySlotKey[a_slot]);
		tables.geometrySlotKey[a_slot] = nullptr;
		tables.geometryLastUsed[a_slot] = Tables::kSlotFree;
		tables.geometrySlots.Free(a_slot);
	}

	void SceneStore::UpdateSlotReferences()
	{
		auto& r = slotReferences;
		using Reference = SlotReferences::Reference;
		auto referenceTo = [](const SlotTable& a_table, std::uint32_t a_slot) {
			return a_table.Alive(a_slot) ? Reference{ a_slot, a_table.Generation(a_slot) } : Reference{};
		};
		auto move = [&](SlotTable& a_table, Reference& a_counted, const Reference& a_now) {
			if (a_counted == a_now)
				return;
			if (a_counted.slot != SlotReferences::kNone)
				a_table.Release(a_counted.slot, a_counted.generation, frame);
			if (a_now.slot != SlotReferences::kNone)
				a_table.AddRef(a_now.slot, a_now.generation);
			a_counted = a_now;
		};
		// An object references its geometry while it has a record, and its pipeline and material while it is bound.
		auto countObject = [&](std::uint32_t o) {
			if (r.object.size() <= o)
				r.object.resize(std::size_t(o) + 1);
			auto& counted = r.object[o];
			const bool live = o < tables.objects.size() && !(tables.objects[o].flags & kObjectFree);
			const bool bound = live && !(tables.objects[o].flags & kObjectNoBindings);
			const auto& object = live ? tables.objects[o] : ObjectRecord{};
			move(tables.geometrySlots, counted.geometry, live ? referenceTo(tables.geometrySlots, object.geometryIndex) : Reference{});
			move(tables.pipelineSlots, counted.pipeline, bound ? referenceTo(tables.pipelineSlots, object.pipelineIndex) : Reference{});
			move(tables.materialSlots, counted.material, bound ? referenceTo(tables.materialSlots, object.materialIndex) : Reference{});
		};
		// A geometry slot references the next partition of its skin.
		auto countLink = [&](std::uint32_t g) {
			if (r.link.size() <= g)
				r.link.resize(std::size_t(g) + 1);
			const bool linked = tables.geometrySlots.Alive(g) && tables.geometries[g].nextPartition != kNoPartition;
			move(tables.geometrySlots, r.link[g], linked ? referenceTo(tables.geometrySlots, tables.geometries[g].nextPartition) : Reference{});
		};
		if (!r.objects.Continues(tables.changeLog, tablesGeneration) || !r.links.Continues(tables.geometryLog, tablesGeneration)) {
			// Every reference counted again: the first frame, new tables, or a log this reader fell behind.
			tables.geometrySlots.ResetReferences(frame);
			tables.pipelineSlots.ResetReferences(frame);
			tables.materialSlots.ResetReferences(frame);
			r.object.assign(tables.objects.size(), {});
			r.link.assign(tables.geometries.size(), {});
			for (std::uint32_t o = 0; o < tables.objects.size(); ++o)
				countObject(o);
			for (std::uint32_t g = 0; g < tables.geometries.size(); ++g)
				countLink(g);
			r.objects.Restart(tablesGeneration);
			r.links.Restart(tablesGeneration);
		} else {
			for (const auto& change : r.objects.Unread(tables.changeLog))
				countObject(change.slot);
			for (const std::uint32_t g : r.links.Unread(tables.geometryLog))
				countLink(g);
		}
		r.objects.Advance(tables.changeLog);
		r.links.Advance(tables.geometryLog);
	}

	void SceneStore::SweepSlots()
	{
		UpdateSlotReferences();
		std::uint32_t freed = tables.geometrySlots.Expire(frame, [&](std::uint32_t a_slot) {
			auto& link = slotReferences.link;
			if (a_slot < link.size()) {
				tables.geometrySlots.Release(link[a_slot].slot, link[a_slot].generation, frame);
				link[a_slot] = {};
			}
			geometryIndex.erase(tables.geometrySlotKey[a_slot]);
			tables.geometrySlotKey[a_slot] = nullptr;
			tables.geometryLastUsed[a_slot] = Tables::kSlotFree;
		});
		freed += tables.pipelineSlots.Expire(frame, [&](std::uint32_t a_slot) {
			pipelineIndex.erase(tables.pipelines[a_slot]);
			tables.geometryTemplate[a_slot] = nullptr;
			tables.geometryConstantsValid[a_slot] = 0;
			tables.pipelineLastUsed[a_slot] = Tables::kSlotFree;
		});
		freed += tables.materialSlots.Expire(frame, [&](std::uint32_t a_slot) {
			materialIndex.erase(tables.materialSlotKey[a_slot]);
			if (materialCache.erase(tables.materialSlotKey[a_slot]))
				++stats.materialCacheEvicted;
			tables.materialSlotKey[a_slot] = { nullptr, 0u };
			tables.materialLastUsed[a_slot] = Tables::kSlotFree;
		});
		stats.slotsSwept += freed;
		if (freed)
			slotsFreedThisFrame = true;
	}

	void SceneStore::ValidateMaterialSlice()
	{
		// The standing alarm: a few records drawn this frame, re-evaluated live and compared outside their
		// frame-sourced components. A difference is a material writer the events do not cover; it is
		// reported, not repaired, because repairing it here is what hid the missing events before.
		if (!MaterialCacheEnabled() || tables.materials.empty())
			return;
		ScopedScan scan(Scan::MaterialValidate);
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		std::uint32_t looked = 0;
		for (std::uint32_t n = 0; n < kMaterialValidationsPerFrame * kMaterialValidationStride && looked < kMaterialValidationsPerFrame; ++n) {
			const std::uint32_t slot = materialValidationCursor++ % static_cast<std::uint32_t>(tables.materials.size());
			if (tables.materialLastUsed[slot] != frame)
				continue;
			++looked;
			const auto key = tables.materialSlotKey[slot];
			MaterialRecord live;
			if (!key.first || !evaluator.EvaluateMaterial(key.first, key.second, live))
				continue;
			++stats.materialsValidated;
			MaterialRecord served = tables.materials[slot];
			MaterialSources::CopyFrameComponents(live, served, key.second);
			if (!(served == live))
				NoteStaleMaterial(slot, key, served, live);
		}
	}

	bool SceneStore::ObjectSlotsEnabled()
	{
		static const bool enabled = SwitchValue("CS_DCLF_OBJECT_SLOTS") != "0";
		return enabled;
	}

	std::uint32_t SceneStore::AcquireObjectSlot(Tracked& a_tracked, RE::BSGeometry* a_geometry)
	{
		if (!ObjectSlotsEnabled() || denseWalk) {
			const auto slot = static_cast<std::uint32_t>(tables.objects.size());
			tables.GrowObjects(std::size_t(slot) + 1);
			tables.objectGeometry[slot] = a_geometry;
			return slot;
		}
		if (a_tracked.slot != kNoObjectSlot && a_tracked.slot < tables.objectGeometry.size() && tables.objectGeometry[a_tracked.slot] == a_geometry)
			return a_tracked.slot;
		std::uint32_t slot;
		if (!tables.objectFree.empty()) {
			slot = tables.objectFree.back();
			tables.objectFree.pop_back();
		} else {
			slot = static_cast<std::uint32_t>(tables.objects.size());
			tables.GrowObjects(std::size_t(slot) + 1);
		}
		tables.objectGeometry[slot] = a_geometry;
		a_tracked.slot = slot;
		return slot;
	}

	void SceneStore::ReleaseObjectSlot(Tracked& a_entry)
	{
		const auto slot = a_entry.slot;
		a_entry.slot = kNoObjectSlot;
		a_entry.objectStamp = 0;
		shadowSetsDirty = true;
		if (slot == kNoObjectSlot || slot >= tables.objects.size() || tables.objectGeometry[slot] != a_entry.geometry.get())
			return;
		if (IsResidentSlot(slot)) {
			DropResidentSlot(slot, true, false);
			++residentStats.released;
		}
		tables.ResetObject(slot);
		tables.objectFree.push_back(slot);
		if (tables.liveObjects)
			--tables.liveObjects;
	}

	void SceneStore::EraseTracked(RE::BSGeometry* a_geometry)
	{
		if (const auto it = tracked.find(a_geometry); it != tracked.end()) {
			ReleaseObjectSlot(it->second);
			MoveBucket(it->second, Ineligible::Count);
			UnlistFadeDependent(it->first, it->second);
			UnlistDependents(it->first, it->second, true);
			tracked.erase(it);
		}
	}

	void SceneStore::SweepObjectSlots()
	{
		// Consumers look objects up in the per-frame index lists by binary search (CaptureParity's actor check).
		std::sort(tables.actorObjects.begin(), tables.actorObjects.end());
		if (!ObjectSlotsEnabled() || denseWalk) {
			tables.liveObjects = static_cast<std::uint32_t>(tables.objects.size());
			return;
		}
		// A slot this walk did not write belongs to an object that has no record this frame (ineligible now, or
		// no longer tracked): it is freed, and its Tracked entry, if it still exists, forgets it. The pointer is
		// only a key here; the geometry may already be gone.
		for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot) {
			auto* geometry = tables.objectGeometry[slot];
			if (!geometry || tables.objectSeen[slot] == walkSerial)
				continue;
			if (const auto it = tracked.find(geometry); it != tracked.end() && it->second.slot == slot)
				it->second.slot = kNoObjectSlot;
			tables.ResetObject(slot);
			tables.objectFree.push_back(slot);
		}
		tables.liveObjects = static_cast<std::uint32_t>(tables.objects.size() - tables.objectFree.size());
	}

	void SceneStore::CheckWalkParity()
	{
		// The slot walk has just run. Everything a second walk overwrites is kept and restored, so the frame goes on
		// with the slot tables exactly as they were.
		const Tables slots = tables;
		const auto savedStamp = objectStamp;
		const auto savedStats = stats;
		const auto savedSkinned = skinnedObjects;
		const auto savedDecalOrder = decalOrder;
		// The dense walk may resolve a geometry slot the delta walk did not (an object only it writes, which is a
		// difference): the index map is not part of the tables, so it is kept too.
		const auto savedGeometryIndex = geometryIndex;
		const auto savedRefreshed = refreshedGeometry;
		// The delta walk's `order` holds only what it evaluated; the reference is the whole tracked set.
		if (SceneDeltaEnabled())
			BuildFullOrder();
		referenceReasons.clear();
		denseWalk = true;
		SceneWalk(true);
		denseWalk = false;
		geometryIndex = savedGeometryIndex;
		refreshedGeometry = savedRefreshed;
		const Tables dense = std::move(tables);
		tables = slots;
		objectStamp = savedStamp;
		stats = savedStats;
		skinnedObjects = savedSkinned;
		decalOrder = savedDecalOrder;

		++walkParity.checks;
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> denseIndex;
		for (std::uint32_t d = 0; d < dense.objects.size(); ++d)
			denseIndex.emplace(dense.objectGeometry[d], d);
		auto note = [&](const char* a_what, const RE::BSGeometry* a_geometry) {
			if (walkParity.first.empty())
				walkParity.first = fmt::format("{} on '{}'", a_what, a_geometry && a_geometry->name.c_str() ? a_geometry->name.c_str() : "?");
		};
		auto same = [](const auto& a_left, const auto& a_right) { return std::memcmp(&a_left, &a_right, sizeof(a_left)) == 0; };
		// A skinned object's rows, by content: the two walks lay the palettes out in their own visiting orders.
		auto sameRows = [&](std::uint32_t a_slot, std::uint32_t a_dense) {
			const std::size_t rows = slots.boneRows[a_slot];
			if (!rows)
				return true;
			const std::size_t at = std::size_t(slots.boneOffset[a_slot]) * 4, denseAt = std::size_t(dense.boneOffset[a_dense]) * 4, floats = rows * 4;
			if (at + floats > slots.bones.size() || denseAt + floats > dense.bones.size() || at + floats > slots.previousBones.size() ||
				denseAt + floats > dense.previousBones.size())
				return false;
			return std::memcmp(&slots.bones[at], &dense.bones[denseAt], floats * sizeof(float)) == 0 &&
			       std::memcmp(&slots.previousBones[at], &dense.previousBones[denseAt], floats * sizeof(float)) == 0;
		};
		std::uint32_t liveSeen = 0;
		for (std::uint32_t s = 0; s < slots.objects.size(); ++s) {
			const auto* geometry = slots.objectGeometry[s];
			if (!geometry) {
				if (!(slots.objects[s].flags & kObjectFree)) {
					++walkParity.differ;
					note("a free slot without kObjectFree", nullptr);
				}
				continue;
			}
			++liveSeen;
			++walkParity.objects;
			const auto it = denseIndex.find(geometry);
			if (it == denseIndex.end()) {
				++walkParity.extra;
				note("a slot the dense walk has no object for", geometry);
				continue;
			}
			const std::uint32_t d = it->second;
			denseIndex.erase(it);
			const char* what = nullptr;
			// Every record keeps its accumulated half across walks (LapseAccumulated): compared as the walk left it, which
			// is the scene half alone (the accumulated half's columns are the accumulate phase's, checked by the change log).
			auto objectRecord = slots.objects[s];
			auto drawRecord = slots.draws[s];
			objectRecord.flags = slots.sceneFlags[s];
			objectRecord.materialIndex = objectRecord.pipelineIndex = 0;
			drawRecord.pipelineIndex = 0;
			if (!same(objectRecord, dense.objects[d]) || slots.sceneFlags[s] != dense.sceneFlags[d]) {
				what = "the object record";
				if (walkParity.first.empty()) {
					const auto entryIt = tracked.find(const_cast<RE::BSGeometry*>(geometry));
					const auto& a = slots.objects[s];
					const auto& b = dense.objects[d];
					walkParity.first = fmt::format("the object record on '{}' (per-frame {}): flags {:X}/{:X} scene {:X}/{:X} geometry {}/{} world {} previous {} bound {}",
						geometry->name.c_str() ? geometry->name.c_str() : "?", entryIt != tracked.end() && entryIt->second.perFrame, a.flags, b.flags, slots.sceneFlags[s], dense.sceneFlags[d],
						a.geometryIndex, b.geometryIndex, std::memcmp(a.world, b.world, sizeof(a.world)) == 0, std::memcmp(a.previousWorld, b.previousWorld, sizeof(a.previousWorld)) == 0,
						a.boundRadius == b.boundRadius && std::memcmp(a.boundCenter, b.boundCenter, sizeof(a.boundCenter)) == 0);
				}
			}
			else if (!same(drawRecord, dense.draws[d]))
				what = "the draw";
			else if (slots.skinPartitions[s] != dense.skinPartitions[d] || slots.boneRows[s] != dense.boneRows[d] || !sameRows(s, d))
				what = "the skin rows";
			else if (slots.shadowTechnique[s] != dense.shadowTechnique[d] || slots.shadowReject[s] != dense.shadowReject[d])
				what = "the shadow verdict";
			else if (slots.sunEntry[s] != dense.sunEntry[d]) {
				what = "the sun entry";
				if (walkParity.first.empty()) {
					const auto entryIt = tracked.find(const_cast<RE::BSGeometry*>(geometry));
					const auto& a = slots.sunEntry[s];
					const auto& b = dense.sunEntry[d];
					walkParity.first = fmt::format("the sun entry on '{}' (per-frame {}, traits {:X}, entry node '{}'): kept ({:.2f} {:.2f} {:.2f} r {:.2f}) now ({:.2f} {:.2f} {:.2f} r {:.2f})",
						geometry->name.c_str() ? geometry->name.c_str() : "?", entryIt != tracked.end() && entryIt->second.perFrame, entryIt != tracked.end() ? PerFrameTraits(entryIt->second, *geometry) : 999u,
						entryIt != tracked.end() && entryIt->second.sunEntryNode && entryIt->second.sunEntryNode->name.c_str() ? entryIt->second.sunEntryNode->name.c_str() : "?",
						a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
				}
			} else if (slots.shadowDiffuse[s] != dense.shadowDiffuse[d] || slots.shadowMaterial[s] != dense.shadowMaterial[d])
				what = "the shadow material";
			else if ((slots.faceStream[s] == kNoFaceStream) != (dense.faceStream[d] == kNoFaceStream))
				what = "the face stream";
			else if (slots.faceStream[s] != kNoFaceStream) {
				const auto& a = slots.faceStreams[slots.faceStream[s]];
				const auto& b = dense.faceStreams[dense.faceStream[d]];
				if (a.object != s || b.object != d || a.region != b.region || a.vertexCount != b.vertexCount || a.generation != b.generation || a.positions != b.positions)
					what = "the face stream";
			}
			if (what) {
				++walkParity.differ;
				note(what, geometry);
			}
		}
		walkParity.missing += static_cast<std::uint32_t>(denseIndex.size());
		// The classifications themselves, records or not: a kept verdict the reference no longer reaches is stale
		// whether or not it decides a record (a negative one leaves the object to the engine).
		for (const auto& [geometry, entry] : tracked) {
			if (entry.candidateFrame == 0)
				continue;
			const auto it = referenceReasons.find(geometry);
			if (it == referenceReasons.end() || it->second == entry.candidateReason)
				continue;
			++walkParity.staleVerdicts;
			if (walkParity.firstStale.empty())
				walkParity.firstStale = fmt::format("'{}' kept {} ({} frames old, per-frame {}, traits {:X}) now {}", geometry->name.c_str() ? geometry->name.c_str() : "?",
					kIneligibleNames[static_cast<std::size_t>(entry.candidateReason)], frame - entry.candidateFrame, entry.perFrame, PerFrameTraits(entry, *geometry),
					kIneligibleNames[static_cast<std::size_t>(it->second)]);
		}
		// The traits: an entry a fresh classification would evaluate every frame, or on a heavier path, is one whose event
		// was missed, even while its record still matches.
		if (SceneDeltaEnabled()) {
			ankerl::unordered_dense::map<const RE::NiAVObject*, bool> freshMotion;
			for (auto& [geometry, entry] : tracked) {
				if (entry.candidateFrame == 0)
					continue;
				const auto it = referenceReasons.find(geometry);
				if (it == referenceReasons.end() || it->second != entry.candidateReason)
					continue;
				std::uint32_t traits = 0;
				const auto [perFrame, light] = PerFrameOf(entry, *geometry, it->second, traits, &freshMotion);
				if (!perFrame || (entry.perFrame && (!entry.lightTraits || (light && !(traits & ~entry.lightTraits)))))
					continue;
				++walkParity.staleTraits;
				if (walkParity.firstStaleTraits.empty())
					walkParity.firstStaleTraits = fmt::format("'{}' {} traits {:X} now {:X} ({} frames since classified)", geometry->name.c_str() ? geometry->name.c_str() : "?",
						entry.perFrame ? "per-frame with" : "kept, no", entry.lightTraits, traits, frame - entry.candidateFrame);
			}
		}
		if (!denseIndex.empty())
			note("an object with no slot", denseIndex.begin()->first);
		if (liveSeen != slots.liveObjects || slots.liveObjects + slots.objectFree.size() != slots.objects.size()) {
			++walkParity.differ;
			note("the live count", nullptr);
		}
		// The per-frame lists, as sets: the rows were compared per object above (the slot tables keep each palette's
		// block, so their palette arrays have holes the dense walk's have not), and the actors are mapped to geometries
		// below.
		auto sorted = [](auto a_list) {
			std::sort(a_list.begin(), a_list.end());
			return a_list;
		};
		if (slots.shadowKeysUsed.size() != dense.shadowKeysUsed.size() || sorted(slots.shadowTextureSet) != sorted(dense.shadowTextureSet) ||
			slots.actorObjects.size() != dense.actorObjects.size()) {
			++walkParity.differ;
			note("a per-frame list", nullptr);
		} else {
			std::vector<const RE::BSGeometry*> slotActors, denseActors;
			for (const auto o : slots.actorObjects)
				slotActors.push_back(slots.objectGeometry[o]);
			for (const auto o : dense.actorObjects)
				denseActors.push_back(dense.objectGeometry[o]);
			std::sort(slotActors.begin(), slotActors.end());
			std::sort(denseActors.begin(), denseActors.end());
			if (slotActors != denseActors || !std::is_sorted(slots.actorObjects.begin(), slots.actorObjects.end())) {
				++walkParity.differ;
				note("the actor list", nullptr);
			}
		}
		if (walkParity.checks % 5 == 0) {
			const bool ok = !walkParity.differ && !walkParity.missing && !walkParity.extra && !walkParity.staleVerdicts && !walkParity.staleTraits;
			logger::info("[DCLF] walk parity: {} checks, {} objects compared, {} differ, {} missing, {} extra, {} stale verdicts, {} stale traits ({} slots, {} free){}{}{}{}{}{}",
				walkParity.checks, walkParity.objects, walkParity.differ, walkParity.missing, walkParity.extra, walkParity.staleVerdicts, walkParity.staleTraits,
				tables.objects.size(), tables.objectFree.size(), ok ? " <- OK" : "; first: ", ok ? "" : walkParity.first,
				walkParity.firstStale.empty() ? "" : "; first stale verdict: ", walkParity.firstStale,
				walkParity.firstStaleTraits.empty() ? "" : "; first stale traits: ", walkParity.firstStaleTraits);
			walkParity = {};
		}
	}

	bool SceneStore::SceneDeltaEnabled()
	{
		// The fade events come from AE hooks (FadeWatch); elsewhere the full walk stays.
		static const bool enabled = ObjectSlotsEnabled() && SwitchValue("CS_DCLF_SCENE_DELTA") != "0" && REL::Module::IsAE();
		return enabled;
	}

	void SceneStore::InstallSceneEvents()
	{
		static bool installed = false;
		if (installed || !SceneDeltaEnabled())
			return;
		installed = true;
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSFadeNode[0] };
		const auto onVisible = reinterpret_cast<const std::uintptr_t*>(vtable.address())[0x34];
		stl::detour_thunk<FadeOnVisible>(onVisible);
		stl::detour_thunk<FadeUpdate>(REL::Offset(0x147a160).address());
		stl::detour_thunk<PropertySetFlags>(REL::Offset(0x147bee0).address());
		stl::detour_thunk<PropertySetMaterial>(REL::Offset(0x147bff0).address());
		stl::detour_thunk<PrependController>(REL::Offset(0xd268d0).address());
		stl::detour_thunk<HavokNodeTransform>(REL::Offset(0xea55a0).address());
		stl::write_vfunc<0x2A, LodFadeRenderPasses>(RE::VTABLE_BSLightingShaderProperty[0]);
		lodFadeEventsInstalled = true;
		if (SwitchValue("CS_DCLF_SWITCH_EVENTS") != "0" && InstallSwitchStores()) {
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
		logger::info("[DCLF] scene events installed (fades, property flags and materials, LOD fades, Havok node transforms, controllers): OnVisible at {:#x}",
			onVisible - REL::Module::get().base() + 0x140000000);
	}

	bool SceneStore::SwitchEventsLive()
	{
		return switchEventsInstalled;
	}

	void SceneStore::TestSwitchStore(RE::NiSwitchNode* a_switch, std::int32_t a_index)
	{
		SwitchIndexStore(a_switch, a_index);
	}

	bool SceneStore::CatchUpSwitch(RE::NiSwitchNode& a_switch)
	{
		// NiSwitchNode::OnVisible: childRevID.SetAt(index, revID) (FUN_140d29990), then the child's UpdateDownwardPass
		// (vtable slot 0x2C) with NiUpdateData { savedTime (+0x130), flags bit 1 of +0x128 as the update flag }.
		SwitchState state;
		if (!ReadSwitch(a_switch, state) || state.index < 0)
			return false;
		const auto& children = a_switch.GetChildren();
		const auto index = static_cast<std::uint32_t>(state.index);
		if (index >= children.capacity() || !children[static_cast<std::uint16_t>(index)] || !state.childRevID || index >= state.childRevCapacity ||
			state.childRevID[index] == state.revID)
			return false;
		auto* base = reinterpret_cast<std::byte*>(&a_switch);
		using SetRevision = void (*)(void*, std::uint32_t, const std::uint32_t*);
		static const REL::Relocation<SetRevision> setRevision{ REL::Offset(0xd29990) };
		setRevision(base + 0x138, index, reinterpret_cast<const std::uint32_t*>(base + 0x134));
		struct UpdateData
		{
			float time;
			std::uint32_t flags;
		} data{ *reinterpret_cast<const float*>(base + 0x130), (state.flags >> 1) & 1u };
		auto* child = children[static_cast<std::uint16_t>(index)].get();
		using UpdateDownwardPass = void (*)(RE::NiAVObject*, UpdateData*, std::uint32_t);
		(*reinterpret_cast<UpdateDownwardPass* const*>(child))[0x2C](child, &data, 0);
		return true;
	}

	bool SceneStore::TakeSwitchChanges(std::vector<const RE::NiAVObject*>& a_out)
	{
		a_out.clear();
		a_out.swap(switchesApplied);
		return std::exchange(switchResync, false) || !SwitchEventsLive();
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
			if (CatchUpSwitch(*switchNode))
				++delta.switchCatchUps;
			if (switchesApplied.size() < kMaxSwitchChanges)
				switchesApplied.push_back(node);
			else
				switchResync = true;
			if (a_full)
				continue;
			// Every entry under it: which of them the switch draws is a classification input (ClassifyFrame).
			constexpr std::size_t kMaxNodes = 4096;
			std::vector<RE::NiAVObject*> stack{ node };
			for (std::size_t visited = 0; !stack.empty() && visited < kMaxNodes; ++visited) {
				auto* object = stack.back();
				stack.pop_back();
				if (auto* geometry = object->AsGeometry()) {
					if (const auto entry = tracked.find(geometry); entry != tracked.end()) {
						Reclassify(entry->first, entry->second);
						++delta.switchReclassified;
					}
				} else if (auto* inner = object->AsNode()) {
					for (auto& child : inner->GetChildren())
						if (child)
							stack.push_back(child.get());
				}
			}
		}
		switchPending.clear();
		switchPendingIndex.clear();
	}

	void SceneStore::BuildFullOrder()
	{
		order.clear();
		order.reserve(tracked.size());
		for (auto& [geometry, entry] : tracked) {
			entry.scheduledWalk = walkSerial;
			order.push_back({ geometry, &entry, nullptr });
		}
	}

	void SceneStore::Schedule(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_full)
	{
		if (a_full)
			a_tracked.fullWalk = walkSerial;
		// An entry the first round only moved can still be written in full by a later round (its geometry slot).
		if (a_tracked.scheduledWalk == walkSerial && a_tracked.movedWalk != walkSerial)
			return;
		a_tracked.scheduledWalk = walkSerial;
		a_tracked.movedWalk = 0;
		order.push_back({ a_geometry, &a_tracked, nullptr });
	}

	std::uint64_t SceneStore::ShadingInputsOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		std::uint64_t hash = 0xcbf29ce484222325ull;
		auto mix = [&hash](std::uint64_t a_value) { hash = (hash ^ a_value) * 0x100000001b3ull; };
		const auto& data = a_geometry.GetGeometryRuntimeData();
		const auto* property = data.shaderProperty.get();
		mix(reinterpret_cast<std::uintptr_t>(property));
		if (property) {
			mix(property->flags.underlying());
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
			mix(reinterpret_cast<std::uintptr_t>(material));
			// Only a lighting property's material is a BSLightingShaderMaterialBase; for any other the record reads
			// nothing more. The cast is the entry's (WriteObject resolves it per property pointer).
			const bool lighting = a_tracked.castProperty == property ? a_tracked.castResult != nullptr :
			                                                           netimmerse_cast<const RE::BSLightingShaderProperty*>(property) != nullptr;
			if (material && lighting) {
				mix(std::bit_cast<std::uint32_t>(material->materialAlpha));
				const auto* texture = material->diffuseTexture ? material->diffuseTexture->rendererTexture : nullptr;
				mix(reinterpret_cast<std::uintptr_t>(texture ? texture->resourceView : nullptr));
			}
		}
		const auto* alpha = data.alphaProperty.get();
		mix(reinterpret_cast<std::uintptr_t>(alpha));
		if (alpha)
			mix((std::uint64_t(alpha->alphaFlags) << 8) | alpha->alphaThreshold);
		return hash;
	}

	SceneStore::ShadowInputs SceneStore::ShadowInputsOf(std::uint32_t a_slot) const
	{
		if (a_slot == kNoObjectSlot || a_slot >= tables.objects.size() || (tables.objects[a_slot].flags & kObjectFree))
			return {};
		const auto& object = tables.objects[a_slot];
		return { tables.shadowDiffuse[a_slot], object.geometryIndex < tables.geometries.size() ? tables.geometries[object.geometryIndex].vertexDesc : 0,
			tables.shadowTechnique[a_slot], object.flags & (kObjectNoShadow | kObjectTwoSided | kObjectFree), tables.shadowReject[a_slot], tables.skyTechnique[a_slot] };
	}

	bool SceneStore::AppendKeptSkin(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		auto& data = a_geometry->GetGeometryRuntimeData();
		auto* skin = data.skinInstance.get();
		const std::uint32_t slot = a_tracked.slot;
		if (!skin || !SkinnedEnabled() || !(tables.objects[slot].flags & kObjectSkinned))
			return false;
		const auto* partitions = skin->skinPartition.get();
		std::uint32_t mask = 0;
		if (partitions) {
			mask = SkinPartitionMask(*skin, LodRowOf(*a_geometry, data.shaderProperty.get()));
			if (!mask)
				return false;
		}
		const std::uint32_t rows = skin->numMatrices * 3;
		if (!rows || !skin->boneMatrices || !skin->prevBoneMatrices || rows > 240 || rows != tables.boneRows[slot])
			return false;
		if (a_tracked.skinUpdatedFrame != frame) {
			UpdateSkin(skin, a_geometry->world);
			a_tracked.skinUpdatedFrame = frame;
		}
		skinnedObjects.push_back(a_geometry);
		const auto* current = static_cast<const float*>(skin->boneMatrices);
		const auto* previous = static_cast<const float*>(skin->prevBoneMatrices);
		// The rows into the skin's own block (the size is the same, checked above), noted only when they differ.
		const std::size_t at = std::size_t(tables.boneOffset[slot]) * 4;
		const std::size_t bytes = std::size_t(rows) * 4 * sizeof(float);
		if (std::memcmp(&tables.bones[at], current, bytes) != 0 || std::memcmp(&tables.previousBones[at], previous, bytes) != 0) {
			std::memcpy(&tables.bones[at], current, bytes);
			std::memcpy(&tables.previousBones[at], previous, bytes);
			tables.NoteChange(slot, kChangePalette);
		}
		const auto partitionMask = static_cast<std::uint8_t>(partitions && partitions->numPartitions > 1 ? mask : 0);
		if (tables.skinPartitions[slot] != partitionMask)
			tables.NoteChange(slot, kChangeSkin);
		tables.skinPartitions[slot] = partitionMask;
		++stats.skinned;
		stats.boneRows += rows;
		return true;
	}

	void SceneStore::MoveObject(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		// What WriteObject writes from the geometry's placement, the same way; the geometry slot is kept by
		// FinishDeltaWalk like any kept slot's.
		auto& object = tables.objects[a_tracked.slot];
		const ObjectRecord before = object;
		const auto entry = tables.sunEntry[a_tracked.slot];
		StoreTransform(a_geometry->world, object.world);
		StoreTransform(a_geometry->previousWorld, object.previousWorld);
		object.boundCenter[0] = a_geometry->worldBound.center.x;
		object.boundCenter[1] = a_geometry->worldBound.center.y;
		object.boundCenter[2] = a_geometry->worldBound.center.z;
		object.boundRadius = a_geometry->worldBound.radius;
		tables.sunEntry[a_tracked.slot] = SunEntryOf(a_tracked, *a_geometry);
		// Only a real change is one: a mover that stood still this frame changes nothing.
		if (std::memcmp(before.world, object.world, sizeof(before.world)) != 0 || std::memcmp(before.previousWorld, object.previousWorld, sizeof(before.previousWorld)) != 0 ||
			std::memcmp(before.boundCenter, object.boundCenter, sizeof(before.boundCenter)) != 0 || before.boundRadius != object.boundRadius ||
			entry != tables.sunEntry[a_tracked.slot])
			tables.NoteChange(a_tracked.slot, kChangePlacement);
		a_tracked.movedWalk = walkSerial;
		++delta.moved;
	}

	bool SceneStore::RootMoves(const RE::NiAVObject* a_root)
	{
		// Only a reference's root: a multibound's bound is its shape's, and an actor's entry is never tested.
		if (!a_root || !a_root->GetUserData())
			return false;
		// Kept until an event under the root forgets it (ScheduleRoot) or its last dependent leaves (UnlistDependents).
		const auto [motion, inserted] = rootMotion.try_emplace(a_root, false);
		if (inserted)
			motion->second = RootMovesNow(a_root);
		return motion->second;
	}

	bool SceneStore::RootMovesNow(const RE::NiAVObject* a_root)
	{
		if (!a_root || !a_root->GetUserData())
			return false;
		bool moves = false;
		constexpr std::size_t kMaxNodes = 4096;
		std::vector<const RE::NiAVObject*> stack{ a_root };
		for (std::size_t visited = 0; !stack.empty() && visited < kMaxNodes && !moves; ++visited) {
			auto* object = const_cast<RE::NiAVObject*>(stack.back());
			stack.pop_back();
			if (object->GetControllers() || NonFixedBody(*object)) {
				moves = true;
			} else if (auto* geometry = object->AsGeometry()) {
				// A skin's bound follows its bones, which walk parity found moving with no controller or body in sight.
				moves = geometry->GetGeometryRuntimeData().skinInstance != nullptr;
			} else if (auto* node = object->AsNode()) {
				for (auto& child : node->GetChildren())
					if (child)
						stack.push_back(child.get());
			}
		}
		return moves;
	}

	std::uint64_t SceneStore::ClassifyInputsOf(const RE::BSGeometry& a_geometry)
	{
		// What ClassifyStatic reads: the renderer data and skin, the property, its flags, material and fade state, the
		// material alpha, and the alpha property.
		std::uint64_t hash = 0xcbf29ce484222325ull;
		auto mix = [&hash](std::uint64_t a_value) { hash = (hash ^ a_value) * 0x100000001b3ull; };
		const auto& data = a_geometry.GetGeometryRuntimeData();
		mix(reinterpret_cast<std::uintptr_t>(data.rendererData));
		mix(reinterpret_cast<std::uintptr_t>(data.skinInstance.get()));
		const auto* property = data.shaderProperty.get();
		mix(reinterpret_cast<std::uintptr_t>(property));
		if (property) {
			mix(property->flags.underlying());
			mix(reinterpret_cast<std::uintptr_t>(property->material));
			mix(FadeStateOf(const_cast<RE::BSShaderProperty*>(property)));
			if (const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property); lighting && property->material)
				mix(std::bit_cast<std::uint32_t>(static_cast<const RE::BSLightingShaderMaterialBase*>(property->material)->materialAlpha));
		}
		if (const auto* alpha = data.alphaProperty.get()) {
			mix(reinterpret_cast<std::uintptr_t>(alpha));
			mix((std::uint64_t(alpha->alphaFlags) << 8) | alpha->alphaThreshold);
		}
		return hash;
	}

	void SceneStore::ListDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		// The properties as this evaluation read them: an event on either classifies the entry again. A property swapped
		// without an event is taken up at the entry's next evaluation, and walk parity is the alarm for one that is not.
		const auto& data = a_geometry->GetGeometryRuntimeData();
		const void* property = data.shaderProperty.get();
		const void* alpha = data.alphaProperty.get();
		if (property != a_tracked.listedProperty) {
			if (a_tracked.listedProperty)
				Unlist(propertyDependents, a_tracked.listedProperty, a_geometry);
			if (property)
				propertyDependents[property].push_back(a_geometry);
			a_tracked.listedProperty = property;
		}
		if (alpha != a_tracked.listedAlpha) {
			if (a_tracked.listedAlpha)
				Unlist(propertyDependents, a_tracked.listedAlpha, a_geometry);
			if (alpha)
				propertyDependents[alpha].push_back(a_geometry);
			a_tracked.listedAlpha = alpha;
		}
	}

	void SceneStore::UnlistDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_root)
	{
		if (a_tracked.listedProperty)
			Unlist(propertyDependents, a_tracked.listedProperty, a_geometry);
		if (a_tracked.listedAlpha)
			Unlist(propertyDependents, a_tracked.listedAlpha, a_geometry);
		a_tracked.listedProperty = nullptr;
		a_tracked.listedAlpha = nullptr;
		if (a_root && a_tracked.listedRoot) {
			MarkSunEntryDirty(a_tracked.listedRoot);
			// The others under it: its bound takes this one in no more. The node is a key here; it may be gone.
			if (Unlist(rootDependents, a_tracked.listedRoot, a_geometry))
				dirtyRoots.push_back(a_tracked.listedRoot);
			else
				rootMotion.erase(a_tracked.listedRoot);
			a_tracked.listedRoot = nullptr;
		}
	}

	bool SceneStore::SkyOcclusionEnabled()
	{
		return Toggles::Get().Active().skyOcclusion && globals::features::skylighting.loaded;
	}

	void SceneStore::DropSunCandidates()
	{
		sunCandidateSet.clear();
		primarySignature.clear();
		sunEntriesDirty.clear();
		sunCandidates.reset();
		++sunCandidatesGeneration;
	}

	bool SceneStore::SunEntryAllows(const Tracked& a_tracked, bool a_switchNodes)
	{
		// A table object: its shadow is the shadow epoch's (the claims decide, per frame), or the caster rule rejects it.
		if (a_tracked.slot != kNoObjectSlot)
			return true;
		if (a_tracked.candidateFrame == 0)
			return false;
		switch (a_tracked.candidateReason) {
		case Ineligible::NotLightingShader:  // no class but Lighting casts into the cascades (CS_DCLF_CASCADE_PROBE)
		case Ineligible::Hidden:             // the cull skips app-culled nodes
		case Ineligible::AlphaBlend:         // the caster rule: no pass for an alpha-blended property
		case Ineligible::Fading:             // ... nor for a fade below one
			return true;
		case Ineligible::Switch:  // an unselected child: the switch node culls only the selected one
			return a_switchNodes;
		default:
			return false;
		}
	}

	bool SceneStore::PrimaryEntryAllows(const Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		if (a_tracked.slot == kNoObjectSlot || a_tracked.candidateFrame == 0 || a_tracked.candidateReason != Ineligible::None)
			return false;
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		if (!property || (property->flags.underlying() & 0xc000000ull))  // the decal flags: their depth is the native depth pass's
			return false;
		const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		return !(alpha && (alpha->alphaFlags & 1));
	}

	void SceneStore::UpdateSunCandidates(bool a_full)
	{
		bool changed = a_full;
		if (a_full) {
			sunCandidateSet.clear();
			primarySignature.clear();
			sunEntriesDirty.clear();
			for (const auto& [root, dependents] : rootDependents)
				sunEntriesDirty.push_back(root);
		}
		if (!sunEntriesDirty.empty()) {
			std::sort(sunEntriesDirty.begin(), sunEntriesDirty.end());
			sunEntriesDirty.erase(std::unique(sunEntriesDirty.begin(), sunEntriesDirty.end()), sunEntriesDirty.end());
			const bool switchNodes = SwitchNodesEnabled();
			for (const auto* root : sunEntriesDirty) {
				bool candidate = false;
				std::uint64_t signature = 1469598103934665603ull;
				if (const auto it = rootDependents.find(root); it != rootDependents.end() && !it->second.empty()) {
					candidate = true;
					for (auto* geometry : it->second) {
						const auto entry = tracked.find(geometry);
						if (entry == tracked.end() || !SunEntryAllows(entry->second, switchNodes)) {
							candidate = false;
							break;
						}
						const std::uint64_t allows = PrimaryEntryAllows(entry->second, *geometry) ? 1 : 0;
						signature = (signature ^ (reinterpret_cast<std::uintptr_t>(geometry) * 2 + allows)) * 1099511628211ull;
					}
				}
				if (candidate ? sunCandidateSet.insert(root).second : sunCandidateSet.erase(root) != 0)
					changed = true;
				if (candidate) {
					const auto [slot, inserted] = primarySignature.try_emplace(root, signature);
					if (!inserted && slot->second != signature) {
						slot->second = signature;
						changed = true;
					}
				} else {
					primarySignature.erase(root);
				}
			}
			sunEntriesDirty.clear();
		}
		if (changed) {
			++sunCandidatesGeneration;
			++stats.sunCandidateChanges;
			return;
		}
		if (sunCandidatesBuilt == sunCandidatesGeneration && sunCandidates)
			return;
		// A walk that changed nothing: the snapshot for the generation now in force.
		auto snapshot = std::make_shared<SunCandidates>();
		snapshot->generation = sunCandidatesGeneration;
		snapshot->entries.reserve(sunCandidateSet.size());
		std::uint32_t index = 0;
		for (const auto* root : sunCandidateSet) {
			snapshot->entries.emplace(root, index);
			if (const auto it = rootDependents.find(root); it != rootDependents.end())
				for (auto* geometry : it->second)
					if (snapshot->geometries.emplace(geometry, static_cast<std::uint32_t>(snapshot->geometryEntry.size())).second) {
						snapshot->geometryEntry.push_back(index);
						const auto entry = tracked.find(geometry);
						snapshot->primaryGeometry.push_back(entry != tracked.end() && PrimaryEntryAllows(entry->second, *geometry) ? 1 : 0);
					}
			++index;
		}
		sunCandidates = std::move(snapshot);
		sunCandidatesBuilt = sunCandidatesGeneration;
		++stats.sunCandidateSnapshots;
	}

	void SceneStore::Reclassify(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		a_tracked.candidateFrame = 0;
		Schedule(a_geometry, a_tracked);
	}

	bool SceneStore::PlacementMatters(const Tracked& a_tracked)
	{
		const auto reason = a_tracked.candidateReason;
		return a_tracked.slot != kNoObjectSlot || a_tracked.candidateFrame == 0 || reason == Ineligible::None || reason == Ineligible::Hidden ||
		       reason == Ineligible::Switch || reason == Ineligible::Actor;
	}

	void SceneStore::ScheduleRoot(const RE::NiAVObject* a_root)
	{
		rootMotion.erase(a_root);
		const auto it = rootDependents.find(a_root);
		if (it == rootDependents.end())
			return;
		for (auto* geometry : it->second)
			if (const auto entry = tracked.find(geometry); entry != tracked.end() && PlacementMatters(entry->second))
				Reclassify(entry->first, entry->second);
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
		// known to move has its dependents on the light path's placement.
		for (const RE::NiAVObject* object = a_node; object; object = object->parent) {
			const auto dependents = rootDependents.find(object);
			if (dependents == rootDependents.end())
				continue;
			if (const auto motion = rootMotion.find(object); motion != rootMotion.end() && motion->second) {
				bool placed = true;
				for (auto* geometry : dependents->second)
					if (const auto entry = tracked.find(geometry); entry != tracked.end() && !placedEveryFrame(entry->second) && entry->second.slot != kNoObjectSlot)
						placed = false;
				if (placed)
					continue;
			}
			ScheduleRoot(object);
		}
		// Every entry below it: its placement, and its traits (a body or controller it did not have when classified).
		constexpr std::size_t kMaxNodes = 4096;
		std::vector<RE::NiAVObject*> stack{ a_node };
		for (std::size_t visited = 0; !stack.empty() && visited < kMaxNodes; ++visited) {
			auto* object = stack.back();
			stack.pop_back();
			if (auto* geometry = object->AsGeometry()) {
				if (const auto entry = tracked.find(geometry); entry != tracked.end() && !placedEveryFrame(entry->second) && PlacementMatters(entry->second))
					Reclassify(entry->first, entry->second);
			} else if (auto* node = object->AsNode()) {
				for (auto& child : node->GetChildren())
					if (child)
						stack.push_back(child.get());
			}
		}
	}

	void SceneStore::MoveBucket(Tracked& a_tracked, Ineligible a_bucket)
	{
		const std::uint8_t bucket = a_bucket == Ineligible::Count ? Tracked::kNoBucket : static_cast<std::uint8_t>(a_bucket);
		if (a_tracked.bucket == bucket)
			return;
		if (a_tracked.bucket != Tracked::kNoBucket && buckets[a_tracked.bucket])
			--buckets[a_tracked.bucket];
		if (bucket != Tracked::kNoBucket)
			++buckets[bucket];
		a_tracked.bucket = bucket;
	}

	void SceneStore::ListFadeDependent(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		// Only a kept record needs the event: a per-frame one is written in full anyway, unless it only moves or only
		// follows a switch.
		const auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
		const RE::BSFadeNode* node = property && (!a_tracked.perFrame || a_tracked.lightTraits) ? property->fadeNode : nullptr;
		if (node == a_tracked.fadeNode)
			return;
		UnlistFadeDependent(a_geometry, a_tracked);
		if (node) {
			fadeDependents[node].push_back(a_geometry);
			a_tracked.fadeNode = node;
		}
	}

	void SceneStore::UnlistFadeDependent(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		if (!a_tracked.fadeNode)
			return;
		if (const auto it = fadeDependents.find(a_tracked.fadeNode); it != fadeDependents.end()) {
			auto& list = it->second;
			if (const auto at = std::find(list.begin(), list.end(), a_geometry); at != list.end()) {
				*at = list.back();
				list.pop_back();
			}
			if (list.empty())
				fadeDependents.erase(it);
		}
		a_tracked.fadeNode = nullptr;
	}

	std::uint32_t SceneStore::PerFrameTraits(const Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		// The movable set (dclf-event-driven-tables.md, "Reverse-engineering results"): a static reference never moves
		// in place, so what moves is owned by an actor, animated by a controller, or simulated by Havok. A skin's
		// palette, a face's snapshot and a switch's selection change without moving anything. A controller on the
		// shader or alpha property animates the material alpha the shadow verdict reads.
		std::uint32_t traits = 0;
		traits |= a_tracked.faceShape ? kTraitFace : 0u;
		traits |= a_tracked.actorOwned ? kTraitActor : 0u;
		// A switch's selection changes by event with the switch events (ApplySwitchEvents), not every frame.
		traits |= a_tracked.parentReason == Ineligible::Switch && !SwitchEventsLive() ? kTraitSwitch : 0u;
		const auto& data = a_geometry.GetGeometryRuntimeData();
		traits |= data.skinInstance ? kTraitSkin : 0u;
		if (const auto* property = data.shaderProperty.get(); property && property->GetControllers())
			traits |= kTraitAnimatedShading;
		if (const auto* alpha = data.alphaProperty.get(); alpha && alpha->GetControllers())
			traits |= kTraitAnimatedShading;
		for (const RE::NiAVObject* object = &a_geometry; object; object = object->parent) {
			if (object->GetControllers() || NonFixedBody(*object)) {
				traits |= kTraitMoves;
				break;
			}
			if (object == a_tracked.categoryNode)
				break;
		}
		return traits;
	}

	std::pair<bool, std::uint32_t> SceneStore::PerFrameOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, Ineligible a_reason, std::uint32_t& a_traits,
		ankerl::unordered_dense::map<const RE::NiAVObject*, bool>* a_freshMotion)
	{
		// An unselected switch child may get a record when its switch selects it: every frame without the switch events,
		// by event with them (ApplySwitchEvents), like any other verdict.
		const bool mayRecord = (a_tracked.parentReason == Ineligible::Switch && !SwitchEventsLive()) || a_reason == Ineligible::None || DeferredToAccumulate(a_reason) ||
		                       ShadowOnlyCaster(a_reason, const_cast<RE::BSGeometry&>(a_geometry));
		std::uint32_t traits = PerFrameTraits(a_tracked, a_geometry);
		if (mayRecord && !(traits & (kTraitFace | kTraitActor)) && a_tracked.sunEntryNode) {
			bool moves = false;
			if (a_freshMotion) {
				const auto [motion, inserted] = a_freshMotion->try_emplace(a_tracked.sunEntryNode, false);
				if (inserted)
					motion->second = RootMovesNow(a_tracked.sunEntryNode);
				moves = motion->second;
			} else {
				moves = RootMoves(a_tracked.sunEntryNode);
			}
			if (moves)
				traits |= kTraitRootMoves;
		}
		a_traits = traits;
		// Per frame when a frame can change its record: its verdict lets it have one (or it is under a switch), and its
		// inputs change every frame. A movable slot's chain is re-read every frame whatever its verdict, as the design has
		// it: an actor's equipment is shown, hidden and swapped with no event of its own (walk parity caught a shield and a
		// chopping axe left hidden), and a visibility controller hides and shows its node.
		const bool frameVerdict = a_reason == Ineligible::Hidden || a_reason == Ineligible::Switch || a_reason == Ineligible::Actor;
		const bool perFrame = a_tracked.faceShape || (mayRecord && traits) || (traits & kTraitActor) || (frameVerdict && (traits & kTraitMoves));
		// The light path takes only a record's placement, palette, switch selection or shading; an entry it cannot have a
		// record for is written in full, which re-reads its verdict.
		const std::uint32_t light = perFrame && mayRecord && !(traits & ~(kTraitSwitch | kTraitMoves | kTraitRootMoves | kTraitAnimatedShading | kTraitSkin)) ? traits : 0u;
		return { perFrame, light };
	}

	void SceneStore::CheckChangeLog()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_CHANGE_LOG_PARITY");
		if (!enabled)
			return;
		auto& c = changeParity;
		if (c.cursor.active) {
			const bool continues = c.cursor.Continues(tables.changeLog, tablesGeneration);
			c.cursor.active = false;
			if (!continues) {
				++c.skipped;  // the log started again (a load, a reset): every reader resyncs anyway
			} else {
				++c.checks;
				// What the log says changed since the snapshot, per slot.
				ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> logged;
				for (const auto& change : tables.changeLog.From(c.cursor.position))
					logged[change.slot] |= change.causes;
				for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot) {
					++c.slots;
					const auto now = tables.ColumnsOf(slot);
					const bool grown = slot >= c.snapshot.size();
					// A slot grown since is new: a live one must be in the log at all.
					const std::uint32_t causes = grown ? ((tables.objects[slot].flags & kObjectFree) ? 0u : kChangeAll) : Tables::CausesBetween(c.snapshot[slot], now);
					if (!causes)
						continue;
					++c.changed;
					const auto it = logged.find(slot);
					const std::uint32_t missing = grown ? (it == logged.end() ? causes : 0u) : causes & ~(it == logged.end() ? 0u : it->second);
					if (!missing)
						continue;
					++c.missing;
					if (c.first.empty()) {
						std::string names;
						for (std::uint32_t bits = missing; bits; bits &= bits - 1)
							names += fmt::format("{}{}", names.empty() ? "" : "+", kChangeCauseNames[std::countr_zero(bits)]);
						const auto* geometry = tables.objectGeometry[slot];
						c.first = fmt::format("frame {}: slot {} '{}' changed {} with no log entry for it", frame, slot,
							geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", names);
					}
				}
			}
		}
		if (frame % 60 == 0) {
			c.snapshot.resize(tables.objects.size());
			for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot)
				c.snapshot[slot] = tables.ColumnsOf(slot);
			c.cursor.Restart(tablesGeneration);
			c.cursor.Advance(tables.changeLog);
		}
	}

	void SceneStore::LapseAccumulated()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Lapse");
		// A patch this phase did not renew: the engine no longer registers the object (culled, hidden, faded out), or it
		// has no bindings this frame. Its record goes back to the scene half; a resident's is kept by its residency.
		std::uint32_t lapsed = 0;
		for (const std::uint32_t slot : lastPatched) {
			if (slot < patchedFrame.size() && patchedFrame[slot] == frame)
				continue;
			if (IsResidentSlot(slot))
				continue;
			ResetAccumulatedHalf(slot);
			++lapsed;
		}
		delta.restored += lapsed;
		lastPatched.swap(accumulatePatched);
		accumulatePatched.clear();
	}

	void SceneStore::ResetAccumulatedHalf(std::uint32_t a_slot)
	{
		if (a_slot >= tables.objects.size() || (tables.objects[a_slot].flags & kObjectFree))
			return;
		const auto columnsBefore = tables.ColumnsOf(a_slot);
		auto& object = tables.objects[a_slot];
		object.flags = tables.sceneFlags[a_slot];
		object.materialIndex = 0;
		object.pipelineIndex = 0;
		tables.draws[a_slot].pipelineIndex = 0;
		tables.shading[a_slot] = ObjectShading{};
		tables.emissiveMult[a_slot] = 1.0f;
		tables.lights[a_slot] = ObjectLights{};
		tables.treeAnim[a_slot] = ObjectTreeAnim{};
		tables.FreeExtras(a_slot);
		tables.fadeDistance[a_slot] = 0.0f;
		tables.NoteWrite(a_slot, columnsBefore);
	}

	bool SceneStore::ResidentParityEnabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_RESIDENT_PARITY");
		return enabled;
	}

	bool SceneStore::ResidentCapable(const RE::BSGeometry* a_geometry) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end())
			return false;
		const auto& entry = it->second;
		// A record, eligible, written by events only: the light path's placement is fine (MoveObject keeps the
		// accumulated half), and so is a kept skin's (AppendKeptSkin: the palette rows and the partition mask, from the
		// same LOD row the synthetic pass reads; a skin it cannot keep is written in full, which ends the residency). A
		// full write every frame is not, and neither are the per-frame inputs of a face, an actor or animated shading.
		return entry.slot != kNoObjectSlot && entry.objectStamp == objectStamp && entry.candidateReason == Ineligible::None && !entry.faceShape &&
		       !entry.actorOwned && !(entry.lightTraits & kTraitAnimatedShading) && !(entry.perFrame && !entry.lightTraits);
	}

	std::string SceneStore::ResidentIncapableReason(const RE::BSGeometry* a_geometry) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end())
			return "untracked";
		const auto& entry = it->second;
		if (entry.slot == kNoObjectSlot || entry.objectStamp != objectStamp)
			return "no record";
		if (entry.candidateReason != Ineligible::None)
			return fmt::format("verdict {}", static_cast<int>(entry.candidateReason));
		if (entry.faceShape)
			return "face";
		if (entry.actorOwned)
			return "actor";
		if (entry.lightTraits & kTraitSkin)
			return "skin trait";
		if (entry.lightTraits & kTraitAnimatedShading)
			return "animated shading";
		if (entry.perFrame && !entry.lightTraits)
			return "written every frame";
		if (a_geometry->GetGeometryRuntimeData().skinInstance)
			return "skin instance";
		return "capable";
	}

	void SceneStore::MarkResidentSlot(std::uint32_t a_slot, const ResidentPatch& a_patch)
	{
		residentMaintenanceDirty = true;
		if (residentPos.size() <= a_slot)
			residentPos.resize(std::max<std::size_t>(a_slot + 1, tables.objects.size()), kNotResident);
		// The accumulate phase notes the join, and a patch that changed, with the rest of its write.
		if (residentPos[a_slot] != kNotResident) {
			residentPatches[residentPos[a_slot]] = a_patch;
			return;
		}
		residentPos[a_slot] = static_cast<std::uint32_t>(residents.size());
		residents.push_back(a_slot);
		residentPatches.push_back(a_patch);
		if (a_slot < tables.residentSlot.size())
			tables.residentSlot[a_slot] = 1;
	}

	void SceneStore::DropResidentSlot(std::uint32_t a_slot, bool a_notify, bool a_restore)
	{
		if (!IsResidentSlot(a_slot))
			return;
		residentMaintenanceDirty = true;
		const std::uint32_t at = residentPos[a_slot];
		const std::uint32_t last = residents.back();
		residents[at] = last;
		residentPatches[at] = residentPatches.back();
		residentPos[last] = at;
		residents.pop_back();
		residentPatches.pop_back();
		residentPos[a_slot] = kNotResident;
		if (a_slot < tables.residentSlot.size()) {
			tables.residentSlot[a_slot] = 0;
			tables.NoteChange(a_slot, kChangeMembership);
		}
		if (a_restore)
			ResetAccumulatedHalf(a_slot);
		if (a_notify && a_slot < tables.objectGeometry.size() && tables.objectGeometry[a_slot])
			residentEvictions.push_back(tables.objectGeometry[a_slot]);
	}

	void SceneStore::EndResidency(const RE::BSGeometry* a_geometry)
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it != tracked.end() && it->second.slot != kNoObjectSlot)
			DropResidentSlot(it->second.slot, false, true);
	}

	void SceneStore::EndAllResidency()
	{
		residentMaintenanceDirty = true;
		for (const std::uint32_t slot : residents) {
			ResetAccumulatedHalf(slot);
			residentPos[slot] = kNotResident;
			if (slot < tables.residentSlot.size()) {
				tables.residentSlot[slot] = 0;
				tables.NoteChange(slot, kChangeMembership);
			}
		}
		residents.clear();
		residentPatches.clear();
	}

	void SceneStore::TakeResidentEvictions(std::vector<const RE::BSGeometry*>& a_geometries, std::vector<const RE::NiAVObject*>& a_roots)
	{
		a_geometries.clear();
		a_geometries.swap(residentEvictions);
		a_roots.clear();
		a_roots.swap(residentRootEvents);
	}

	void SceneStore::KeepResidentsAlive()
	{
		ZoneScopedN("CS.DCLF.Accumulate.KeepResidents");
		if (residentMaintenanceDirty) {
			ZoneScopedN("CS.DCLF.Accumulate.RebuildResidentMaintenance");
			residentPipelines.clear();
			residentMaterials.clear();
			residentTrees.clear();
			ankerl::unordered_dense::set<std::uint32_t> pipelines, materials;
			for (const std::uint32_t slot : residents) {
				const auto& object = tables.objects[slot];
				if (pipelines.insert(object.pipelineIndex).second)
					residentPipelines.emplace_back(object.pipelineIndex, slot);
				if (materials.insert(object.materialIndex).second)
					residentMaterials.push_back(object.materialIndex);
				if (object.flags & kObjectTreeAnim)
					residentTrees.push_back(slot);
			}
			residentMaintenanceDirty = false;
		}
		for (const auto& [pipeline, slot] : residentPipelines) {
			if (pipeline < tables.pipelineLastUsed.size() && tables.pipelineLastUsed[pipeline] != frame) {
				// No accumulated object used the pipeline this frame: the resident's property is its lighting template, as
				// the first user's would be. A resident is drawn whenever the GPU finds it, so it counts as kept.
				const auto* geometry = tables.objectGeometry[slot];
				tables.pipelineLastUsed[pipeline] = frame;
				tables.geometryTemplate[pipeline] = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				tables.geometryTemplateNative[pipeline] = 1;
			}
		}
		for (const std::uint32_t material : residentMaterials)
			if (material < tables.materialLastUsed.size())
				tables.materialLastUsed[material] = frame;
		for (const std::uint32_t slot : residentTrees) {
			const auto* geometry = tables.objectGeometry[slot];
			// A tree's wind state is the tree manager's, advanced while the feedback keeps the root's kAccumulated: taken
			// every frame, as the accumulate phase takes it for every other drawn tree.
			if (geometry)
				if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get()) {
					ObjectTreeAnim tree = tables.treeAnim[slot];
					DeriveTreeAnim(*property, tree);
					if (std::memcmp(&tree, &tables.treeAnim[slot], sizeof(tree)) != 0) {
						tables.treeAnim[slot] = tree;
						tables.NoteChange(slot, kChangeTree);
					}
				}
		}
	}

	void SceneStore::CheckResidentParity()
	{
		++residentStats.parityChecks;
		std::uint32_t logged = 0;
		for (std::size_t r = 0; r < residents.size(); ++r) {
			const std::uint32_t slot = residents[r];
			const auto& patch = residentPatches[r];
			const auto* geometry = tables.objectGeometry[slot];
			if (!geometry)
				continue;
			// A root that has started to fade since the feedback's last decode: that decode's successor ends the residency (the
			// frame of latency every root state the feedback services has), so its pass is not compared.
			if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
				property && property->fadeNode && property->fadeNode->GetRuntimeData().currentFade < 1.0f) {
				++residentStats.parityPending;
				continue;
			}
			++residentStats.parityChecked;
			AccumulatedPass fresh;
			const bool built = PrimaryCull::FreshSyntheticPass(*geometry, fresh);
			const bool passSame = built && fresh.technique == patch.pass.technique && fresh.subPass == patch.pass.subPass && fresh.hint == patch.pass.hint &&
			                      fresh.lodRow == patch.pass.lodRow && fresh.sunTest == patch.pass.sunTest;
			const auto& object = tables.objects[slot];
			const bool recordSame = object.pipelineIndex == patch.pipeline && object.materialIndex == patch.material && (object.flags & kObjectNativeVisible) &&
			                        !(object.flags & kObjectNoBindings);
			residentStats.parityPass += passSame ? 0 : 1;
			residentStats.parityRecord += recordSame ? 0 : 1;
			if ((!passSame || !recordSame) && logged++ < 4)
				logger::info("[DCLF][TEMP] resident parity: '{}' pass {} (technique {:X} -> {:X}, hint {} -> {}, LOD row {} -> {}, sun test {} -> {}), record {} (pipeline {} -> {}, material {} -> {}, flags {:X})",
					geometry->name.c_str() ? geometry->name.c_str() : "?", passSame ? "same" : built ? "DIFFERS" : "NOT BUILT", patch.pass.technique, fresh.technique,
					patch.pass.hint, fresh.hint, patch.pass.lodRow, fresh.lodRow, patch.pass.sunTest, fresh.sunTest, recordSame ? "same" : "DIFFERS", patch.pipeline,
					object.pipelineIndex, patch.material, object.materialIndex, object.flags);
		}
	}

	void SceneStore::EvaluateRound(PartTimer& a_timer, WalkResult& a_result, std::size_t a_first)
	{
		for (std::size_t i = a_first; i < order.size(); ++i) {
			RE::BSGeometry* geometry = order[i].geometry;
			Tracked& entry = *order[i].tracked;
			a_timer.Add(BuildPart::LoopTail);
			// A static that only moves or follows a switch, whose classification stands: what the full walk would take
			// again is the switch's verdict (WriteObject's cached branch) and the placement.
			if (a_first == 0 && entry.lightTraits && entry.perFrame && entry.fullWalk != walkSerial && entry.candidateFrame != 0) {
				const bool recorded = entry.slot != kNoObjectSlot && entry.objectStamp == objectStamp;
				bool kept = true;
				if (entry.lightTraits & kTraitSwitch) {
					// The selection is what changes (a harvested plant, a tree's LOD); the rest of the chain is a static's,
					// fixed like any kept record's (dclf-event-driven-tables.md).
					const bool verdictKept = (entry.candidateReason == Ineligible::None || entry.candidateReason == Ineligible::Switch) &&
					                         (entry.candidateReason == Ineligible::None) == recorded;
					if (verdictKept && entry.switchNode && SwitchNodesEnabled())
						kept = SwitchSelects(*entry.switchNode, entry.switchChild) == (entry.candidateReason == Ineligible::None);
					else
						kept = verdictKept && ClassifyFrame(entry) == entry.candidateReason;
				} else {
					kept = recorded;
				}
				if (kept && (entry.lightTraits & kTraitAnimatedShading))
					kept = ShadingInputsOf(entry, *geometry) == entry.shadingInputs;
				// Last: it writes the rows only when the record is kept (an unselected switch child has none).
				if (kept && recorded && (entry.lightTraits & kTraitSkin))
					kept = AppendKeptSkin(geometry, entry);
				if (kept) {
					if (recorded && (entry.lightTraits & (kTraitMoves | kTraitRootMoves | kTraitSkin))) {
						MoveObject(geometry, entry);
					} else {
						entry.movedWalk = walkSerial;  // not written in full: a later round may still write it
						++delta.kept;
					}
					continue;
				}
			}
			const ShadowInputs shadowBefore = ShadowInputsOf(entry.slot);
			const bool hadSlot = entry.slot != kNoObjectSlot;
			const std::uint32_t slotBefore = entry.slot;
			const Tables::Columns columnsBefore = hadSlot ? tables.ColumnsOf(slotBefore) : Tables::Columns{};
			const Ineligible reasonBefore = entry.candidateReason;
			Ineligible bucket = Ineligible::Count;
			const bool written = WriteObject(geometry, entry, a_timer, a_result, true, bucket);
			MoveBucket(entry, bucket);
			if (!written && entry.slot != kNoObjectSlot)
				ReleaseObjectSlot(entry);
			// What the write changed (Tables::changeLog); a slot taken anew is new in everything.
			if (written && entry.slot != kNoObjectSlot) {
				if (entry.slot == slotBefore)
					tables.NoteWrite(entry.slot, columnsBefore);
				else
					tables.NoteChange(entry.slot, kChangeAll);
			}
			// What its sun entry's candidacy reads (SunEntryAllows).
			if (hadSlot != (entry.slot != kNoObjectSlot) || reasonBefore != entry.candidateReason)
				MarkSunEntryDirty(entry.sunEntryNode);
			shadowSetsDirty |= !(ShadowInputsOf(written ? entry.slot : kNoObjectSlot) == shadowBefore);
			ListDependents(geometry, entry);
			// Classified now: a new entry, or an event took its classification again.
			if (entry.candidateFrame == frame) {
				// Per frame only when a frame can change its record: a face shape (classified every frame), an entry
				// under a switch, or one whose verdict lets it have a record, with inputs that change every frame. An
				// entry left out by its verdict is taken again when the verdict is due, like any other.
				std::uint32_t traits = 0;
				std::tie(entry.perFrame, entry.lightTraits) = PerFrameOf(entry, *geometry, entry.candidateReason, traits);
				entry.switchNode = nullptr;
				entry.switchChild = nullptr;
				if (entry.lightTraits & kTraitSwitch) {
					std::uint32_t switches = 0;
					const RE::NiAVObject* child = geometry;
					// ClassifyFrame's walk: every node below the category node.
					for (RE::NiNode* node = geometry->parent; node && node != entry.categoryNode; child = node, node = node->parent) {
						if (auto* switchNode = node->AsSwitchNode()) {
							++switches;
							entry.switchNode = switchNode;
							entry.switchChild = child;
						}
					}
					if (switches != 1)
						entry.switchNode = nullptr;
				}
			}
			if (entry.perFrame && !entry.perFrameListed) {
				entry.perFrameListed = true;
				perFrameSet.push_back(geometry);
			}
			if (written) {
				if (entry.lightTraits & kTraitAnimatedShading)
					entry.shadingInputs = ShadingInputsOf(entry, *geometry);
				ListFadeDependent(geometry, entry);
				// A static's previous transform is its current one from its second update on; until then it is
				// written again.
				if (!entry.perFrame && !SameTransform(geometry->world, geometry->previousWorld)) {
					pendingEvaluation.push_back(geometry);
					++delta.settling;
				}
			} else {
				UnlistFadeDependent(geometry, entry);
			}
		}
	}

	void SceneStore::DeltaWalk()
	{
		++delta.walks;
		WalkResult result;
		const bool full = fullEvaluation;
		// The shadow sets cover every record; BeginWalk clears them, and they are rebuilt only when an input changed.
		auto keptTextureSet = std::move(tables.shadowTextureSet);
		auto keptTextureSeen = std::move(tables.shadowTextureSeen);
		auto keptKeys = std::move(tables.shadowKeysUsed);
		auto keptSkyKeys = std::move(tables.skyKeysUsed);
		shadowSetsDirty = full || shadowSetsDirty;
		BeginWalk(!full);
		PartTimer timer(stats.partMs);
		if (full) {
			// Everything, as the full walk: after a reset, a load or a live toggle nothing kept can be trusted.
			fullEvaluation = false;
			++delta.full;
			perFrameSet.clear();
			pendingEvaluation.clear();
			fadeChanged.clear();
			fadeDependents.clear();
			propertyChanged.clear();
			nodeChanged.clear();
			dirtyRoots.clear();
			propertyDependents.clear();
			accumulatePatched.clear();
			lastPatched.clear();
			rootMotion.clear();
			tables.InvalidateChangeLog();
			buckets = {};
			for (auto& [geometry, entry] : tracked) {
				entry.perFrameListed = false;
				entry.bucket = Tracked::kNoBucket;
				entry.fadeNode = nullptr;
				entry.listedProperty = nullptr;
				entry.listedAlpha = nullptr;
			}
			BuildFullOrder();
		} else {
			order.clear();
			std::size_t counted = 0;
			auto count = [&](std::uint64_t& a_into) {
				a_into += order.size() - counted;
				counted = order.size();
			};
			for (std::size_t i = 0; i < perFrameSet.size();) {
				const auto it = tracked.find(perFrameSet[i]);
				if (it == tracked.end() || !it->second.perFrameListed || !it->second.perFrame || it->second.scheduledWalk == walkSerial) {
					// Erased (or erased and added again, which listed the new entry anew), or no longer per frame
					// (queued for its next classification).
					if (it != tracked.end() && !it->second.perFrame && it->second.scheduledWalk != walkSerial)
						it->second.perFrameListed = false;
					perFrameSet[i] = perFrameSet.back();
					perFrameSet.pop_back();
					continue;
				}
				Schedule(it->first, it->second, false);
				++i;
			}
			count(delta.perFrame);
			for (auto* geometry : pendingEvaluation)
				if (const auto it = tracked.find(geometry); it != tracked.end())
					Schedule(it->first, it->second);
			pendingEvaluation.clear();
			count(delta.pending);
			for (const auto* node : fadeChanged) {
				const auto dependents = fadeDependents.find(node);
				if (dependents == fadeDependents.end())
					continue;
				for (auto* geometry : dependents->second)
					if (const auto it = tracked.find(geometry); it != tracked.end())
						Reclassify(it->first, it->second);
			}
			fadeChanged.clear();
			count(delta.fade);
			delta.propertyEvents += propertyChanged.size();
			delta.nodeEvents += nodeChanged.size();
			for (const void* key : propertyChanged) {
				const auto dependents = propertyDependents.find(key);
				if (dependents == propertyDependents.end())
					continue;
				for (auto* geometry : dependents->second)
					if (const auto it = tracked.find(geometry); it != tracked.end())
						Reclassify(it->first, it->second);
			}
			propertyChanged.clear();
			count(delta.property);
			std::sort(nodeChanged.begin(), nodeChanged.end(), [](const auto& a_left, const auto& a_right) { return a_left.get() < a_right.get(); });
			nodeChanged.erase(std::unique(nodeChanged.begin(), nodeChanged.end()), nodeChanged.end());
			for (auto& node : nodeChanged)
				ApplyNodeEvent(node.get());
			nodeChanged.clear();
			count(delta.node);
			std::sort(dirtyRoots.begin(), dirtyRoots.end());
			dirtyRoots.erase(std::unique(dirtyRoots.begin(), dirtyRoots.end()), dirtyRoots.end());
			for (const auto* root : dirtyRoots)
				ScheduleRoot(root);
			if (!residents.empty())
				residentRootEvents.insert(residentRootEvents.end(), dirtyRoots.begin(), dirtyRoots.end());
			dirtyRoots.clear();
			count(delta.roots);
		}
		ApplySwitchEvents(full);
		EvaluateRound(timer, result, 0);
		if (!shadowSetsDirty) {
			tables.shadowTextureSet = std::move(keptTextureSet);
			tables.shadowTextureSeen = std::move(keptTextureSeen);
			tables.shadowKeysUsed = std::move(keptKeys);
			tables.skyKeysUsed = std::move(keptSkyKeys);
		}
		FinishDeltaWalk(timer, result);
		if (full)
			SweepObjectSlots();
		UpdateSunCandidates(full);
		EndFaceWalk();
		stats.ineligible = buckets;
		delta.evaluated += order.size();
		delta.evaluatedMax = std::max(delta.evaluatedMax, static_cast<std::uint32_t>(order.size()));
		delta.live += tables.liveObjects;
	}

	void SceneStore::FinishDeltaWalk(PartTimer& a_timer, WalkResult& a_result)
	{
		// The kept slots' geometry slots: a referenced one lives (Tables::geometrySlots), so nothing renews them here. Their buffer
		// references are touched every 64 frames, staggered by slot, against GpuResources' kEvictFrames. A slot whose touch
		// fails, every unresolved slot once buffers start resolving (the render graph came back), and a slot
		// ResolveGeometrySlot freed are stale: the objects drawing them are written again, which resolves them.
		auto& gpu = GpuResources::Get();
		const bool resolveBuffers = frameResolveBuffers;
		const std::size_t second = order.size();
		std::optional<ScopedScan> scanStale(std::in_place, Scan::DeltaStale);
		staleGeometrySlots.clear();
		if (resolveBuffers) {
			const bool every = !geometryResolvedLastWalk;
			for (std::uint32_t g = every ? 0u : (frame & 63u); g < tables.geometries.size(); g += every ? 1u : 64u) {
				const auto used = tables.geometryLastUsed[g];
				if (!tables.geometrySlots.Alive(g) || used == frame)
					continue;  // free, or written this frame (resolved or touched by the write)
				const auto& record = tables.geometries[g];
				if (!record.vertexAddress || !gpu.Touch(record.vertexBuffer) || !gpu.Touch(record.indexBuffer))
					staleGeometrySlots.push_back(g);
			}
		}
		geometryResolvedLastWalk = resolveBuffers;
		staleGeometrySlots.insert(staleGeometrySlots.end(), freedGeometry.begin(), freedGeometry.end());
		freedGeometry.clear();
		if (!staleGeometrySlots.empty()) {
			std::sort(staleGeometrySlots.begin(), staleGeometrySlots.end());
			staleGeometrySlots.erase(std::unique(staleGeometrySlots.begin(), staleGeometrySlots.end()), staleGeometrySlots.end());
			for (std::uint32_t s = 0; s < tables.objects.size(); ++s) {
				const auto& object = tables.objects[s];
				if ((object.flags & kObjectFree) || tables.objectSeen[s] == walkSerial)
					continue;
				// A skin of several partitions draws the chain of slots linked from its first (WriteObject).
				bool stale = false;
				std::uint32_t g = object.geometryIndex;
				for (std::uint32_t link = 0; !stale && link < kMaxSkinPartitions && g != kNoPartition; ++link) {
					stale = g >= tables.geometries.size() || std::binary_search(staleGeometrySlots.begin(), staleGeometrySlots.end(), g);
					if (!stale && !tables.skinPartitions[s] && !(object.flags & kObjectSkinned))
						break;
					g = stale ? g : tables.geometries[g].nextPartition;
				}
				if (!stale)
					continue;
				++delta.geometryDirty;
				if (const auto it = tracked.find(tables.objectGeometry[s]); it != tracked.end()) {
					Schedule(it->first, it->second);
				} else {
					tables.ResetObject(s);
					tables.objectFree.push_back(s);
					shadowSetsDirty = true;
				}
			}
		}
		scanStale.reset();
		EvaluateRound(a_timer, a_result, second);
		// A geometry slot re-resolved in place for one object: every kept object drawing it copied the old record.
		if (!refreshedGeometry.empty()) {
			std::sort(refreshedGeometry.begin(), refreshedGeometry.end());
			const std::size_t third = order.size();
			for (std::uint32_t s = 0; s < tables.objects.size(); ++s) {
				const auto& object = tables.objects[s];
				if ((object.flags & kObjectFree) || tables.objectSeen[s] == walkSerial ||
					!std::binary_search(refreshedGeometry.begin(), refreshedGeometry.end(), object.geometryIndex))
					continue;
				if (const auto it = tracked.find(tables.objectGeometry[s]); it != tracked.end())
					Schedule(it->first, it->second);
			}
			delta.geometryDirty += order.size() - third;
			EvaluateRound(a_timer, a_result, third);
			refreshedGeometry.clear();
		}
		// The per-frame lists that cover every record, kept or written: the shadow casters' textures and pipelines,
		// rebuilt when a record's inputs to them changed.
		std::sort(tables.actorObjects.begin(), tables.actorObjects.end());
		tables.liveObjects = static_cast<std::uint32_t>(tables.objects.size() - tables.objectFree.size());
		if (!shadowSetsDirty) {
			stats.shadowCasters = keptShadowCasters;
			stats.shadowRejects = keptShadowRejects;
			a_timer.Add(BuildPart::Record);
			return;
		}
		shadowSetsDirty = false;
		ScopedScan scanSets(Scan::ShadowSets);
		tables.shadowTextureSet.clear();
		tables.shadowTextureSeen.clear();
		tables.shadowKeysUsed.clear();
		tables.skyKeysUsed.clear();
		stats.shadowCasters = 0;
		stats.shadowRejects = {};
		for (std::uint32_t s = 0; s < tables.objects.size(); ++s) {
			const auto& object = tables.objects[s];
			if (object.flags & kObjectFree)
				continue;
			++stats.shadowRejects[tables.shadowReject[s] < stats.shadowRejects.size() ? tables.shadowReject[s] : 0];
			if (const auto sky = tables.skyTechnique[s]) {
				if (auto* diffuse = tables.shadowDiffuse[s]; diffuse && tables.shadowTextureSeen.insert(diffuse).second)
					tables.shadowTextureSet.push_back(diffuse);
				const ShadowPipelineKey key{ sky, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u, VertexLayoutOf(tables.geometries[object.geometryIndex].vertexDesc) };
				if (std::find(tables.skyKeysUsed.begin(), tables.skyKeysUsed.end(), key) == tables.skyKeysUsed.end())
					tables.skyKeysUsed.push_back(key);
			}
			if (object.flags & kObjectNoShadow)
				continue;
			++stats.shadowCasters;
			if (auto* diffuse = tables.shadowDiffuse[s]; diffuse && tables.shadowTextureSeen.insert(diffuse).second)
				tables.shadowTextureSet.push_back(diffuse);
			const ShadowPipelineKey key{ tables.shadowTechnique[s], (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
				VertexLayoutOf(tables.geometries[object.geometryIndex].vertexDesc) };
			if (std::find(tables.shadowKeysUsed.begin(), tables.shadowKeysUsed.end(), key) == tables.shadowKeysUsed.end())
				tables.shadowKeysUsed.push_back(key);
		}
		keptShadowCasters = stats.shadowCasters;
		keptShadowRejects = stats.shadowRejects;
		a_timer.Add(BuildPart::Record);
	}

	std::int32_t SceneStore::FindObject(const RE::BSGeometry* a_geometry) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		return it == tracked.end() || it->second.objectStamp != objectStamp ? -1 : static_cast<std::int32_t>(it->second.objectId);
	}

	Ineligible SceneStore::Classify(RE::BSGeometry* a_geometry) const
	{
		auto it = tracked.find(a_geometry);
		if (it == tracked.end())
			return Ineligible::NotTriShape;
		const Ineligible reason = ClassifyStatic(*a_geometry, nullptr);
		return reason != Ineligible::None ? reason : ClassifyFrame(it->second);
	}

	bool SceneStore::GetTrackInfo(const RE::BSGeometry* a_geometry, std::uint32_t& a_frame, TrackSource& a_source) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end())
			return false;
		a_frame = it->second.trackedFrame;
		a_source = it->second.trackedBy;
		return true;
	}

	bool SceneStore::GetCategoryInfo(const RE::NiNode* a_node, std::uint32_t& a_frame, std::uint8_t& a_cause) const
	{
		const auto it = categoryFound.find(a_node);
		if (it == categoryFound.end())
			return false;
		a_frame = it->second.first;
		a_cause = it->second.second;
		return true;
	}

	bool SceneStore::IsTracked(const RE::BSGeometry* a_geometry) const
	{
		return tracked.contains(const_cast<RE::BSGeometry*>(a_geometry));
	}

	Ineligible SceneStore::ReasonThisFrame(const RE::BSGeometry* a_geometry, bool* a_accumulate) const
	{
		if (a_accumulate)
			*a_accumulate = false;
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end())
			return Ineligible::None;
		if (it->second.accumulateReasonFrame == frame) {
			if (a_accumulate)
				*a_accumulate = true;
			return it->second.accumulateReason;
		}
		return it->second.candidateReason;
	}
}
