#include "Internal.h"

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

	bool SceneStore::IsLoadingScreenUp()
	{
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
	}

	void SceneStore::ProcessEvents()
	{
		// A scene placement job no BeforeShadowMaps joined (a frame without shadow maps): its items name tracked
		// entries, which the events below may erase.
		JoinPlacements();
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
			auto& capture = PassCapture::Get();
			capture.ClearFrameClaims();
			capture.PublishClaims(std::make_shared<const PassCapture::ClaimSet>());
			for (std::uint32_t mode = 0; mode < PassCapture::kShadowModes; ++mode)
				capture.PublishShadowClaims(mode, nullptr);
			SceneTracker::FreeEvents(tracker.Drain());
			DrainFadeEvents(fadeChanged);
			fadeChanged.clear();
			DrainPropertyEvents(propertyChanged);
			propertyChanged.clear();
			DrainLodFadeEvents(propertyChanged);
			propertyChanged.clear();
			DrainNodeEvents(nodeChanged);
			nodeChanged.clear();
			moveEvents.Discard();
			movedFrame.clear();
			hiddenEvents.Discard();
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
			++trackedLayout;
			sceneIdentity.Reset();
			categoryNodes.clear();
			validationCursor = 0;
			fullEvaluation = true;
			fadeDependents.clear();
			propertyDependents.clear();
			rootDependents.clear();
			dirtyRoots.clear();
			rootMotion.clear();
			movingRoots.clear();
			hiddenDependents.clear();
			DropSunCandidates();
			buckets = {};
		}

		// Drained before the category refresh, so a detach this frame can force it: a detach can take a
		// category node with it, and the signature cannot see that until the cell itself goes.
		SceneTracker::Event* events = nullptr;
		{
			DCLF_SCENE_PART(CategoryNodes, "CS.DCLF.Scene.CategoryNodes");
			events = tracker.Drain();
			bool sawDetach = false;
			for (const auto* event = events; event && !sawDetach; event = event->next)
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
			for (auto* event = events; event; event = event->next) {
				if (event->type == SceneTracker::EventType::Attached) {
					++stats.attachedEvents;
					if (!categoryNodes.empty())
						AddSubtree(event->node.get());
				} else {
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
			SceneTracker::FreeEvents(events);
		}

		{
			// Once a frame: ProcessEvents runs at the present and again at the scene's start.
			DCLF_SCENE_PART(Validate, "CS.DCLF.Scene.Validate");
			if (std::exchange(validatedFrame, frame) != frame)
				ValidateSlice();
		}
		DCLF_SCENE_PART(StructuralEvents, "CS.DCLF.Scene.StructuralEvents");
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
		stl::detour_thunk<PropertySetFlags>(REL::Offset(kPropertySetFlags).address());
		stl::detour_thunk<PropertySetMaterial>(REL::Offset(kPropertySetMaterial).address());
		stl::detour_thunk<PrependController>(REL::Offset(kPrependController).address());
		stl::detour_thunk<HavokNodeTransform>(REL::Offset(kHavokNodeTransform).address());
		stl::write_vfunc<0x2A, LodFadeRenderPasses>(RE::VTABLE_BSLightingShaderProperty[0]);
		lodFadeEventsInstalled = true;
		if (InstallSwitchStores()) {
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
		InstallMoveEvents();
		hiddenEventsInstalled = InstallHiddenStores();
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
			VisitSubtree(node, [&](RE::NiAVObject& a_object) {
				if (auto* geometry = a_object.AsGeometry())
					if (const auto entry = tracked.find(geometry); entry != tracked.end()) {
						Reclassify(entry->first, entry->second);
						++delta.switchReclassified;
					}
				return true;
			});
		}
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
