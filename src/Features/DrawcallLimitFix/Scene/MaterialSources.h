#pragma once

#include <cstdint>
#include <vector>

#include <ankerl/unordered_dense.h>

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF
{
	struct MaterialRecord;

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
	 *   PBR materials report theirs through NoteWritten). Each write is an event; the accumulate phase
	 *   re-evaluates exactly the written materials.
	 * - TexcoordOffset (VS 11): the material's two texture-transform buffers, read at the index the engine
	 *   flips every frame (Main::Update). Controller writes wake affected material slots; the short watch
	 *   spans the buffer flip and samples both buffers until they agree (ApplyTextureTransform).
	 * - The shader object and engine globals: PS 6, SnowRimLightParameters, CharacterLightParams,
	 *   LODTexParams.z, LandscapeTexture5to6IsSnow.zw. The same for every material that writes them, so they
	 *   are taken each frame from one live evaluation per signature (Signature, ApplyFrameComponents). IBLParams
	 *   is the shader object's too, but no Lighting stage reads it: the record keeps its first evaluation's.
	 * - Render targets: the character light's t11, re-picked every frame. It is the frame's (FrameCharacterLight), not the
	 *   records': a character-light pass's record keeps t11's modes and no view, and the frame record gives the view.
	 */
	namespace MaterialSources
	{
		/** @brief Installs the write hooks (controllers, the engine's material vtables). */
		void Install();

		/** @brief A material's fields were written (any thread, lock-free). */
		void NoteWritten(const RE::BSShaderMaterial* a_material);

		/** @brief NoteWritten when the scope ends: for an in-place rewrite with early returns (CS's PBR materials). */
		struct NoteWrittenOnExit
		{
			const RE::BSShaderMaterial* material;
			~NoteWrittenOnExit() { NoteWritten(material); }
		};

		/**
		 * @brief Render thread: every material written since the last call. False when the queue overflowed,
		 * in which case every material has to be treated as written.
		 */
		bool Drain(ankerl::unordered_dense::set<const RE::BSShaderMaterial*>& a_out);
		void DrainShadingChanges(std::vector<const void*>& a_out);
		void DrainTransformChanges(ankerl::unordered_dense::set<const RE::BSShaderMaterial*>& a_out);
		/** @brief The same events, for the kept shadow build (its one consumer): materials whose texture transform a controller moved. */
		void DrainShadowTransformChanges(std::vector<const RE::BSShaderMaterial*>& a_out);

		/**
		 * @brief Which frame-sourced groups a pass descriptor's SetupMaterial writes. Records with the same
		 * signature hold the same values in those groups.
		 */
		std::uint32_t Signature(std::uint32_t a_passDescriptor);

		/** @brief The frame-sourced PS floats (positions in the PS layout), for the build to repack. */
		const std::vector<std::uint32_t>& FramePSFloats();
		/** @brief TexcoordOffset's floats (positions in the VS layout), for the build to repack. */
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

		/** @brief Writes this frame's TexcoordOffset from the material's fields into the record; whether a float changed. */
		bool ApplyTextureTransform(const RE::BSShaderMaterial* a_material, MaterialRecord& a_record);

		/** @brief Copies the frame-sourced components (floats, and t11 whole: written or not, its view and modes) of a_from over a_to, for comparisons. */
		void CopyFrameComponents(const MaterialRecord& a_from, MaterialRecord& a_to, std::uint32_t a_passDescriptor);
	}
}
