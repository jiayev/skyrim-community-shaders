#pragma once

#include <cstdint>
#include <vector>

#include <ankerl/unordered_dense.h>

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
	 *   flips every frame (Main::Update). A controller writes the buffer the next frame reads. It is computed
	 *   from the material's fields for every used material every frame (ApplyTextureTransform).
	 * - The shader object and engine globals: IBLParams, PS 6, SnowRimLightParameters, CharacterLightParams,
	 *   LODTexParams.z, LandscapeTexture5to6IsSnow.zw. The same for every material that writes them, so they
	 *   are taken each frame from one live evaluation per signature (Signature, ApplyFrameComponents).
	 * - Render targets: the character light's t11, re-picked every frame; with the frame components.
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
		 * every listed float the record has written, and t11. True when t11 changed (the record's textures
		 * are part of its version; its floats are repacked every build).
		 */
		bool ApplyFrameComponents(const MaterialRecord& a_live, MaterialRecord& a_record, std::uint32_t a_passDescriptor, bool* a_floatsChanged = nullptr);

		/** @brief Writes this frame's TexcoordOffset from the material's fields into the record. */
		/** @brief Returns whether a float changed. */
		bool ApplyTextureTransform(const RE::BSShaderMaterial* a_material, MaterialRecord& a_record);

		/** @brief Copies the frame-sourced components (floats and t11) of a_from over a_to, for comparisons. */
		void CopyFrameComponents(const MaterialRecord& a_from, MaterialRecord& a_to, std::uint32_t a_passDescriptor);
	}
}
