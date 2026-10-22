#pragma once

// Raw access to the engine, for the hooks and the engine state DCLF reads without CommonLib declarations. DCLF runs on
// AE 1.6.1170 only: every offset here and at the hooks is that runtime's (module-relative, as REL::Offset takes them).

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace DCLF::Engine
{
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
	inline const void* EmptyFadeNode()
	{
		return Global<const void*>(0x332a2a0);
	}

	// The engine addresses DCLF reads or calls (module offsets, AE 1.6.1170); the hooks' patch sites stay named where they are installed.
	namespace Address
	{
		inline constexpr std::uintptr_t kDepthAccumulator = 0x338c828;  // BSShaderAccumulator*: the main camera's depth pass (render mode 0xC)
		inline constexpr std::uintptr_t kMainAccumulator = 0x338c830;  // BSShaderAccumulator*: the main camera's (render mode 0)
		inline constexpr std::uintptr_t kFirstPersonAccumulator = 0x338c840;  // BSShaderAccumulator*: the first-person view's
		inline constexpr std::uintptr_t kSceneListCount = 0x338c868;  // std::uint32_t: the scene lists
		inline constexpr std::uintptr_t kSceneLists = 0x338c870;  // BSTArray<NiPointer<NiAVObject>>*: the scene lists
		inline constexpr std::uintptr_t kExtraList = 0x338c888;  // BSTArray<NiPointer<NiAVObject>>: culled by the first list job only
		inline constexpr std::uintptr_t kListCullJobs = 0x338c630;  // JobList*: _gJobList_SceneListAccumCulling (null: the list culls run inline)
		inline constexpr std::uintptr_t kListProcesses = 0x338c8a0;  // BSGeometryListCullingProcess**: one per scene list
		inline constexpr std::uintptr_t kDrawWorldEye = 0x338c8c0;  // Main::DrawWorld's camera position (float[3])
		inline constexpr std::uintptr_t kWorldSceneGraph = 0x338c8e8;  // the world scene graph; its camera at +0x128
		inline constexpr std::uintptr_t kSunOff = 0x338c911;  // bool: the sun does not draw (no full-frustum cull, no cascades)
		inline constexpr std::uintptr_t kListJobsFinish = 0xcf6810;  // the scene list jobs' finish (the job list's wait)
		inline constexpr std::uintptr_t kFadeTypeDivisors = 0x2032e00;  // float per LOD type (+0x153 & 0xF): the LOD factor's divisor
		inline constexpr std::uintptr_t kFadeLodUpdates = 0x2032dfc;  // byte: the fade LOD updates are on
		inline constexpr std::uintptr_t kFadesOn = 0x2032dfd;  // byte: BSFadeNode::OnVisible updates fades
		inline constexpr std::uintptr_t kFadeOutThreshold = 0x2032e38;  // float: a faded-in node fades out below this fade value
		inline constexpr std::uintptr_t kFadeDistanceMult = 0x2032e48;  // float: multiplies the node's near and far distances
		inline constexpr std::uintptr_t kFadeSpecialA = 0x332a254;  // float: with kFadeSpecialB, OnVisible's LOD-type-6 branch (the metric override)
		inline constexpr std::uintptr_t kFadeSpecialB = 0x1769578;  // float: 1.0 unless overridden (also the float tree LOD refills pack)
		inline constexpr std::uintptr_t kFadeDefaultScale = 0x1ad2840;  // float: the fade scale when the type's divisor is not positive
		inline constexpr std::uintptr_t kMetricScale = 0x1aa6300;  // float: the fade metric scale
		inline constexpr std::uintptr_t kScreenDoorFades = 0x2033468;  // byte: screen-door fades are on
		inline constexpr std::uintptr_t kNoSunShadowDir = 0x20330a4;  // byte: GetRenderPasses gives no pass ShadowDir
		// A local shadow light's mask constants (BSUtilityShader: FUN_1414fbd30, SetupGeometry, SetupTechnique for the mask techniques).
		inline constexpr std::uintptr_t kShadowBiasFactor = 0x1ac1d48;  // float: AlphaTestRef.y = shadowBiasScale * this * kShadowBiasScale
		inline constexpr std::uintptr_t kShadowBiasScale = 0x33dcfb0;  // float
		inline constexpr std::uintptr_t kShadowFadeDistance = 0x18af8b0;  // float: ShadowLightParam.z (the mask's fade distance squared)
		inline constexpr std::uintptr_t kShadowFadeDistanceLod = 0x33dcfac;  // float: ShadowLightParam.z for a light with lodFade (+0x63)
		inline constexpr std::uintptr_t kShadowFilterRadius = 0x20358f0;  // float: ShadowSampleParam.z = this / kShadowMapResolution
		inline constexpr std::uintptr_t kShadowMapResolution = 0x33dcfc0;  // uint
		inline constexpr std::uintptr_t kTreeOnVisible = 0x147d3c0;  // BSTreeNode::OnVisible (the height test, BSLeafAnimNode's, the LOD fix-up)
		inline constexpr std::uintptr_t kFadeUpdate = 0x147a160;  // FUN_14147a160(node, fadeAmount, camera): BSFadeNode's fade state machine
		inline constexpr std::uintptr_t kLodLevelUpdate = 0x147a430;  // FUN_14147a430(node, level): the leaf and tree LOD transition (+0x152)
		inline constexpr std::uintptr_t kShadowSceneNode = 0x2033060;  // ShadowSceneNode*: the main one (BSShaderManager::State's first; its +0x304 the wind magnitude)
		inline constexpr std::uintptr_t kFocusTargetCount = 0x332a498;  // the focus targets' count
		inline constexpr std::uintptr_t kTransformToMatrix = 0x14aaf10;  // FUN_1414aaf10: an NiTransform as the shaders' row-major matrix
		inline constexpr std::uintptr_t kLightingShader = 0x338ca00;  // BSLightingShader*: the instance its constructor writes
		inline constexpr std::uintptr_t kImageSpaceManager = 0x332a6a0;  // ImageSpaceManager*; the HDR multiplier at +0xe0
		inline constexpr std::uintptr_t kHighDetailRange = 0x2033094;  // float[4]: the loaded grid's centre and half extents (FUN_141480ab0)
		inline constexpr std::uintptr_t kHighDetailMargin = 0x1ad28d0;  // float: taken off the high detail range's z and w (15)
		inline constexpr std::uintptr_t kProjectedTiling = 0x2035560;  // float: ProjectedUV's first tiling (FUN_1414e00c0)
		inline constexpr std::uintptr_t kProjectedTiling2 = 0x2035578;  // float: its second tiling
		inline constexpr std::uintptr_t kProjectedNormals = 0x2035518;  // byte: the projected-normals switch
		inline constexpr std::uintptr_t kSwitchSetRevision = 0xd29990;  // FUN_140d29990(revisions, index, revID): a switch child's revision marked current
		inline constexpr std::uintptr_t kMaterialRelease = 0x14f7a40;  // FUN_1414f7a40(manager, material): a material reference dropped (AE ID 107720)
		inline constexpr std::uintptr_t kMaterialManager = 0x3187758;  // the material database (AE ID 403555)
		inline constexpr std::uintptr_t kFloatControllerTargets = 0x35ef210;  // std::uint32_t per type: the float controller's material float index
		inline constexpr std::uintptr_t kColorControllerTargets = 0x35ef298;  // std::uint32_t per type: the colour controller's material byte offset

		// The feature's hook sites (DrawcallLimitFix::Hooks::Install), each checked with CallsTo before it is patched.
		inline constexpr std::uintptr_t kGeometryGroupDraw = 0x14f1acd;     // a geometry group's pass list draw (FUN_1414f19d0)
		inline constexpr std::uintptr_t kRenderPassImmediately = 0x14f3dc0;  // what it calls: BSBatchRenderer::RenderPassImmediately
		inline constexpr std::uintptr_t kUpdateEntryCall = 0x645ede;         // Main::Update's first call
		inline constexpr std::uintptr_t kStagesReset = 0xcfd280;             // what it calls
		inline constexpr std::uintptr_t kUpdateSkinInstance = 0xe4ff90;      // FUN_140e4ff90(skin, world): a skin instance's palette update
		inline constexpr std::uintptr_t kParabolaRadius = 0x2035df8;          // float: the paraboloid shadow map's radius (ParabolaParam.x's inverse)
		inline constexpr std::uintptr_t kParabolaSide = 0x2035dfc;            // float: ParabolaParam.y
		inline constexpr std::uintptr_t kDepthBiasEnabled = 0x2032ff6;        // byte: ToggleDepthBias (read by FUN_1414b3bb0)
		inline constexpr std::uintptr_t kCameraDepthRange = 0x202b130;        // float[2]: the camera state's depth range (FUN_140e57e10)
		inline constexpr std::uintptr_t kMatrixMultiply = 0x153d3c8;          // the engine's 4x4 multiply (out, a, b)
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
