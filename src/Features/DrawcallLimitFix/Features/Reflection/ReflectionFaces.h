#pragma once

// The water reflection's engine hooks (Features/Reflection's early hooks): render thread. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Views.h"

#include <cstdint>

namespace RE
{
	class NiCamera;
}

namespace DCLF::Features::Reflection::ReflectionFaces
{
	/*
	 * The water reflection's cube map faces (AE 1.6.1170; skyrim-engine-notes.md, "Water reflections: the cube map";
	 * dclf-architecture.md, "Water reflections"). TESWaterReflections::Update (0x140520570, called at 0x14052289c) adds the cube camera's
	 * roots (FUN_1405275e0: LOD land, LOD objects and LOD trees under their bReflectLOD* switches, then the sky) and renders its
	 * faces through BSCubeMapCamera's vfunc 0x35 (0x1414ed4e0, a jump to FUN_1414ed920), a face mask a call, on the render
	 * thread. Each face binds the cube target's face and depth target 6, clears them, orients the camera to the face
	 * (FUN_1414ed500, at 0x1414eda00), culls the camera's roots into its accumulator (+0x1A0) and renders it.
	 */

	/**
	 * @brief Hooks the update, the LOD add-roots and the face render (always: what is drawn inside them is decided per frame). The face
	 * render names the cube camera's accumulators as the view's renderers and its plain mode as their withholding
	 * (PassCapture::SetViewRenderers, SetViewWithholdActive).
	 */
	void Install();

	/** @brief Render thread: whether a face render is running, and its face mask. */
	bool InFace();
	std::uint32_t FaceMask();
	/** @brief The face render's mode is the plain one (0): no silhouettes, the sky not suppressed (PassCapture withholds only then). */
	bool Plain();

	/**
	 * @brief Called on the render thread once the engine has oriented the camera to a face (FUN_1414ed500), before the face's cull
	 * and draws: DCLF's view of the face (Reflection::FaceOriented).
	 */
	using FaceOriented = void (*)(const RE::NiCamera& a_camera, std::uint32_t a_face);
	void SetFaceOriented(FaceOriented a_callback);

	/**
	 * @brief Called on the render thread after the engine's draws of each face (its accumulator's render, FUN_1414a90f0 at
	 * 0x1414edbf8), with the face's targets still bound: the observers' capture of the face (Reflection::FaceDrawn).
	 */
	using AfterFaceDraws = void (*)();
	void SetAfterFaceDraws(AfterFaceDraws a_callback);

	/**
	 * @brief The cube camera's LOD roots DCLF draws in this update's faces (no face cull of them), asked once an update before
	 * the add-roots with whether its face renders are plain: kLodRoots (LOD land and LOD objects: the reflection phase), kTreeRoot
	 * (LOD trees: the faces' tree LOD). Their add-root calls are skipped; the sky's stays. The values are Api::ViewRootBit's (Core's:
	 * IndirectDraws::ViewLodRootsOwned and the residue watch name them).
	 */
	enum RootBits : std::uint32_t
	{
		kLodRoots = Api::ViewRootBit(Api::ViewRoot::LodLandAndObjects),
		kTreeRoot = Api::ViewRootBit(Api::ViewRoot::LodTrees),
	};
	using RootsOwned = std::uint32_t (*)(bool a_plain);
	void SetRootsOwned(RootsOwned a_callback);
	/** @brief Since the last call: updates, and those whose LOD roots and tree root were not added. */
	struct RootStats
	{
		std::uint32_t updates = 0, lodSkipped = 0, treeSkipped = 0;
	};
	RootStats TakeRootStats();
}
