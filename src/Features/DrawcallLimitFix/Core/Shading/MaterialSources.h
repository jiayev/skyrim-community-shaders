#pragma once

// MaterialSources: the rules that keep a material record's frame-sourced parts current (signatures, TexcoordOffset, t11) without
// re-evaluating it. Pure functions; the scene-work thread applies them to the tables' records.

#include <array>
#include <cstdint>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Core/Shading/MaterialWrites.h"

struct ID3D11ShaderResourceView;

namespace DCLF
{
	struct MaterialRecord;
	namespace MaterialPort
	{
		struct MaterialFrame;
	}

	/**
	 * @brief Where every value of a material record comes from, and when it changes (engine notes,
	 * "BSLightingShader::SetupMaterial: where every material constant comes from").
	 *
	 * A record is what SetupMaterial writes for one (material, pass descriptor). Its inputs are of four kinds,
	 * and each is kept current by its own deterministic rule rather than by noticing that a record went stale:
	 *
	 * - The material's own fields. They change only when something writes the material: a shader-property
	 *   controller (BSLightingShaderPropertyFloatController / ColorController::Update), or the material's
	 *   in-place rewrites (CopyMembers, OnLoadTextureSet, ClearTextures, ReceiveValuesFromRootMaterial; CS's
	 *   PBR materials report theirs through NoteWritten). Each write captures the material for the scene work
	 *   (MaterialPort::PushCapture), which evaluates the written material's records again from the capture
	 *   (Coordinator::RefreshMaterialRecords).
	 * - TexcoordOffset (VS 11): the material's two texture-transform buffers, read at the index the engine
	 *   flips every frame (Main::Update). A controller's write captures the material; the scene work watches it
	 *   across the buffer flip, writing the frame's buffer (FrameGlobals' textureTransformBuffer) from the
	 *   capture's two until they agree.
	 * - The shader object and engine globals: PS 6, SnowRimLightParameters, CharacterLightParams,
	 *   LODTexParams.z, LandscapeTexture5to6IsSnow.zw. The same for every material that writes them, so the scene
	 *   work takes them from one port evaluation per signature against the frame's sources (Signature,
	 *   ApplyFrameComponents). IBLParams is the shader object's too, but no Lighting stage reads it: the record
	 *   keeps its first evaluation's.
	 * - Render targets: the character light's t11, re-picked every frame. It is the frame's (FrameCharacterLight), not the
	 *   records': a character-light pass's record keeps t11's modes and no view, and the frame record gives the view.
	 */
	namespace MaterialSources
	{
		/**
		 * @brief Which frame-sourced groups a pass descriptor's SetupMaterial writes. Records with the same
		 * signature hold the same values in those groups.
		 */
		std::uint32_t Signature(std::uint32_t a_passDescriptor);

		/** @brief The frame-sourced PS floats (positions in the PS layout): what the frame components' copies take. */
		const std::vector<std::uint32_t>& FramePSFloats();
		/** @brief TexcoordOffset's floats (positions in the VS layout): what the frame components' copies take. */
		const std::vector<std::uint32_t>& FrameVSFloats();

		/**
		 * @brief Copies the frame-sourced components of a live evaluation into a record of the same signature:
		 * every listed float the record has written, and t11's modes in a character-light pass (its view is the frame's:
		 * the record holds none). True when t11 changed (the record's textures are part of its version; its floats are
		 * repacked every build).
		 */
		bool ApplyFrameComponents(const MaterialRecord& a_live, MaterialRecord& a_record, std::uint32_t a_passDescriptor, bool* a_floatsChanged = nullptr);

		/** @brief A record as records hold it: no view at a character-light pass's t11 (its modes stay). */
		void StripFrameViews(MaterialRecord& a_record, std::uint32_t a_passDescriptor);
		/** @brief Whether a pass descriptor's t11 is the character light's (the frame's view). */
		bool FrameCharacterLight(std::uint32_t a_passDescriptor);
		/** @brief The character light's view a live evaluation of a character-light pass bound at t11, or null. */
		ID3D11ShaderResourceView* CharacterLightView(const MaterialRecord& a_live, std::uint32_t a_passDescriptor);

		/**
		 * @brief Pure: the character light's view (a character-light pass's t11) the frame's sources give: the render target vanilla
		 * SetupMaterial binds, else TruePBR's; null while neither has one (-1 while a cell loads).
		 */
		ID3D11ShaderResourceView* FrameCharacterLightView(const MaterialPort::MaterialFrame& a_frame);

		/** @brief Copies the frame-sourced components (floats, and t11 whole: written or not, its view and modes) of a_from over a_to, for comparisons. */
		void CopyFrameComponents(const MaterialRecord& a_from, MaterialRecord& a_to, std::uint32_t a_passDescriptor);
		/**
		 * @brief Copies the floats a record keeps from its first evaluation (IBLParams, PS 29: the shader object's, drifting with the time
		 * of day, read by no Lighting stage) of a_from over a_to: a rewrite keeps them, a comparison leaves them out.
		 */
		void KeepUnreadFloats(const MaterialRecord& a_from, MaterialRecord& a_to);
	}
}
