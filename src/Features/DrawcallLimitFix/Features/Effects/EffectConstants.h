#pragma once

// The Effect port's constants: BSEffectShader's variable space, the pass descriptor's bits, its frame constant rows and soft-depth
// register (Features/Manifest.cpp lists them), its record bits and BSEffectShader's engine addresses. Included by the feature's sources and the manifest only.

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace DCLF::Features::Effects
{
	/*
	 * The Effect port (CS_DCLF_EFFECTS; dclf-architecture.md, "The Effect shader in the forward epoch"): BSEffectShader's constants in
	 * the engine's own variable space. BSEffectShader's Setup functions write VS variable i through the vertex shader's constant table at
	 * +0x50 + i and PS variable i through the pixel shader's at +0x40 + i (SetupTechnique 0x1414ea2c0, SetupMaterial 0x1414ea6c0,
	 * SetupGeometry 0x1414eab10), the order Community Shaders reflects Effect.hlsl into (ShaderCache.cpp, effectVS / effectPS). An effect
	 * MaterialRecord's blocks hold them in EffectVSLayout / EffectPSLayout order (EffectMaterial.cpp).
	 */
	inline constexpr std::uint32_t kEffectVSVariables = 14;
	inline constexpr std::uint32_t kEffectPSVariables = 19;

	inline constexpr std::uint32_t kEffectVSWorld = 0;
	inline constexpr std::uint32_t kEffectVSPreviousWorld = 1;
	inline constexpr std::uint32_t kEffectVSBones = 2;
	inline constexpr std::uint32_t kEffectVSEyePosition = 3;
	inline constexpr std::uint32_t kEffectVSFogParam = 4;  // first PerTechnique VS variable
	inline constexpr std::uint32_t kEffectVSFogNearColor = 5;
	inline constexpr std::uint32_t kEffectVSFogFarColor = 6;
	inline constexpr std::uint32_t kEffectVSFalloffData = 7;
	inline constexpr std::uint32_t kEffectVSSoftMaterialParams = 8;
	inline constexpr std::uint32_t kEffectVSTexcoordOffset = 9;  // first PerMaterial VS variable
	inline constexpr std::uint32_t kEffectVSTexcoordOffsetMembrane = 10;
	inline constexpr std::uint32_t kEffectVSSubTexOffset = 11;
	inline constexpr std::uint32_t kEffectVSPosAdjust = 12;
	inline constexpr std::uint32_t kEffectVSMatProj = 13;

	inline constexpr std::uint32_t kEffectPSPropertyColor = 0;
	inline constexpr std::uint32_t kEffectPSAlphaTestRef = 1;
	inline constexpr std::uint32_t kEffectPSPLightPositionX = 4;  // first PerGeometry PS variable
	inline constexpr std::uint32_t kEffectPSDLightColor = 11;
	inline constexpr std::uint32_t kEffectPSVPOSOffset = 12;
	inline constexpr std::uint32_t kEffectPSCameraData = 13;  // first PerTechnique PS variable
	inline constexpr std::uint32_t kEffectPSFilteringParam = 14;
	inline constexpr std::uint32_t kEffectPSBaseColor = 15;  // first PerMaterial PS variable
	inline constexpr std::uint32_t kEffectPSBaseColorScale = 16;
	inline constexpr std::uint32_t kEffectPSLightingInfluence = 17;

	/** @brief Effect.hlsl's member names, by variable (MainPipelines.cpp, ConstantTableOf, for an effect key: the port's space's names). */
	inline constexpr std::array<std::string_view, kEffectVSVariables> kEffectVSNames{ "World", "PreviousWorld", "Bones", "EyePosition", "FogParam", "FogNearColor",
		"FogFarColor", "FalloffData", "SoftMateralVSParams", "TexcoordOffset", "TexcoordOffsetMembrane", "SubTexOffset", "PosAdjust", "MatProj" };
	inline constexpr std::array<std::string_view, kEffectPSVariables> kEffectPSNames{ "PropertyColor", "AlphaTestRef", "MembraneRimColor", "MembraneVars",
		"PLightPositionX", "PLightPositionY", "PLightPositionZ", "PLightingRadiusInverseSquared", "PLightColorR", "PLightColorG", "PLightColorB", "DLightColor",
		"VPOSOffset", "CameraDataEffect", "FilteringParam", "BaseColor", "BaseColorScale", "LightingInfluence", "ExtendedFlags" };

	/**
	 * @brief The variables of more than 4 floats (World, PreviousWorld and MatProj are float3x4, MatProj a float4[3]) and the ones nothing
	 * reads (Bones, skinned only, never needs real storage): EffectVSLayout's, the Effect port's space (MakeStageLayout).
	 */
	inline constexpr std::pair<std::uint32_t, std::uint32_t> kEffectVSSized[] = { { kEffectVSWorld, 12 }, { kEffectVSPreviousWorld, 12 }, { kEffectVSMatProj, 12 } };
	inline constexpr std::uint32_t kEffectVSScratch[] = { kEffectVSBones };

	/** @brief Which constant group (PerTechnique, PerMaterial, PerGeometry: b0-b2) each effect variable belongs to, and each group's first. */
	inline constexpr std::uint64_t kEffectVSGroups[3] = { 0x70ull, 0x380ull, 0x3C0Full };
	inline constexpr std::uint64_t kEffectPSGroups[3] = { 0x7000ull, 0x38000ull, 0x40FFFull };
	inline constexpr std::uint32_t kEffectVSFirstVariable[3] = { kEffectVSFogParam, kEffectVSTexcoordOffset, kEffectVSWorld };
	inline constexpr std::uint32_t kEffectPSFirstVariable[3] = { kEffectPSCameraData, kEffectPSBaseColor, kEffectPSPLightPositionX };

	/**
	 * @brief The Effect port's material kind (MaterialRecord::kind, LightingDescriptors::port, MaterialSnapshot::port): kind 1, the first
	 * port registered (Core's Lighting is 0), which the feature's registration checks (EffectMaterial.cpp, RegisterPort).
	 */
	inline constexpr std::uint8_t kMaterialKindEffect = 1;

	/*
	 * The effect pass descriptor's bits (BSEffectShaderProperty::GetRenderPasses' FUN_1414bde90; Community Shaders' ShaderCache.h
	 * EffectShaderFlags), and the engine's pass enum base (passEnum - 0x4000002C is the descriptor SetupTechnique receives).
	 */
	namespace EffectFlag
	{
		inline constexpr std::uint32_t Vc = 1u << 0;
		inline constexpr std::uint32_t TexCoord = 1u << 1;
		inline constexpr std::uint32_t TexCoordIndex = 1u << 2;
		inline constexpr std::uint32_t Skinned = 1u << 3;
		inline constexpr std::uint32_t Normals = 1u << 4;
		inline constexpr std::uint32_t BinormalTangent = 1u << 5;
		inline constexpr std::uint32_t Texture = 1u << 6;
		inline constexpr std::uint32_t IndexedTexture = 1u << 7;
		inline constexpr std::uint32_t Falloff = 1u << 8;
		inline constexpr std::uint32_t AddBlend = 1u << 10;
		inline constexpr std::uint32_t MultBlend = 1u << 11;
		inline constexpr std::uint32_t Particles = 1u << 12;
		inline constexpr std::uint32_t StripParticles = 1u << 13;
		inline constexpr std::uint32_t Blood = 1u << 14;
		inline constexpr std::uint32_t Membrane = 1u << 15;
		inline constexpr std::uint32_t Lighting = 1u << 16;
		inline constexpr std::uint32_t ProjectedUv = 1u << 17;
		inline constexpr std::uint32_t Soft = 1u << 18;
		inline constexpr std::uint32_t GrayscaleToColor = 1u << 19;
		inline constexpr std::uint32_t GrayscaleToAlpha = 1u << 20;
		inline constexpr std::uint32_t IgnoreTexAlpha = 1u << 21;
		inline constexpr std::uint32_t MultBlendDecal = 1u << 22;
		inline constexpr std::uint32_t AlphaTest = 1u << 23;
		inline constexpr std::uint32_t SkyObject = 1u << 24;
		inline constexpr std::uint32_t MsnSpuSkinned = 1u << 25;
		inline constexpr std::uint32_t MotionVectorsNormals = 1u << 26;
		inline constexpr std::uint32_t Deferred = 1u << 27;
	}
	inline constexpr std::uint32_t kEffectPassEnumBase = 0x4000002Cu;
	/** @brief The engine's pass enum of an effect pass descriptor (BSShader::MakeRenderPass's, GetRenderPasses' +0x4000002C). */
	inline constexpr std::uint32_t EffectPassEnum(std::uint32_t a_descriptor) { return a_descriptor + kEffectPassEnumBase; }

	/**
	 * @brief The Effect port's frame constant rows (Api::FrameConstantRow, PS b13; EffectFrame.cpp): c17 the camera's planes
	 * (SampleEffectCamera; Effect.hlsl, DCLFEffectCamera), c18 its Lighting variant's DLightColor (SampleEffectLight; DCLFEffectDLightColor),
	 * c19 Blood's AlphaTestRef.yz globals (SampleEffectBlood; DCLFEffectBlood).
	 */
	inline constexpr std::uint32_t kEffectCameraFrameRow = 17;
	inline constexpr std::uint32_t kEffectLightFrameRow = 18;
	inline constexpr std::uint32_t kEffectBloodFrameRow = 19;
	/**
	 * @brief t118 (kTextureRegisters - 10): the soft effects' depth (Api::FrameTexture; Effect.hlsl, DCLFEffectSoftDepth), which the
	 * engine's SetupTechnique binds at t3 for a Soft pass: the depth copy (kPOST_ZPREPASS_COPY, the main view's 0x14328cbfc). A render
	 * target, so the frame record names it (EffectFrame.cpp).
	 */
	inline constexpr std::uint32_t kSoftDepthRegister = 118;

	// The Effect family's record bits (BindlessObject::recordFlags, DCLFObjects.hlsli; Api::ProgramFamilyContribution::records):
	// External Emittance's suppress bit (DCLFSuppressExternalEmittance). Shares bit 3 with refraction: an effect record never has
	// kRecordRefraction.
	inline constexpr std::uint32_t kRecordEffectSuppressEmittance = 1u << 3;
	// A billboard effect (CS_DCLF_BILLBOARDS): bit 4, and NiBillboardNode's mode (userFlags & 7) in bits 5-7, by which Effect.hlsl turns
	// the leaf to the drawing camera (DCLFBillboardWorld). Shares bits 4-7 with refraction.
	inline constexpr std::uint32_t kRecordEffectBillboard = 1u << 4;
	inline constexpr std::uint32_t kRecordEffectBillboardModeShift = 5;

	/** @brief The effect material's texture-path bits a PropertyRecord holds in `feature` (the port's capture: EffectProperty.cpp). */
	inline constexpr std::uint32_t kEffectHasSourcePath = 1u << 0;
	inline constexpr std::uint32_t kEffectHasGreyscalePath = 1u << 1;

	// AE 1.6.1170 (module offsets) of BSEffectShader's state the port reads (EffectPrograms.cpp, EffectFrame.cpp).
	namespace Address
	{
		inline constexpr std::uintptr_t kEffectShader = 0x33dcce8;    // BSEffectShader*: the instance GetRenderPasses makes passes of
		inline constexpr std::uintptr_t kEffectCamera = 0x332a328;  // NiCamera*: BSEffectShader's camera (its near and far at +0x160, +0x164)
		inline constexpr std::uintptr_t kEffectBlood = 0x33dccf0;  // float[2]: BSEffectShader's blood globals
	}
}
