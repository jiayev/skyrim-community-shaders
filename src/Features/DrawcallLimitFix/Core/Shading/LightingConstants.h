#pragma once

// The Lighting shader's constant layout and DCLF's per-object rows: variable groups, the frame blocks (lighting, fog, extras) the
// bindless stages read instead of per-pipeline constants, the BindlessObject / Shading / Placement rows, and packing a
// group into a cbuffer. Sample* run on the render thread; the rest are pure.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/FrameTables.h"

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
	inline constexpr std::uint32_t kVSTexcoordOffset = 11;   // the frame's (texture transforms patch the rows)
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
	inline constexpr std::uint32_t kPSIBLParams = 29;     // read by no Lighting stage: a record keeps its first (KeepUnreadFloats)

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

	// PerGeometry variables that are frame globals, the same in every pipeline (checked by CS_DCLF_PERSISTENT_PARITY):
	// DirLightDirection, DirLightColor, DirectionalAmbient, AmbientSpecularTintAndFresnelPower, AmbientColor. EyePosition (VS 2):
	// see WritesEyePosition.
	inline constexpr std::uint32_t kVSEyePosition = 2;
	/**
	 * @brief Whether SetupGeometry (AE 0x1414dd040) writes EyePosition for a pass: Envmap, Eye, technique 0x10, and descriptor bit 9
	 * or any of 0x21c00 (hair, specular skin, ...). Always the camera less posAdjust (CS keeps every pass in world space); other
	 * passes leave a stale value no draw of theirs reads.
	 */
	inline constexpr bool WritesEyePosition(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t technique = (a_passDescriptor >> 24) & 0x3f;
		return technique == 1 || technique == 0xb || technique == 0x10 || (a_passDescriptor & 0x21e00u) != 0;
	}
	inline constexpr std::uint32_t kPSFrameGeometry[5] = { 3, 4, 5, 6, 18 };
	// The DCLF_BINDLESS pixel stage reads the first four from DCLFFrameLighting (PS b13), a row each and three for
	// DirectionalAmbient, so no pipeline's block changes with the sun.
	inline constexpr std::uint32_t kFrameLightingRegister = 13;
	inline constexpr std::uint32_t kFrameLightingRows = 6;
	using FrameLighting = std::array<float, kFrameLightingRows * 4>;
	// Fog (VS PerTechnique FogParam, FogNearColor, FogFarColor) is the frame's and drifts with the time of day; the DCLF_BINDLESS
	// vertex stage reads it from DCLFFrameFog (VS b13), so no technique row changes with it. PS FogColor drifts too but no Lighting
	// stage reads it: a technique row keeps the one it was made with.
	inline constexpr std::uint32_t kVSFrameFog[3] = { 13, 14, 15 };
	inline constexpr std::uint32_t kFrameFogRegister = 13;
	inline constexpr std::uint32_t kFrameFogRows = 3;
	using FrameFog = std::array<float, kFrameFogRows * 4>;
	/**
	 * @brief What DCLF_BINDLESS leaves out of a pipeline's PerGeometry block (packed as zero): the frame lighting, what the object's
	 * record and extras rows supply, and what Lighting.hlsl never reads (EyePosition, AmbientColor). SSRParams stays (only w is per
	 * object).
	 */
	inline constexpr std::uint64_t kVSBindlessGeometryUnread = (1ull << kVSWorld) | (1ull << kVSPreviousWorld) | (1ull << kVSEyePosition) | (1ull << kVSLandBlendParams) |
	                                                           (1ull << kVSTreeParams) | (1ull << kVSWindTimers) | (1ull << kVSTextureProj);
	inline constexpr std::uint64_t kPSBindlessGeometryUnread = (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6) | (1ull << 18) | (1ull << kPSMaterialData) |
	                                                           (1ull << kPSEmitColor) | (7ull << kPSProjectedUVParams);

	/** @brief Whether two PerGeometry evaluations agree in everything a DCLF_BINDLESS draw reads from the pipeline's block. */
	bool SameBindlessGeometry(const GeometryConstants& a_a, const GeometryConstants& a_b);

	/**
	 * @brief Merges a PerGeometry PS block's frame lighting into a_out where a_written (a bit per float) lacks it. Techniques write
	 * different components (Eye leaves AmbientSpecularTintAndFresnelPower.w), so the frame merges every evaluation.
	 */
	void MergeFrameLighting(const ConstantBlock& a_ps, FrameLighting& a_out, std::uint32_t& a_written);
	/** @brief The vertex fog a technique writes, into a_out where a_written (a bit per float) does not have it yet. */
	void MergeFrameFog(const TechniqueConstants& a_technique, FrameFog& a_out, std::uint32_t& a_written);
	/** @brief a_from's fog floats (the vertex rows and FogColor) into a_to: a technique row keeps the fog it was made with. */
	void KeepTechniqueFog(const TechniqueConstants& a_from, TechniqueConstants& a_to);

	/**
	 * @brief An object's PerGeometry values as its native draw binds them: its pass's frame block with the object's camera-relative
	 * transforms and shading on top. The eyes are passed in: by packing time the engine has moved the camera on.
	 */
	struct BindlessPlacement;
	struct BindlessShading;
	struct ExtrasFrame;
	GeometryConstants ObjectGeometryConstants(const SceneTables& a_tables, const FrameTables& a_frame, std::uint32_t a_objectIndex, const BindlessPlacement& a_placement,
		const BindlessShading& a_shading, const ExtrasFrame& a_extrasFrame, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_previousEye);

	/**
	 * @brief Packs a constant group into a shader's cbuffer layout via its constant table (variable -> dword offset, as CS reflects
	 * it). Unwritten components become 0. Returns the bytes spanned (0 when the table places nothing).
	 */
	std::size_t PackConstantGroup(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables,
		std::uint32_t a_firstVariable, std::span<std::byte> a_out);

	/**
	 * @brief Where the per-object PerGeometry variables land in a group packed once per pipeline. In floats; ~0: not declared.
	 */
	struct GeometryPatchOffsets
	{
		std::uint32_t vsWorld = ~0u, vsWorldSize = 0;
		std::uint32_t vsPreviousWorld = ~0u, vsPreviousWorldSize = 0;
		std::uint32_t psMaterialData = ~0u, psMaterialDataSize = 0;
		std::uint32_t psEmitColor = ~0u, psEmitColorSize = 0;
		std::uint32_t psSSRParams = ~0u, psSSRParamsSize = 0;
		// The feature block's variables (Extensions::ShadingBlockRule's, in its order; a word each at most).
		std::array<std::uint32_t, 8> vsBlock{ ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
		std::array<std::uint32_t, 8> vsBlockSize{};
		std::uint32_t vsLandBlendParams = ~0u, vsLandBlendParamsSize = 0;
		std::uint32_t vsTextureProj = ~0u, vsTextureProjSize = 0;
		std::array<std::uint32_t, 3> psProjectedUVParams{ ~0u, ~0u, ~0u };
		std::array<std::uint32_t, 3> psProjectedUVParamsSize{};
	};

	/** @brief The object's kExtraRows rows in Tables::extraRows, or null when it has none. */
	const float* ExtraRowsOf(const SceneTables& a_tables, std::uint32_t a_objectIndex);

	/**
	 * @brief Per-frame inputs that complete an object's static extras with its placement (Lighting.hlsl; CompleteExtras on the CPU).
	 * Sampled by each main commit (render thread): the vertex part after the fog (VS b13, c3-c7), the pixel part at PS b13 c16.
	 */
	struct ExtrasFrame
	{
		// VS b13 c3: the land blend's position (BSShaderManager::State, its clock's blend of two positions) in xy.
		float landBlend[4]{};
		// VS b13 c4-c7: the ProjectedUV projection (a fixed rotation about Z at posAdjust) as the engine's NiTransform-to-matrix routine
		// makes it (row-major, row vectors, zero translation). TextureProj's rows are the columns of World x this (this alone for Envmap).
		float projection[16]{};
		// PS b13 c16: ProjectedUVParams3 (FUN_1414e00c0's globals): the diffuse and detail tiling, 0, the projected-normals switch.
		float projectedGlobals[4]{};
	};
	static_assert(sizeof(ExtrasFrame) == 96);
	inline constexpr std::size_t kExtrasFrameVertexBytes = offsetof(ExtrasFrame, projectedGlobals);
	/** @brief This frame's, from the engine's globals (render thread). */
	ExtrasFrame SampleExtrasFrame();
	/**
	 * @brief How TextureProj is made (the static land-blend row's x; ProjectedUV and land blend are exclusive): World x projection;
	 * the projection alone (Envmap); the shape's own (a multi-index shape's materialProjection, static rows 1-3).
	 */
	inline constexpr float kTextureProjWorld = 0.0f, kTextureProjProjection = 1.0f, kTextureProjShape = 2.0f;
	/** @brief An object's extras rows as its draw completes them (Lighting.hlsl); a_world row-major 3x4, absolute. */
	void CompleteExtras(const float* a_static, std::uint32_t a_objectFlags, const float (&a_world)[12], const ExtrasFrame& a_frame, float* a_out);

	/** @brief Resolves those offsets for one pipeline's shader pair. */
	GeometryPatchOffsets GeometryPatchOffsetsOf(std::span<const std::uint8_t> a_vsTable, std::span<const std::uint8_t> a_psTable);

	/**
	 * @brief Writes one object's per-object PerGeometry variables over its pipeline's packed template: ObjectGeometryConstants plus
	 * PackConstantGroup without walking the table (unwritten components zero, as there). Extras are the static rows.
	 */
	void PatchObjectGeometry(const SceneTables& a_tables, std::uint32_t a_objectIndex, const BindlessPlacement& a_placement,
		const BindlessShading& a_shading, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets,
		std::span<std::byte> a_vsOut, std::span<std::byte> a_psOut);

	/**
	 * @brief One object's record (DCLFObjectRecord, Common/DCLFObjects.hlsli): what changes with its structure (bindings, skin,
	 * tree, extras). Placement and shading have rows of their own, so neither a move nor a flicker rewrites a record.
	 */
	struct BindlessObject
	{
		// Values native shaders read from their own constant buffers: the ancestor value (Api::AncestorValueContribution::recordWord 0:
		// Light Limit Fix's room index, PS b3; its shadow mask is the draw's object word) and the alpha test reference (PS b11).
		std::int32_t ancestorValue;
		// kRecord* bits; bits 9-31 the fade root slot + 1 (0: none), by which the pixel stage reads FadeStateCS's fade (DCLFAlphaFade).
		std::uint32_t recordFlags;
		float alphaTestRef;
		// The LOD fades the pass applies (kLodFade*), from LodFadeFrame, while the placement's fade node has them apply
		// (LodFadesApply); else MaterialData's fades are the shading row's.
		std::uint32_t lodFades;
		// The feature block (words 4-11; DCLFTreeParams, DCLFWindTimers): zero bar a table family's reference (Api::TableFamily::recordWord:
		// Features/TreeWind's tree slot and generation in words 10-11); the variables it supplies are the shading row's and the feature's.
		std::uint32_t featureBlock[8];
		// Skinning (kObjectPaletteBlock): current and previous palette rows in the frame's palettes (VS t122, DCLFPalettes; PaletteRowsOf)
		// and the row count (three a bone). Absolute, like World. 0/0/0 otherwise.
		std::uint32_t boneOffset;
		std::uint32_t previousBoneOffset;
		std::uint32_t boneRows;
		// The object's kExtraRows rows in the extras buffer (VS and PS t126, DCLFExtras), or 0 when it has none.
		std::uint32_t extraOffset;
	};
	static_assert(sizeof(BindlessObject) == 64);

	/**
	 * @brief One object's shading (DCLFShading, PS t121): what its property's controllers and fades animate, and its actor's
	 * wetness. FrameValues' rows by object slot, sampled at the frame's start from shading events and the wetness capture.
	 */
	struct BindlessShading
	{
		// MaterialData, EmitColor, and SSRParams.w in the last float; unwritten components zero.
		ObjectShading shading;
		// Advanced Skin's SkinPerGeometry (PS b7): the owning actor's wetness (Skin::GetWetness), zero for everything else.
		float skinPerGeometry[4];
		// Linear Lighting's emissive multiplier (PS b8): the same sample emitColor folds in (the shader divides it out again).
		float emissiveMult;
		// A bit per ObjectShading float SetupGeometry writes (kShadingWritten*), for CPU comparisons; the shader reads none.
		std::uint32_t written;
		std::uint32_t padding[2];
		// The feature block (DCLFShadingTreeParams, DCLFShadingWindTimers): the shading contribution claiming it derives its first blockWords
		// (Extensions::ShadingBlockRule: Features/TreeWind's SetupGeometry TreeParams and WindTimers.xy for a TreeAnim property, what a
		// member draws with until TreeWindCS has an entry for its tree: DCLFTreeParamsOf); the rest zero.
		float featureBlock[8];
	};
	static_assert(sizeof(BindlessShading) == 96);

	/**
	 * @brief An object's shading from its property now (SetupGeometry's MaterialData, EmitColor, SSRParams.w for a_pass, and Linear
	 * Lighting's multiplier); wetness left as is. a_member: fades are DCLF's (FadeStateCS), so not in its alpha. Any thread inside the
	 * engine-read window. A material port's property (Extensions::PortOfRtti) is its port's (SamplePortShading).
	 */
	void SampleShading(const RE::BSShaderProperty& a_property, std::uint32_t a_pass, bool a_member, BindlessShading& a_out);
	// BindlessObject::recordFlags (DCLFObjects.hlsli, DCLFRecordFlags): bit 0 a Lighting object flag's record bit
	// (Api::LightingObjectFlag: Integrations/SubsurfaceScattering's IsBeastRace, kRecordBeastRace) ...
	// ... and an alpha-tested object whose alpha property blends (kObjectAlphaBlended): the depth pass's reference (Utility.hlsl).
	inline constexpr std::uint32_t kRecordAlphaBlended = 1u << 1;
	// Refraction (Utility.hlsl DCLF_REFRACTION): a refraction object and the RENDER_NORMAL variants (GetRenderPasses, AE 0x1414adfb0)
	// its draw takes from here, not its pipeline: FALLOFF (kRefractionFalloff), CLAMP (kNonProjectiveShadows), no Normals
	// (kModelSpaceNormals), VC (kVertexColors), two-sided (kTwoSided). Skinned is the record's palette.
	inline constexpr std::uint32_t kRecordRefraction = 1u << 2;
	inline constexpr std::uint32_t kRecordRefractionFalloff = 1u << 3;
	inline constexpr std::uint32_t kRecordRefractionClamp = 1u << 4;
	inline constexpr std::uint32_t kRecordRefractionNoNormals = 1u << 5;
	inline constexpr std::uint32_t kRecordRefractionVertexColors = 1u << 6;
	inline constexpr std::uint32_t kRecordRefractionTwoSided = 1u << 7;
	/** @brief The kRecordRefraction* bits of an object drawn with a_key: none unless it is a refraction key (kRasterRefraction). */
	inline constexpr std::uint32_t RefractionRecordFlagsOf(const PipelineKey& a_key)
	{
		if (!(a_key.rasterFlags & kRasterRefraction))
			return 0u;
		return kRecordRefraction | ((a_key.rasterFlags & kRasterRefractionFalloff) ? kRecordRefractionFalloff : 0u) |
		       ((a_key.rasterFlags & kRasterRefractionClamp) ? kRecordRefractionClamp : 0u) |
		       ((a_key.passDescriptor & (1u << 2)) ? kRecordRefractionNoNormals : 0u) |     // the pass's ModelSpaceNormals
		       ((a_key.passDescriptor & (1u << 0)) ? kRecordRefractionVertexColors : 0u) |  // its VC
		       ((a_key.rasterFlags & kRasterTwoSided) ? kRecordRefractionTwoSided : 0u);
	}
	// A program family's record bits (Api::ProgramFamilyContribution::records) share bits 3-7 with refraction: a family's record never has
	// kRecordRefraction (Features/Effects/EffectConstants.h: the Effect family's suppress, billboard and mode bits).
	/**
	 * @brief The record bits of an object drawn with a_key its family sets (Extensions::FamilyRules' records: the Effect family's
	 * suppress and billboard bits, its billboard mode from the key's spare bits): none for a Lighting key.
	 */
	inline std::uint32_t FamilyRecordFlagsOf(const PipelineKey& a_key, std::uint32_t a_objectFlags)
	{
		const auto& rules = Extensions::FamilyRules(a_key.rasterFlags);
		const std::uint32_t turned = rules.turnedKeyBits ? (a_key.rasterFlags >> rules.spareShift) & kProgramFamilySpareBits : 0u;
		return ((a_objectFlags & kObjectSuppressExternalEmittance) ? rules.records.suppressEmittance : 0u) |
		       (turned ? rules.records.turned | ((turned - 1) << rules.records.turnedModeShift) : 0u);
	}
	/**
	 * @brief Whether a_key's family keeps the alpha property's threshold as its reference whatever its alpha test (its records'
	 * alphaRefPassBits in its pass descriptor): the Effect Blood permutation, whose PS AlphaTestRef.x SetupGeometry (0x1414eab10) writes
	 * as the threshold / 255 (DCLFBloodAlphaTestRef).
	 */
	inline bool FamilyAlphaRefKey(const PipelineKey& a_key) { return (a_key.passDescriptor & Extensions::FamilyRules(a_key.rasterFlags).records.alphaRefPassBits) != 0; }
	/** @brief BindlessObject::alphaTestRef of an object drawn with a_key: threshold / 255 when alpha tested or a Blood effect, else 0. */
	inline float AlphaTestRefOf(std::uint32_t a_objectFlags, const PipelineKey* a_key)
	{
		const bool blood = a_key && !(a_objectFlags & kObjectNoBindings) && FamilyAlphaRefKey(*a_key);
		return (a_objectFlags & kObjectAlphaTest) || blood ? ((a_objectFlags >> kObjectAlphaThresholdShift) & 0xFF) / 255.0f : 0.0f;
	}
	/**
	 * @brief A decal's draw (kRasterDecalNudge): the vertex stage moves its clip position toward the eye (DCLFObjects.hlsli,
	 * DCLFDecalNudged), in place of the engine's decal depth bias, which a device-generated command's pipeline switch does not carry,
	 * and over the exact depth tie a bias-free decal (interiors) has with its host, which DCLF's two vertex modules do not keep.
	 */
	inline constexpr std::uint32_t kRecordDecalNudge = 1u << 8;
	/** @brief The kRecordDecalNudge bit of an object drawn with a_key. */
	inline constexpr std::uint32_t DecalRecordFlagsOf(const PipelineKey& a_key) { return (a_key.rasterFlags & kRasterDecalNudge) ? kRecordDecalNudge : 0u; }
	inline constexpr std::uint32_t kRecordFadeRootShift = 9;
	/**
	 * @brief BindlessObject::recordFlags of an object: its flags and its fade root slot + 1 in bits 9-31 (the record's own: the root
	 * states' family, Api::TableFamily::recordShift, writes it as BuildObjectRecord's families do, with the same 23-bit cut-off).
	 */
	inline std::uint32_t RecordFlagsOf(std::uint32_t a_objectFlags, std::uint32_t a_fadeRoot)
	{
		return Extensions::LightingRecordFlagsOf(a_objectFlags) | ((a_objectFlags & kObjectAlphaBlended) ? kRecordAlphaBlended : 0u) |
		       (a_fadeRoot < (1u << (32 - kRecordFadeRootShift)) - 1 ? (a_fadeRoot + 1) << kRecordFadeRootShift : 0u);
	}

	/**
	 * @brief One object's placement (DCLFPlacement, VS and PS t123): what a move changes. World and PreviousWorld are absolute (the
	 * shaders subtract the drawing camera's eye, VS_PerFrame c40/c41), so one row serves every epoch and camera.
	 */
	struct BindlessPlacement
	{
		float world[12];
		float previousWorld[12];
		// BuildDrawsCS's world bound and sun entry sphere (radius +max without one: inside every process).
		float bound[4];
		float sunEntry[4];
		// LodFadeNodeOf: the fade-out tests' centre (BuildDrawsCS, FadeStateCS) and the draw's LOD fades' (Lighting.hlsl).
		float lodFadeNode[4];
		// The forward sort key: a blended leaf under a BSOrderedNode draws in the node's group at its depth and child order
		// (FUN_1414b39d0's group stack): the nearest such node's bound centre, and in w (as int bits) the leaf's 1-based depth-first
		// ordinal under it (OnVisible's append order); all 0 under none.
		float forwardSort[4];
	};
	static_assert(sizeof(BindlessPlacement) == 160);
	// Must match the shaders' float4 row indices (kPlacementBoundRow, kPlacementSunEntryRow, kPlacementFadeNodeRow, kObjectForwardSortRow).
	static_assert(offsetof(BindlessPlacement, bound) == 6 * 16 && offsetof(BindlessPlacement, sunEntry) == 7 * 16 && offsetof(BindlessPlacement, lodFadeNode) == 8 * 16 &&
				  offsetof(BindlessPlacement, forwardSort) == 9 * 16);

	/**
	 * @brief A palette block's rows in the frame's palettes: current at twice its Tables::boneOffset, previous right after. Blocks
	 * never overlap, so neither do these.
	 */
	struct PaletteRows
	{
		std::uint32_t current = 0, previous = 0;
	};
	constexpr PaletteRows PaletteRowsOf(std::uint32_t a_offset, std::uint32_t a_rows) { return { 2 * a_offset, 2 * a_offset + a_rows }; }

	/** @brief Fills one, from the same inputs PatchObjectGeometry writes into a packed group (its shading is a BindlessShading). */
	void BuildObjectRecord(const SceneTables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags, BindlessObject& a_out);

	/** @brief World made relative to an eye the way the engine does it (and the shaders do for a record). */
	void StoreRelativeTo(float* a_out, const float (&a_world)[12], const RE::NiPoint3& a_eye);

	/** @brief The packed dword offset of a block's float, or ~0 when no variable of the group covers it. */
	std::uint32_t PackedPositionOf(const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_firstVariable,
		std::uint32_t a_float);

	/** @brief Bytes a group spans in a shader's cbuffer layout (16-byte multiple). */
	std::size_t ConstantGroupSize(const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_firstVariable);
}
