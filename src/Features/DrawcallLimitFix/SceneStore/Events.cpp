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
			sceneIdentity.Reset();
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

	void SceneStore::InstallSceneEvents()
	{
		static bool installed = false;
		if (installed)
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
	}

	bool SceneStore::SwitchEventsLive()
	{
		return switchEventsInstalled;
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
}
