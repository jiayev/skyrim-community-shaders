#pragma once

// Raw access to the engine, for the hooks and the engine state DCLF reads without CommonLib declarations. Every offset here and at
// the hooks is module-relative (as REL::Offset takes them) and an Offset: AE 1.6.1170's and SE 1.5.97's.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace DCLF::Engine
{
	/**
	 * @brief An engine address as a module offset on each runtime DCLF supports: AE 1.6.1170's and SE 1.5.97's. 0 is one not found
	 * for that runtime; whatever uses it must not install there (Known).
	 */
	struct Offset
	{
		std::uintptr_t ae = 0;
		std::uintptr_t se = 0;

		[[nodiscard]] std::uintptr_t Get() const noexcept { return REL::Module::IsSE() ? se : ae; }
		[[nodiscard]] bool Known() const noexcept { return Get() != 0; }
		operator std::uintptr_t() const noexcept { return Get(); }
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
	inline constexpr Offset kEmptyFadeNode{ 0x332a2a0, 0x31d0da8 };  // NiPointer<BSFadeNode>
	inline const void* EmptyFadeNode()
	{
		return Global<const void*>(kEmptyFadeNode);
	}

	// The engine addresses DCLF reads or calls (module offsets, AE 1.6.1170 and SE 1.5.97); the hooks' patch sites stay named where they are installed.
	namespace Address
	{
		inline constexpr Offset kDepthAccumulator{ 0x338c828, 0x32333f8 };  // BSShaderAccumulator*: the main camera's depth pass (render mode 0xC)
		inline constexpr Offset kMainAccumulator{ 0x338c830, 0x3233400 };  // BSShaderAccumulator*: the main camera's (render mode 0)
		inline constexpr Offset kFirstPersonAccumulator{ 0x338c840, 0x3233410 };  // BSShaderAccumulator*: the first-person view's
		inline constexpr Offset kSceneListCount{ 0x338c868, 0x3233438 };  // std::uint32_t: the scene lists
		inline constexpr Offset kSceneLists{ 0x338c870, 0x3233440 };  // BSTArray<NiPointer<NiAVObject>>*: the scene lists
		inline constexpr Offset kExtraList{ 0x338c888, 0x3233458 };  // BSTArray<NiPointer<NiAVObject>>: culled by the first list job only
		inline constexpr Offset kListCullJobs{ 0x338c630, 0x3233200 };  // JobList*: _gJobList_SceneListAccumCulling (null: the list culls run inline)
		inline constexpr Offset kListProcesses{ 0x338c8a0, 0x3233470 };  // BSGeometryListCullingProcess**: one per scene list
		inline constexpr Offset kDrawWorldEye{ 0x338c8c0, 0x3233490 };  // Main::DrawWorld's camera position (float[3])
		inline constexpr Offset kWorldSceneGraph{ 0x338c8e8, 0x32334b8 };  // the world scene graph; its camera at +0x128
		inline constexpr Offset kSunOff{ 0x338c911, 0x32334e1 };  // bool: the sun does not draw (no full-frustum cull, no cascades)
		inline constexpr Offset kListJobsFinish{ 0xcf6810, 0xc31ea0 };  // the scene list jobs' finish (the job list's wait)
		inline constexpr Offset kFadeTypeDivisors{ 0x2032e00, 0x1e0dc60 };  // float per LOD type (+0x153 & 0xF): the LOD factor's divisor
		inline constexpr Offset kFadeLodUpdates{ 0x2032dfc, 0x1e0dc5c };  // byte: the fade LOD updates are on
		inline constexpr Offset kFadesOn{ 0x2032dfd, 0x1e0dc5d };  // byte: BSFadeNode::OnVisible updates fades
		inline constexpr Offset kFadeOutThreshold{ 0x2032e38, 0x1e0dc98 };  // float: a faded-in node fades out below this fade value
		inline constexpr Offset kFadeDistanceMult{ 0x2032e48, 0x1e0dca8 };  // float: multiplies the node's near and far distances
		inline constexpr Offset kFadeSpecialA{ 0x332a254, 0x31d0d5c };  // float: with kFadeSpecialB, OnVisible's LOD-type-6 branch (the metric override)
		inline constexpr Offset kFadeSpecialB{ 0x1769578, 0x152259c };  // float: -1.0, a constant (also the float tree LOD refills pack)
		inline constexpr Offset kFadeDefaultScale{ 0x1ad2840, 0x1548b50 };  // float: the fade scale when the type's divisor is not positive
		inline constexpr Offset kMetricScale{ 0x1aa6300, 0x184cec0 };  // float: the fade metric scale
		inline constexpr Offset kScreenDoorFades{ 0x2033468, 0x1e0e2b8 };  // byte: screen-door fades are on
		inline constexpr Offset kNoSunShadowDir{ 0x20330a4, 0x1e0df14 };  // byte: GetRenderPasses gives no pass ShadowDir
		// A local shadow light's mask constants (BSUtilityShader: FUN_1414fbd30, SetupGeometry, SetupTechnique for the mask techniques).
		inline constexpr Offset kShadowBiasFactor{ 0x1ac1d48, 0x1868784 };  // float: AlphaTestRef.y = shadowBiasScale * this * kShadowBiasScale
		inline constexpr Offset kShadowBiasScale{ 0x33dcfb0, 0x3283b8c };  // float
		inline constexpr Offset kShadowFadeDistance{ 0x18af8b0, 0x1667cd0 };  // float: ShadowLightParam.z (the mask's fade distance squared)
		inline constexpr Offset kShadowFadeDistanceLod{ 0x33dcfac, 0x3283b88 };  // float: ShadowLightParam.z for a light with lodFade (+0x63)
		inline constexpr Offset kShadowFilterRadius{ 0x20358f0, 0x1e10670 };  // float: ShadowSampleParam.z = this / kShadowMapResolution
		inline constexpr Offset kShadowMapResolution{ 0x33dcfc0, 0x3283b90 };  // uint
		inline constexpr Offset kTreeOnVisible{ 0x147d3c0, 0x1293170 };  // BSTreeNode::OnVisible (the height test, BSLeafAnimNode's, the LOD fix-up)
		inline constexpr Offset kFadeUpdate{ 0x147a160, 0x1290050 };  // FUN_14147a160(node, fadeAmount, camera): BSFadeNode's fade state machine
		inline constexpr Offset kLodLevelUpdate{ 0x147a430, 0x1290370 };  // FUN_14147a430(node, level): the leaf and tree LOD transition (+0x152)
		inline constexpr Offset kShadowSceneNode{ 0x2033060, 0x1e0ded0 };  // ShadowSceneNode*: the main one (BSShaderManager::State's first; its +0x304 the wind magnitude)
		inline constexpr Offset kFocusTargetCount{ 0x332a498, 0x31d0fb8 };  // the focus targets' count
		inline constexpr Offset kTransformToMatrix{ 0x14aaf10, 0x12c3440 };  // FUN_1414aaf10: an NiTransform as the shaders' row-major matrix
		inline constexpr Offset kLightingShader{ 0x338ca00, 0x32335d8 };  // BSLightingShader*: the instance its constructor writes
		inline constexpr Offset kImageSpaceManager{ 0x332a6a0, 0x31d11a0 };  // ImageSpaceManager*; the HDR multiplier at +0xe0
		inline constexpr Offset kHighDetailRange{ 0x2033094, 0x1e0df04 };  // float[4]: the loaded grid's centre and half extents (FUN_141480ab0)
		inline constexpr Offset kHighDetailMargin{ 0x1ad28d0, 0x15e29b4 };  // float: taken off the high detail range's z and w (15)
		inline constexpr Offset kProjectedTiling{ 0x2035560, 0x1e10300 };  // float: ProjectedUV's first tiling (FUN_1414e00c0)
		inline constexpr Offset kProjectedTiling2{ 0x2035578, 0x1e10318 };  // float: its second tiling
		inline constexpr Offset kProjectedNormals{ 0x2035518, 0x1e102b8 };  // byte: the projected-normals switch
		inline constexpr Offset kSwitchSetRevision{ 0xd29990, 0xc63cf0 };  // FUN_140d29990(revisions, index, revID): a switch child's revision marked current
		inline constexpr Offset kMaterialRelease{ 0x14f7a40, 0x130be10 };  // FUN_1414f7a40(manager, material): a material reference dropped (AE ID 107720)
		inline constexpr Offset kMaterialManager{ 0x3187758, 0x2f27160 };  // the material database (AE ID 403555)
		inline constexpr Offset kFloatControllerTargets{ 0x35ef210, 0x3495e00 };  // std::uint32_t per type: the float controller's material float index
		inline constexpr Offset kColorControllerTargets{ 0x35ef298, 0x3495e88 };  // std::uint32_t per type: the colour controller's material byte offset

		// The feature's hook sites (DrawcallLimitFix::Hooks::Install), each checked with CallsTo before it is patched.
		inline constexpr Offset kGeometryGroupDraw{ 0x14f1acd, 0 };     // a geometry group's pass list draw (FUN_1414f19d0; SE: the loop is RenderPassImmediately<1>'s)
		inline constexpr Offset kRenderPassImmediately{ 0x14f3dc0, 0x1308440 };  // what it calls: BSBatchRenderer::RenderPassImmediately
		inline constexpr Offset kUpdateEntryCall{ 0x645ede, 0x5b300e };         // Main::Update's first call
		inline constexpr Offset kStagesReset{ 0xcfd280, 0xc37fa0 };             // what it calls
		inline constexpr Offset kCellAnimationsCall{ 0x6d2a72, 0x6403aa };      // the cell animations job's update call (FUN_1406d29d0, FUN_140640300)
		inline constexpr Offset kCellAnimationsUpdate{ 0x19c0f0, 0x152c90 };    // what it calls: TES's animated references' update (TES, time)
		inline constexpr Offset kUpdateSkinInstance{ 0xe4ff90, 0xd74f70 };      // FUN_140e4ff90(skin, world): a skin instance's palette update
		inline constexpr Offset kParabolaRadius{ 0x2035df8, 0x1e10b78 };          // float: the paraboloid shadow map's radius (ParabolaParam.x's inverse)
		inline constexpr Offset kParabolaSide{ 0x2035dfc, 0x1e10b7c };            // float: ParabolaParam.y
		inline constexpr Offset kDepthBiasEnabled{ 0x2032ff6, 0x1e0de3a };        // byte: ToggleDepthBias (read by FUN_1414b3bb0)
		inline constexpr Offset kCameraDepthRange{ 0x202b130, 0x3028470 };        // float[2]: the camera state's depth range (FUN_140e57e10)
		inline constexpr Offset kMatrixMultiply{ 0x153d3c8, 0x134c1e2 };          // the engine's 4x4 multiply (out, a, b)
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
