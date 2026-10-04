#pragma once

#include <cstdint>

namespace DCLF::ReflectionFaces
{
	/*
	 * The water reflection's cube map faces (AE 1.6.1170; skyrim-engine-notes.md, "Water reflections: the cube map";
	 * dclf-lod.md, "Water reflections"). TESWaterReflections::Update renders them through BSCubeMapCamera's vfunc 0x35
	 * (0x1414ed4e0, a jump to FUN_1414ed920), a face mask a call, on the render thread. Each face binds the cube target's face
	 * and depth target 6, clears them, culls the camera's roots into its accumulator (+0x1A0) and renders it.
	 */

	/** @brief Hooks the face render (always: what is drawn inside it is decided per frame). */
	void Install();

	/** @brief Render thread: whether a face render is running now, and its camera and face mask. */
	bool InFace();
	const RE::NiCamera* Camera();
	std::uint32_t FaceMask();
	/** @brief The face render's mode is the plain one (0): no silhouettes, the sky not suppressed (PassCapture withholds only then). */
	bool Plain();

	/** @brief Called on the render thread after every face render (the reflection census, TEMP). */
	using AfterFaces = void (*)(std::uint32_t a_faceMask);
	void SetAfterFaces(AfterFaces a_callback);

	/**
	 * @brief Called on the render thread after the engine's draws of each face (its accumulator's render, FUN_1414a90f0 at
	 * 0x1414edbf8), with the face's targets still bound: DCLF's capture of the face (IndirectDraws::CaptureReflectionFace).
	 */
	using AfterFaceDraws = void (*)();
	void SetAfterFaceDraws(AfterFaceDraws a_callback);
}
