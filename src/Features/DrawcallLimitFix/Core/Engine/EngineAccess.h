#pragma once

// Raw access to the engine, for the hooks and the engine state DCLF reads without CommonLib declarations. Every address here and at
// the hooks is an Offset: Address Library ids, resolved to the module-relative offset (as REL::Offset takes them) of the running
// runtime.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

namespace DCLF::Engine
{
	/** @brief The game versions DCLF's addresses are checked against: anything else gets no address at all (Supported). */
	enum class Runtime
	{
		Unsupported,
		Se1597,   // SE 1.5.97
		Ae1170,   // AE 1.6.1170
		Ae17104,  // AE 1.7.104
	};

	inline Runtime Running()
	{
		static const Runtime running = [] {
			const auto version = REL::Module::get().version();
			if (version == SKSE::RUNTIME_SSE_1_5_97)
				return Runtime::Se1597;
			if (version == SKSE::RUNTIME_SSE_1_6_1170)
				return Runtime::Ae1170;
			if (version == REL::Version(1, 7, 104, 0))
				return Runtime::Ae17104;
			return Runtime::Unsupported;
		}();
		return running;
	}

	inline bool Supported() { return Running() != Runtime::Unsupported; }

	/**
	 * @brief An Address Library id, and a place `at` bytes past the address it names (a hook site inside a function). Built at
	 * compile time only, and refused there when it cannot be one: an id at or above 2^20 (SE's largest is 847307) or a place 0x4000
	 * or more into a function, as a module offset pair written where an id belongs (`{ 0x14f50b0, 0x1309250 }`) would be.
	 */
	struct Id
	{
		std::uint64_t id = 0;
		std::uint32_t at = 0;

		constexpr Id() = default;
		consteval Id(std::uint64_t a_id, std::uint32_t a_at = 0) :
			id(a_id), at(a_at)
		{
			if (a_id >= (1u << 20) || a_at >= 0x4000)
				throw "Engine::Id: not an Address Library id and a place inside its function";
		}
	};

	namespace detail
	{
		/**
		 * @brief An id's module offset, looked up once: CommonLib's lookup is a binary search on SE and AE 1.6.1170, and some
		 * offsets are read per object. Every id an Offset names is in each supported runtime's database (CommonLib fails on one
		 * that is not).
		 */
		inline std::uintptr_t Resolve(std::uint64_t a_id)
		{
			constexpr std::uint64_t kCached = 1u << 20;  // above every id SE (847307) and AE 1.7.104 (565758) have
			static const auto cache = std::make_unique<std::atomic<std::uint32_t>[]>(kCached);
			if (a_id >= kCached)
				return REL::ID(a_id).offset();
			auto& slot = cache[a_id];
			std::uint32_t offset = slot.load(std::memory_order_relaxed);
			if (!offset) {
				offset = static_cast<std::uint32_t>(REL::ID(a_id).offset());
				slot.store(offset, std::memory_order_relaxed);
			}
			return offset;
		}
	}

	/**
	 * @brief An engine address on each runtime DCLF supports: an Address Library id for AE and one for SE (with the place inside the
	 * function for a site within one), and AE 1.7.104's where it moved the site or renumbered the function. An empty id is one not
	 * found for that runtime; whatever uses it must not install there (Known). On an unsupported runtime no Offset is Known.
	 */
	struct Offset
	{
		Id ae{};    // AE 1.6.1170, and 1.7.104 unless ae17 is given
		Id se{};    // SE 1.5.97
		Id ae17{};  // AE 1.7.104, where it differs from ae

		[[nodiscard]] std::uintptr_t Get() const
		{
			const Id* id = nullptr;
			switch (Running()) {
			case Runtime::Se1597:
				id = &se;
				break;
			case Runtime::Ae1170:
				id = &ae;
				break;
			case Runtime::Ae17104:
				id = ae17.id ? &ae17 : &ae;
				break;
			default:
				return 0;
			}
			return id->id ? detail::Resolve(id->id) + id->at : 0;
		}
		[[nodiscard]] bool Known() const { return Get() != 0; }
		operator std::uintptr_t() const { return Get(); }
	};

	/** @brief Whether the instruction at a_address is a CALL rel32 to a_target: a hook site is what was reverse engineered. */
	inline bool CallsTo(std::uintptr_t a_address, std::uintptr_t a_target)
	{
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
		if (bytes[0] != 0xE8)
			return false;
		std::int32_t displacement = 0;
		std::memcpy(&displacement, bytes + 1, sizeof(displacement));
		return a_address + 5 + static_cast<std::intptr_t>(displacement) == a_target;
	}

	/** @brief A module global at a_offset. */
	template <class T>
	T& Global(std::uintptr_t a_offset)
	{
		return *reinterpret_cast<T*>(REL::Module::get().base() + a_offset);
	}

	/** @brief The field of type T at a_offset into a_base. */
	template <class T>
	T& At(const void* a_base, std::size_t a_offset)
	{
		return *reinterpret_cast<T*>(const_cast<std::byte*>(static_cast<const std::byte*>(a_base)) + a_offset);
	}

	/** @brief The performance counter, for the hooks' timings. */
	inline std::int64_t Now()
	{
		LARGE_INTEGER value{};
		QueryPerformanceCounter(&value);
		return value.QuadPart;
	}

	/**
	 * @brief BSGraphics::SetDirtyStates(false): applies the engine's pending render state (targets, viewport, clears) to
	 * D3D11, as its next draw would. Render thread.
	 */
	inline void ApplyPendingState()
	{
		static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
		setDirtyStates(false);
	}

	/**
	 * @brief The engine's "no fade node": the parentless, unnamed BSFadeNode the NiPointer at 0x14332a2a0 holds (made by FUN_14147ac00 at
	 * the engine's static initialisation, released only at exit, so the pointer is immutable while any plugin runs). BSShaderProperty's
	 * constructor (0x14147b580) points every property's fadeNode at it, and the engine's readers treat it as null (GetRenderPasses
	 * 0x1414ae146, BSLightingShader::Func6 0x1414dd379, BSUtilityShader::SetupGeometry 0x1414fb720). Terrain, object LOD and references
	 * whose root is no BSFadeNode keep it.
	 */
	inline constexpr Offset kEmptyFadeNode{ { 414561 }, { 527632 } };  // NiPointer<BSFadeNode>
	inline const void* EmptyFadeNode()
	{
		return Global<const void*>(kEmptyFadeNode);
	}

	// The engine addresses DCLF reads or calls; the hooks' patch sites stay named where they are installed. The comments' addresses are AE 1.6.1170's.
	namespace Address
	{
		inline constexpr Offset kDepthAccumulator{ { 415008 }, { 528063 } };  // BSShaderAccumulator*: the main camera's depth pass (render mode 0xC)
		inline constexpr Offset kMainAccumulator{ { 415009 }, { 528064 } };  // BSShaderAccumulator*: the main camera's (render mode 0)
		inline constexpr Offset kFirstPersonAccumulator{ { 415011 }, { 528066 } };  // BSShaderAccumulator*: the first-person view's
		inline constexpr Offset kSceneListCount{ { 415016 }, { 528071 } };  // std::uint32_t: the scene lists
		inline constexpr Offset kSceneLists{ { 415017 }, { 528072 } };  // BSTArray<NiPointer<NiAVObject>>*: the scene lists
		inline constexpr Offset kExtraList{ { 415020 }, { 528075 } };  // BSTArray<NiPointer<NiAVObject>>: culled by the first list job only
		inline constexpr Offset kListCullJobs{ { 414944 }, { 527999 } };  // JobList*: _gJobList_SceneListAccumCulling (null: the list culls run inline)
		inline constexpr Offset kListProcesses{ { 415022 }, { 528077 } };  // BSGeometryListCullingProcess**: one per scene list
		inline constexpr Offset kDrawWorldEye{ { 415027 }, { 528082 } };  // Main::DrawWorld's camera position (float[3])
		inline constexpr Offset kWorldSceneGraph{ { 415032 }, { 528087 } };  // the world scene graph; its camera at +0x128
		inline constexpr Offset kSunOff{ { 415040 }, { 528095 } };  // bool: the sun does not draw (no full-frustum cull, no cascades)
		inline constexpr Offset kListJobsFinish{ { 69353 }, { 67992 } };  // the scene list jobs' finish (the job list's wait)
		inline constexpr Offset kFadeTypeDivisors{ { 390867 }, { 513143 } };  // float per LOD type (+0x153 & 0xF): the LOD factor's divisor
		inline constexpr Offset kFadeLodUpdates{ { 390865 }, { 513141 } };  // byte: the fade LOD updates are on
		inline constexpr Offset kFadesOn{ { 390866 }, { 513142 } };  // byte: BSFadeNode::OnVisible updates fades
		inline constexpr Offset kFadeOutThreshold{ { 390880 }, { 513156 } };  // float: a faded-in node fades out below this fade value
		inline constexpr Offset kFadeDistanceMult{ { 390884 }, { 513160 } };  // float: multiplies the node's near and far distances
		inline constexpr Offset kFadeSpecialA{ { 414551 }, { 527623 } };  // float: with kFadeSpecialB, OnVisible's LOD-type-6 branch (the metric override)
		inline constexpr Offset kFadeSpecialB{ { 186426 }, { 228611 } };  // float: -1.0, a constant (also the float tree LOD refills pack)
		inline constexpr Offset kFadeDefaultScale{ { 470010 }, { 232846 } };  // float: the fade scale when the type's divisor is not positive
		inline constexpr Offset kMetricScale{ { 252903 }, { 302862 } };  // float: the fade metric scale
		inline constexpr Offset kScreenDoorFades{ { 391102 }, { 513330 } };  // byte: screen-door fades are on
		inline constexpr Offset kNoSunShadowDir{ { 390962 }, { 513222 } };  // byte: GetRenderPasses gives no pass ShadowDir
		// A local shadow light's mask constants (BSUtilityShader: FUN_1414fbd30, SetupGeometry, SetupTechnique for the mask techniques).
		inline constexpr Offset kShadowBiasFactor{ { 255329 }, { 305660 } };  // float: AlphaTestRef.y = shadowBiasScale * this * kShadowBiasScale
		inline constexpr Offset kShadowBiasScale{ { 415265 }, { 528317 } };  // float
		inline constexpr Offset kShadowFadeDistance{ { 208217 }, { 262344 } };  // float: ShadowLightParam.z (the mask's fade distance squared)
		inline constexpr Offset kShadowFadeDistanceLod{ { 415264 }, { 528316 } };  // float: ShadowLightParam.z for a light with lodFade (+0x63)
		inline constexpr Offset kShadowFilterRadius{ { 391759 }, { 513773 } };  // float: ShadowSampleParam.z = this / kShadowMapResolution
		inline constexpr Offset kShadowMapResolution{ { 415268 }, { 528318 } };  // uint
		inline constexpr Offset kTreeOnVisible{ { 105604 }, { 98953 } };  // BSTreeNode::OnVisible (the height test, BSLeafAnimNode's, the LOD fix-up)
		inline constexpr Offset kFadeUpdate{ { 105486 }, { 98849 } };  // FUN_14147a160(node, fadeAmount, camera): BSFadeNode's fade state machine
		inline constexpr Offset kLodLevelUpdate{ { 105487 }, { 98850 } };  // FUN_14147a430(node, level): the leaf and tree LOD transition (+0x152)
		inline constexpr Offset kShadowSceneNode{ { 390951 }, { 513211 } };  // ShadowSceneNode*: the main one (BSShaderManager::State's first; its +0x304 the wind magnitude)
		inline constexpr Offset kFocusTargetCount{ { 414625 }, { 527703 } };  // the focus targets' count
		inline constexpr Offset kTransformToMatrix{ { 106462 }, { 99814 } };  // FUN_1414aaf10: an NiTransform as the shaders' row-major matrix
		inline constexpr Offset kLightingShader{ { 415094 }, { 528150 } };  // BSLightingShader*: the instance its constructor writes
		inline constexpr Offset kImageSpaceManager{ { 414660 }, { 527731 } };  // ImageSpaceManager*; the HDR multiplier at +0xe0
		inline constexpr Offset kHighDetailRange{ { 390958 }, { 513218 } };  // float[4]: the loaded grid's centre and half extents (FUN_141480ab0)
		inline constexpr Offset kHighDetailMargin{ { 470029 }, { 249190 } };  // float: taken off the high detail range's z and w (15)
		inline constexpr Offset kProjectedTiling{ { 391644 }, { 513700 } };  // float: ProjectedUV's first tiling (FUN_1414e00c0)
		inline constexpr Offset kProjectedTiling2{ { 391647 }, { 513702 } };  // float: its second tiling
		inline constexpr Offset kProjectedNormals{ { 391635 }, { 513694 } };  // byte: the projected-normals switch
		inline constexpr Offset kSwitchSetRevision{ { 70598 }, { 69234 } };  // FUN_140d29990(revisions, index, revID): a switch child's revision marked current
		inline constexpr Offset kMaterialRelease{ { 107720 }, { 100935 } };  // FUN_1414f7a40(manager, material): a material reference dropped (AE ID 107720)
		inline constexpr Offset kMaterialManager{ { 403555 }, { 517047 } };  // the material database (AE ID 403555)
		inline constexpr Offset kFloatControllerTargets{ { 415317 }, { 528371 } };  // std::uint32_t per type: the float controller's material float index
		inline constexpr Offset kColorControllerTargets{ { 415344 }, { 528398 } };  // std::uint32_t per type: the colour controller's material byte offset

		// The feature's hook sites (DrawcallLimitFix::Hooks::Install), each checked with CallsTo before it is patched.
		inline constexpr Offset kGeometryGroupDraw{ { 107630, 0xfd }, {} };     // a geometry group's pass list draw (FUN_1414f19d0; SE: the loop is RenderPassImmediately<1>'s)
		inline constexpr Offset kRenderPassImmediately{ { 107644 }, { 100854 } };  // what it calls: BSBatchRenderer::RenderPassImmediately
		inline constexpr Offset kUpdateEntryCall{ { 36564, 0x3e }, { 35565, 0x1e } };         // Main::Update's first call
		inline constexpr Offset kStagesReset{ { 69510 }, { 68163 } };             // what it calls
		inline constexpr Offset kCellAnimationsCall{ { 39067, 0xa2 }, { 38111, 0xaa } };      // the cell animations job's update call (FUN_1406d29d0, FUN_140640300)
		inline constexpr Offset kCellAnimationsUpdate{ { 13291 }, { 13151 } };    // what it calls: TES's animated references' update (TES, time)
		inline constexpr Offset kAnimatedObjectsCall{ { 39100, 0x32 }, { 38143, 0x32 } };     // the "Animated cell object update" job's update call (FUN_1406d3770, FUN_140640fc0)
		inline constexpr Offset kAnimatedObjectsUpdate{ { 36998 }, { 36022 } };   // what it calls: the animated objects' updates, queued on its job group
		inline constexpr Offset kAnimatedObjectsWaitCall{ { 39100, 0x52 }, { 38143, 0x52 } };  // the job's wait for its group (after the group's start)
		inline constexpr Offset kAnimatedObjectsWait{ { 69353 }, { 67992 } };     // what it calls: the job group's wait
		inline constexpr Offset kUpdateSkinInstance{ { 77461 }, { 75655 } };      // FUN_140e4ff90(skin, world): a skin instance's palette update
		inline constexpr Offset kParabolaRadius{ { 391879 }, { 513818 } };          // float: the paraboloid shadow map's radius (ParabolaParam.x's inverse)
		inline constexpr Offset kParabolaSide{ { 391880 }, { 513819 } };            // float: ParabolaParam.y
		inline constexpr Offset kDepthBiasEnabled{ { 390939 }, { 513197 } };        // byte: ToggleDepthBias (read by FUN_1414b3bb0)
		inline constexpr Offset kCameraDepthRange{ { 388951 }, { 524904 } };        // float[2]: the camera state's depth range (FUN_140e57e10)
		inline constexpr Offset kMatrixMultiply{ { 109815 }, { 102357 } };          // the engine's 4x4 multiply (out, a, b)
	}

	// NiAVObject::flags and the bits DCLF reads and writes.
	constexpr std::size_t kObjectFlags = 0xF4;
	constexpr std::uint32_t kFlagHidden = 1u << 0;        // kHidden (app-culled)
	constexpr std::uint32_t kFlagAccumulated = 1u << 26;  // kAccumulated: passed the last cull that reached it

	/**
	 * @brief A node's flags, read atomically: the engine's threads store other bits of the same dword (LOD swaps, culls, fades), so
	 * no reader may tear or cache it. Any thread; a_node must be alive.
	 */
	inline std::uint32_t FlagsOf(const void* a_node)
	{
		return std::atomic_ref<std::uint32_t>(At<std::uint32_t>(a_node, kObjectFlags)).load(std::memory_order_relaxed);
	}
	/** @brief Whether a node is app-culled (kHidden). */
	inline bool IsHidden(const void* a_node) { return (FlagsOf(a_node) & kFlagHidden) != 0; }

	// NiAVObject's vtable slot of OnVisible: the fade nodes' class is told by it (FadeState::PlanOf).
	constexpr std::size_t kOnVisibleSlot = 0x34;

	/**
	 * @brief The tree's LOD switch (+0x180), whose selection changes by switch event (kFadeRootTreeLod), or null: a node whose
	 * OnVisible is not BSTreeNode's has none. The capture reads it; the fade roots key their switch events by it.
	 */
	inline const RE::NiAVObject* TreeLodSwitch(const RE::NiAVObject& a_node)
	{
		const auto onVisible = (*reinterpret_cast<const std::uintptr_t* const*>(&a_node))[kOnVisibleSlot];
		if (onVisible != REL::Module::get().base() + Address::kTreeOnVisible)
			return nullptr;
		return At<const RE::NiAVObject*>(&a_node, 0x180);
	}

	/**
	 * @brief A ShadowSceneNode's object root: its child 3, the cells' parent (DrawWorld_BuildSceneLists' walk), or null. The tree must be
	 * held (the render or main thread inside the frame).
	 */
	inline RE::NiAVObject* ObjectRoot(const RE::NiNode& a_sceneNode)
	{
		const auto& children = a_sceneNode.GetChildren();
		return children.free_idx() > 3 ? children[3].get() : nullptr;
	}

	/** @brief An object root's cell nodes from child a_first (children 0 and 1 are whole entries), those not hidden: a_visit(cell). */
	template <class Visit>
	void ForEachCell(const RE::NiAVObject* a_objectRoot, std::uint16_t a_first, Visit&& a_visit)
	{
		auto* root = a_objectRoot ? const_cast<RE::NiAVObject*>(a_objectRoot)->AsNode() : nullptr;
		if (!root || IsHidden(root))
			return;
		const auto& cells = root->GetChildren();
		for (std::uint16_t i = a_first; i < cells.free_idx(); ++i)
			if (auto* cell = cells[i] ? cells[i]->AsNode() : nullptr; cell && !IsHidden(cell))
				a_visit(*cell);
	}

	/** @brief A cell node's category nodes (its children) not hidden: a_visit(index, node). */
	template <class Visit>
	void ForEachCategory(RE::NiNode& a_cell, Visit&& a_visit)
	{
		const auto& categories = a_cell.GetChildren();
		for (std::uint16_t j = 0; j < categories.free_idx(); ++j)
			if (auto* category = categories[j] ? categories[j]->AsNode() : nullptr; category && !IsHidden(category))
				a_visit(j, *category);
	}

	/** @brief The loaded cells TES draws: the interior cell, else every cell of the grid (empty slots passed as null). */
	template <class Visit>
	void ForEachLoadedCell(const RE::TES& a_tes, Visit&& a_visit)
	{
		if (a_tes.interiorCell) {
			a_visit(a_tes.interiorCell);
		} else if (const auto* grid = a_tes.gridCells) {
			const std::uint32_t count = grid->length * grid->length;
			for (std::uint32_t i = 0; i < count; ++i)
				a_visit(grid->cells[i]);
		}
	}

	/** @brief Whether an NiObject's RTTI is named a_name (its exact class). */
	inline bool RttiIs(const void* a_object, const char* a_name)
	{
		const auto* rtti = static_cast<const RE::NiObject*>(a_object)->GetRTTI();
		return rtti && rtti->name && std::strcmp(rtti->name, a_name) == 0;
	}

	/** @brief Whether an object's vtable is a_vtable (its exact class; the caller keeps the vtable's address, a relocation lookup). */
	inline bool VtableIs(const void* a_object, std::uintptr_t a_vtable)
	{
		return a_object && *static_cast<const std::uintptr_t*>(a_object) == a_vtable;
	}

	/**
	 * @brief BSLightingShaderMaterial and its 13 subclasses' vtables, in this order: Base, Envmap, Eye, Glowmap, Parallax, ParallaxOcc,
	 * Facegen, FacegenTint, HairTint, Landscape, LODLandscape, Snow, MultiLayerParallax (the material write hooks' and the vanilla
	 * material port's classes).
	 */
	inline const std::array<REL::VariantID, 14>& LightingMaterialVtables()
	{
		static const std::array<REL::VariantID, 14> vtables{ RE::VTABLE_BSLightingShaderMaterial[0], RE::VTABLE_BSLightingShaderMaterialBase[0],
			RE::VTABLE_BSLightingShaderMaterialEnvmap[0], RE::VTABLE_BSLightingShaderMaterialEye[0], RE::VTABLE_BSLightingShaderMaterialGlowmap[0],
			RE::VTABLE_BSLightingShaderMaterialParallax[0], RE::VTABLE_BSLightingShaderMaterialParallaxOcc[0], RE::VTABLE_BSLightingShaderMaterialFacegen[0],
			RE::VTABLE_BSLightingShaderMaterialFacegenTint[0], RE::VTABLE_BSLightingShaderMaterialHairTint[0], RE::VTABLE_BSLightingShaderMaterialLandscape[0],
			RE::VTABLE_BSLightingShaderMaterialLODLandscape[0], RE::VTABLE_BSLightingShaderMaterialSnow[0], RE::VTABLE_BSLightingShaderMaterialMultiLayerParallax[0] };
		return vtables;
	}

	// BSShaderProperty's light data, and in it the mask of the shadow lights the geometry is registered with.
	constexpr std::size_t kPropertyLightData = 0x70;
	constexpr std::size_t kLightDataActiveMask = 0x1C;

	/**
	 * @brief An NiSwitchNode's own fields, read at their AE offsets (NiSwitchNode::OnVisible, AE 140d29700):
	 * CommonLib declares them after NiNode, whose declared size in a multi-runtime build is VR's, so its members
	 * read the wrong memory.
	 */
	struct SwitchState
	{
		std::uint16_t flags = 0;  // bit 0: the update pass updates only the selected child
		std::int32_t index = -1;
		std::uint32_t revID = 0;
		const std::uint32_t* childRevID = nullptr;
		std::uint16_t childRevCapacity = 0;
	};
	/**
	 * @brief The child a switch selects: children[index], null when the index or its revision is out of range
	 * (bounded by capacity, not size: a Gamebryo array is indexed by slot). Current when childRevID[index] ==
	 * revID; otherwise NiSwitchNode::OnVisible brings the child up to date before culling it.
	 */
	struct SwitchSelection
	{
		RE::NiAVObject* child = nullptr;
		std::uint16_t index = 0;
		bool current = false;
	};

	inline SwitchState ReadSwitch(const RE::NiSwitchNode& a_switch)
	{
		const auto* base = reinterpret_cast<const std::byte*>(&a_switch);
		SwitchState state;
		state.flags = *reinterpret_cast<const std::uint16_t*>(base + 0x128);
		state.index = *reinterpret_cast<const std::int32_t*>(base + 0x12C);
		state.revID = *reinterpret_cast<const std::uint32_t*>(base + 0x134);
		// childRevID, an NiTPrimitiveArray at +0x138: its data pointer at +0x8, its capacity at +0x10.
		state.childRevID = *reinterpret_cast<const std::uint32_t* const*>(base + 0x140);
		state.childRevCapacity = *reinterpret_cast<const std::uint16_t*>(base + 0x148);
		return state;
	}

	inline SwitchSelection SelectionOf(const RE::NiSwitchNode& a_switch, const SwitchState& a_state)
	{
		const auto& children = a_switch.GetChildren();
		if (a_state.index < 0 || static_cast<std::uint32_t>(a_state.index) >= children.capacity() || !a_state.childRevID ||
			static_cast<std::uint32_t>(a_state.index) >= a_state.childRevCapacity)
			return {};
		const auto index = static_cast<std::uint16_t>(a_state.index);
		return { children[index].get(), index, a_state.childRevID[index] == a_state.revID };
	}

	inline bool SwitchSelects(const RE::NiSwitchNode& a_switch, const RE::NiAVObject* a_child)
	{
		// NiSwitchNode::OnVisible (AE 140d29700) culls children[index] and nothing else. A child that has
		// become the selected one since the last update pass is brought up to date there, in the cull
		// (childRevID[index] != revID), which is after this walk read its transforms: leave it native for
		// that frame.
		const auto selection = SelectionOf(a_switch, ReadSwitch(a_switch));
		return selection.child && selection.child == a_child && selection.current;
	}
}
