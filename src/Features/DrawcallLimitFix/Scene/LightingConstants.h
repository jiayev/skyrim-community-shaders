#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "ConstantEvaluator.h"
#include "SceneStore.h"

namespace DCLF
{
	/** @brief Constant groups (cbuffer PerTechnique, PerMaterial, PerGeometry in Lighting.hlsl; b0-b2). */
	inline constexpr std::uint32_t kPerTechnique = 0;
	inline constexpr std::uint32_t kPerMaterial = 1;
	inline constexpr std::uint32_t kPerGeometry = 2;

	// Lighting variable indices (ShaderConstants::LightingVS / LightingPS).
	inline constexpr std::uint32_t kVSWorld = 0;
	inline constexpr std::uint32_t kVSPreviousWorld = 1;
	inline constexpr std::uint32_t kVSLandBlendParams = 3;  // MTLand, per object
	inline constexpr std::uint32_t kVSTreeParams = 4;   // TreeAnim wind, per object
	inline constexpr std::uint32_t kVSWindTimers = 5;
	inline constexpr std::uint32_t kVSTextureProj = 6;      // ProjectedUV, per object
	inline constexpr std::uint32_t kVSLeftEyeCenter = 9;     // first PerMaterial VS variable
	inline constexpr std::uint32_t kVSHighDetailRange = 12;  // first PerTechnique VS variable
	inline constexpr std::uint32_t kPSNumLights = 0;
	inline constexpr std::uint32_t kPSPointLightPosition = 1;
	inline constexpr std::uint32_t kPSPointLightColor = 2;
	inline constexpr std::uint32_t kPSDirLightDirection = 3;  // first PerGeometry PS variable
	inline constexpr std::uint32_t kPSMaterialData = 7;
	inline constexpr std::uint32_t kPSEmitColor = 8;
	inline constexpr std::uint32_t kPSShadowLightMaskSelect = 10;
	inline constexpr std::uint32_t kPSProjectedUVParams = 12;  // ProjectedUV, per object: 12, 13 (colour), 14 (globals)
	inline constexpr std::uint32_t kPSSSRParams = 16;
	inline constexpr std::uint32_t kPSFogColor = 19;      // first PerTechnique PS variable
	inline constexpr std::uint32_t kPSLODTexParams = 24;  // first PerMaterial PS variable

	constexpr std::uint64_t VariableBits(std::uint32_t a_first, std::uint32_t a_last)
	{
		return ((a_last >= 63 ? ~0ull : ((1ull << (a_last + 1)) - 1))) & ~((1ull << a_first) - 1);
	}

	/** @brief Which constant group each Lighting variable belongs to, by group. */
	inline constexpr std::uint64_t kVSGroups[3] = { VariableBits(12, 15), VariableBits(9, 11), VariableBits(0, 8) | (1ull << 16) };
	inline constexpr std::uint64_t kPSGroups[3] = { (1ull << 11) | (1ull << 19) | (1ull << 20), VariableBits(21, 50), VariableBits(0, 10) | VariableBits(12, 18) };
	/** @brief Each group's first variable (the only one a constant table may place at offset 0). */
	inline constexpr std::uint32_t kVSFirstVariable[3] = { kVSHighDetailRange, kVSLeftEyeCenter, kVSWorld };
	inline constexpr std::uint32_t kPSFirstVariable[3] = { kPSFogColor, kPSLODTexParams, kPSDirLightDirection };

	/** @brief Per-object light assignment, which the Light Limit Fix shaders never read. */
	inline constexpr std::uint64_t kPSLightAssignment = (1ull << kPSNumLights) | (1ull << kPSPointLightPosition) | (1ull << kPSPointLightColor) |
	                                                    (1ull << kPSShadowLightMaskSelect);

	// The PerGeometry variables that are the frame's globals, the same in every pipeline that writes them (checked by
	// CS_DCLF_PERSISTENT_PARITY): DirLightDirection, DirLightColor, DirectionalAmbient, AmbientSpecularTintAndFresnelPower
	// and AmbientColor. EyePosition (VS 2): see WritesEyePosition.
	inline constexpr std::uint32_t kVSEyePosition = 2;
	/**
	 * @brief Whether SetupGeometry (AE 0x1414dd040) writes EyePosition for a pass descriptor: Envmap, Eye and technique
	 * 0x10, and any pass with descriptor bit 9 or one of 0x21c00 (hair, specular skin and the like). The value is the same
	 * for all of them, the camera less posAdjust (Community Shaders' patch keeps every pass in world space); every other
	 * pass leaves whatever the constant buffer last held, which no draw of it reads.
	 */
	inline constexpr bool WritesEyePosition(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t technique = (a_passDescriptor >> 24) & 0x3f;
		return technique == 1 || technique == 0xb || technique == 0x10 || (a_passDescriptor & 0x21e00u) != 0;
	}
	inline constexpr std::uint32_t kPSFrameGeometry[5] = { 3, 4, 5, 6, 18 };
	// The DCLF_BINDLESS pixel stage reads the first four from the frame's own block (DCLFFrameLighting, PS b13 in
	// Lighting.hlsl), one float4 row each and three for DirectionalAmbient, so no pipeline's block changes with the sun.
	inline constexpr std::uint32_t kFrameLightingRegister = 13;
	inline constexpr std::uint32_t kFrameLightingRows = 6;
	using FrameLighting = std::array<float, kFrameLightingRows * 4>;
	// The fog (VS PerTechnique FogParam, FogNearColor, FogFarColor): the frame's, the same in every technique that writes it, and
	// drifting with the time of day almost every frame. The DCLF_BINDLESS vertex stage reads it from its own frame block
	// (DCLFFrameFog, VS b13 in Lighting.hlsl, which nothing else declares), so no technique row changes with it. The pixel stage's
	// FogColor (PS PerTechnique) drifts with it, but no Lighting stage reads it: a technique row keeps the one it was made with.
	inline constexpr std::uint32_t kVSFrameFog[3] = { 13, 14, 15 };
	inline constexpr std::uint32_t kFrameFogRegister = 13;
	inline constexpr std::uint32_t kFrameFogRows = 3;
	using FrameFog = std::array<float, kFrameFogRows * 4>;
	/**
	 * @brief What the DCLF_BINDLESS builds leave out of a pipeline's PerGeometry block: the frame lighting (read from
	 * DCLFFrameLighting), what they read from the object's record and extras rows instead (World, PreviousWorld,
	 * LandBlendParams, TreeParams, WindTimers, TextureProj; MaterialData, EmitColor, ProjectedUVParams 1-3), and what
	 * Lighting.hlsl never reads (EyePosition, AmbientColor). Packed as zero, so the block's bytes change only with what is
	 * the pipeline's own; SSRParams stays (its w alone is per object).
	 */
	inline constexpr std::uint64_t kVSBindlessGeometryUnread = (1ull << kVSWorld) | (1ull << kVSPreviousWorld) | (1ull << kVSEyePosition) | (1ull << kVSLandBlendParams) |
	                                                           (1ull << kVSTreeParams) | (1ull << kVSWindTimers) | (1ull << kVSTextureProj);
	inline constexpr std::uint64_t kPSBindlessGeometryUnread = (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6) | (1ull << 18) | (1ull << kPSMaterialData) |
	                                                           (1ull << kPSEmitColor) | (7ull << kPSProjectedUVParams);

	/** @brief Whether two PerGeometry evaluations agree in everything a DCLF_BINDLESS draw reads from the pipeline's block. */
	bool SameBindlessGeometry(const GeometryConstants& a_a, const GeometryConstants& a_b);

	/**
	 * @brief The frame lighting rows a PerGeometry PS block writes, into a_out where a_written (a bit per float) does not
	 * have them yet; the bits it filled are added. Techniques differ in which components they write (Eye leaves
	 * AmbientSpecularTintAndFresnelPower.w), so the frame's rows are merged from every evaluation of the frame.
	 */
	void MergeFrameLighting(const ConstantBlock& a_ps, FrameLighting& a_out, std::uint32_t& a_written);
	/** @brief The vertex fog a technique writes, into a_out where a_written (a bit per float) does not have it yet. */
	void MergeFrameFog(const TechniqueConstants& a_technique, FrameFog& a_out, std::uint32_t& a_written);
	/** @brief a_from's fog floats (the vertex rows and FogColor) into a_to: a technique row keeps the fog it was made with. */
	void KeepTechniqueFog(const TechniqueConstants& a_from, TechniqueConstants& a_to);
	/** @brief Whether a PerGeometry PS block agrees with the frame lighting in every component it writes. */
	bool MatchesFrameLighting(const ConstantBlock& a_ps, const FrameLighting& a_lighting, std::string* a_first = nullptr);

	/**
	 * @brief An object's PerGeometry values as its native draw binds them: the per-frame block of its pass
	 * descriptor with the object's transforms (camera-relative) and shading on top.
	 *
	 * The eye positions are passed in rather than read from the shadow state, because the indirect draws
	 * pack their constants after the main pass has drawn, by when the engine has moved the camera on: a
	 * world transform made relative to the wrong eye shifts the object by the camera's movement.
	 */
	GeometryConstants ObjectGeometryConstants(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags, const RE::NiPoint3& a_eye,
		const RE::NiPoint3& a_previousEye);

	/**
	 * @brief Writes a constant group into the byte layout of a shader's cbuffer, using the shader's constant
	 * table (variable index -> dword offset, as Community Shaders reflects it). Components the engine
	 * does not write become 0. Returns the bytes the group spans (0 when the table places nothing).
	 */
	std::size_t PackConstantGroup(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables,
		std::uint32_t a_firstVariable, std::span<std::byte> a_out);

	/**
	 * @brief Where the five per-object PerGeometry variables land in a packed constant group.
	 *
	 * The PerGeometry group is the same for every object on a pipeline apart from these five, so the group
	 * is packed once per pipeline and each object only rewrites them. Offsets and sizes are in floats;
	 * an offset of ~0 means the permutation does not declare that variable.
	 */
	struct GeometryPatchOffsets
	{
		std::uint32_t vsWorld = ~0u, vsWorldSize = 0;
		std::uint32_t vsPreviousWorld = ~0u, vsPreviousWorldSize = 0;
		std::uint32_t psMaterialData = ~0u, psMaterialDataSize = 0;
		std::uint32_t psEmitColor = ~0u, psEmitColorSize = 0;
		std::uint32_t psSSRParams = ~0u, psSSRParamsSize = 0;
		std::uint32_t vsTreeParams = ~0u, vsTreeParamsSize = 0;
		std::uint32_t vsWindTimers = ~0u, vsWindTimersSize = 0;
		std::uint32_t vsLandBlendParams = ~0u, vsLandBlendParamsSize = 0;
		std::uint32_t vsTextureProj = ~0u, vsTextureProjSize = 0;
		std::array<std::uint32_t, 3> psProjectedUVParams{ ~0u, ~0u, ~0u };
		std::array<std::uint32_t, 3> psProjectedUVParamsSize{};
	};

	/** @brief The object's kExtraRows rows in Tables::extraRows, or null when it has none. */
	const float* ExtraRowsOf(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex);

	/** @brief Resolves those offsets for one pipeline's shader pair. */
	GeometryPatchOffsets GeometryPatchOffsetsOf(std::span<const std::uint8_t> a_vsTable, std::span<const std::uint8_t> a_psTable);

	/**
	 * @brief Writes one object's five PerGeometry variables over an already packed group.
	 *
	 * Equivalent to ObjectGeometryConstants followed by PackConstantGroup, without walking the whole
	 * variable table per object: everything else in the group comes from the pipeline's template. A
	 * component the object leaves unwritten packs as zero, which is what PackConstantGroup's initial
	 * memset produces for it.
	 */
	void PatchObjectGeometry(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags,
		const RE::NiPoint3& a_eye, const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets,
		std::span<std::byte> a_vsOut, std::span<std::byte> a_psOut);

	/**
	 * @brief One object's entry in the per-object record table the DCLF_BINDLESS builds read
	 * (DCLFObjectRecord in Common/DCLFObjects.hlsli): what is the object's and changes with its structure (shading, bindings,
	 * skin, tree), not with its placement. The placement is the frame's, in a row of its own (BindlessPlacement), so a move
	 * rewrites no record. The shading half is ObjectShading unchanged, which is why it can be copied straight through.
	 */
	struct BindlessObject
	{
		ObjectShading shading;  // MaterialData, EmitColor, and SSRParams.w in the last float
		// The values the native shaders read from constant buffers of their own, which would make the binding
		// record per-object: Light Limit Fix's room index (PS b3; its shadow bit mask is the draw's object word), the alpha
		// test reference (PS b11) and Linear Lighting's emissive multiplier (PS b8). Deliberately a row of
		// their own rather than packed into the spare ObjectShading::materialData[3]: that struct is shared
		// with ObjectGeometryConstants and PatchObjectGeometry, so anything parked there would leak into
		// the non-bindless build's MaterialData.w and into the parity comparison of that group.
		std::int32_t roomIndex;
		std::uint32_t recordFlags;  // kRecordBeastRace
		float alphaTestRef;
		float emissiveMult;
		// Tree animation, per object (technique 12). Under bindless the PerGeometry block is one pair for
		// the whole pipeline, so these cannot stay in it the way they can on the constant-buffer path.
		ObjectTreeAnim tree;
		// Skinning (kObjectSkinned): where this object's bone palette rows start in the epoch's bones
		// buffer (VS t126, DCLFBones), current then previous, and how many rows (three a bone). The rows
		// are packed eye-relative by the epoch, like World, so the shader's pivot is zero. 0/0/0 otherwise.
		std::uint32_t boneOffset;
		std::uint32_t previousBoneOffset;
		std::uint32_t boneRows;
		// The object's kExtraRows rows in the same buffer (after every palette), or 0 when it has none.
		std::uint32_t extraOffset;
		// Advanced Skin's SkinPerGeometry (PS b7): the owning actor's wetness (SceneStore::Tables::skinWetness),
		// zero for everything else. Per object like the rest, so the binding record stays per (material, pipeline).
		float skinPerGeometry[4];
		// The specular and envmap LOD fades the pass applies (kLodFadeSpecular, kLodFadeEnvmap, kLodFadeSsr): its pipeline's.
		// The draw applies them, from the frame's camera (LodFadeFrame), only while the placement's fade node has them apply
		// (BindlessPlacement::lodFadeNode, LodFadesApply), with that node's LOD type; else MaterialData's fades are the property's.
		std::uint32_t lodFades;
		std::uint32_t padding[3];
	};
	static_assert(sizeof(BindlessObject) == 128);
	// BindlessObject::recordFlags (DCLFObjects.hlsli, DCLFRecordFlags): Subsurface Scattering's IsBeastRace (kObjectBeastRace).
	inline constexpr std::uint32_t kRecordBeastRace = 1u << 0;
	// ... and an alpha-tested object whose alpha property blends (kObjectAlphaBlended): the depth pass's reference (Utility.hlsl).
	inline constexpr std::uint32_t kRecordAlphaBlended = 1u << 1;

	/**
	 * @brief One object's placement (DCLFPlacement in Common/DCLFObjects.hlsli, VS and PS t123): what a move changes, by object
	 * slot like the records. World and PreviousWorld are absolute: the shaders subtract the drawing camera's eye (VS_PerFrame
	 * c40/c41), so one row serves every epoch and every camera.
	 */
	struct BindlessPlacement
	{
		float world[12];
		float previousWorld[12];
		// The culling's (BuildDrawsCS): the world bound (centre, radius) and the sun entry's sphere (SceneStore::Tables::sunEntry;
		// radius +max when it has none, inside every process).
		float bound[4];
		float sunEntry[4];
		// The fade node (LodFadeNodeOf): its world bound centre, and in w its LOD type plus kLodFadeHeld when the LOD fades do not
		// apply; w < 0 without one. The fade-out tests' centre (BuildDrawsCS, FadeStateCS) and the draw's LOD fades' (Lighting.hlsl).
		float lodFadeNode[4];
	};
	static_assert(sizeof(BindlessPlacement) == 144);
	// BuildDrawsCS.hlsl and FadeStateCS.hlsl read these by their float4 index in the row (kPlacementBoundRow, kPlacementSunEntryRow,
	// kPlacementFadeNodeRow).
	static_assert(offsetof(BindlessPlacement, bound) == 6 * 16 && offsetof(BindlessPlacement, sunEntry) == 7 * 16 && offsetof(BindlessPlacement, lodFadeNode) == 8 * 16);

	/**
	 * @brief Fills one, from the same inputs PatchObjectGeometry writes into a packed group.
	 * a_boneRegion: the bone rows' layout the record addresses (the buffer's: SceneSizing::boneRegion), its previous palette and
	 * extras past it; ~0u: the tables' own capacity.
	 */
	void BuildObjectRecord(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags, BindlessObject& a_out,
		std::uint32_t a_boneRegion = ~0u);
	/** @brief Fills an object's placement row from the tables (render flag 0x10: the previous transform is the current one). */
	void BuildPlacementRow(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags, BindlessPlacement& a_out);

	/** @brief World made relative to an eye the way the engine does it (and the shaders do for a record). */
	void StoreRelativeTo(float* a_out, const float (&a_world)[12], const RE::NiPoint3& a_eye);

	/**
	 * @brief Where PackConstantGroup puts a block's float: its dword offset in the packed group, or ~0 when no
	 * variable of the group covers it (the float is then not packed at all).
	 */
	std::uint32_t PackedPositionOf(const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_firstVariable,
		std::uint32_t a_float);

	/** @brief Bytes a group spans in a shader's cbuffer layout (16-byte multiple). */
	std::size_t ConstantGroupSize(const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_firstVariable);
}
