#pragma once

// Ports of the engine's pass and shader-descriptor selection: why an object stays native (Ineligible), the Lighting descriptors DCLF
// draws it with (a material port's are its derive's: Extensions::DerivePort), LOD fades and the particle path's acceptance. Pure
// functions of the capture records, called on the scene-work thread and its pool; the render thread samples the frame inputs
// (SampleLodFadeFrame).

#include <array>
#include <cstdint>
#include <string_view>

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

namespace DCLF::SceneCapture
{
	struct PropertyRecord;
	struct LeafView;
	struct AlphaRecord;
}

namespace DCLF
{
	/** @brief Why an object stays on the native render loop. */
	enum class Ineligible : std::uint8_t
	{
		None,
		NotTriShape,         // BSDynamicTriShape, LOD shapes, ...
		Skinned,             // has a skin instance
		NoRendererData,      // no GPU buffers yet
		NotLightingShader,   // effect, water, sky, grass, ...
		Technique,           // technique outside the supported set
		AlphaBlend,          // transparency
		Decal,               // decal batches
		ProjectedUV,         // snow/moss projection
		UnsupportedParent,   // a blended leaf under a BSOrderedNode (its opaque children are eligible)
		Hidden,              // app-culled or hidden this frame (per frame)
		Fading,              // fade node not fully faded in (per frame)
		Actor,               // part of an actor's 3D
		Lod,                 // object or landscape LOD
		UnstableBuffer,      // a buffer DXVK cannot make stable (the game can map it)
		SkinShape,           // a skin the skinned path does not take: dismember instance, several partitions, too many bones
		Billboard,           // under an NiBillboardNode (the main cull turns it to the camera)
		Switch,              // under an NiSwitchNode not selecting it (or its leaves not admitted: Features/SwitchNodes)
		Held,                // a CS feature redraws it after the opaque pass (Api::HeldGroupContribution): its group's toggle off, or a decal
		MultiIndex,          // BSMultiIndexTriShape: its main pass is the engine's (the snow layer), its shadow a tri-shape's
		EffectOpaque,        // CS_DCLF_EFFECTS: an effect without a blending alpha property
		EffectVariant,       // CS_DCLF_EFFECTS: an effect permutation not ported
		Count
	};

	constexpr std::array<std::string_view, static_cast<std::size_t>(Ineligible::Count)> kIneligibleNames{
		"eligible",
		"not-trishape",
		"skinned",
		"no-renderer-data",
		"not-lighting-shader",
		"technique",
		"alpha-blend",
		"decal",
		"projected-uv",
		"unsupported-parent",
		"hidden",
		"fading",
		"actor",
		"lod",
		"unstable-buffer",
		"skin-shape",
		"billboard",
		"switch",
		"terrain-no-blend",  // Held's: Integrations/TerrainBlending's groups, the only held ones (the census's text kept)
		"multi-index",
		"effect-opaque",
		"effect-variant",
	};

	struct LightingDescriptors
	{
		std::uint32_t technique = 0;  // descriptor bits 24-29, as GetRenderPasses selects it
		std::uint32_t pass = 0;       // pass descriptor (BSRenderPass::passEnum - 0x4800002D), before the VS/PS split
		std::uint32_t vertex = 0;     // final descriptors, as the native deferred draw looks the shaders up
		std::uint32_t pixel = 0;
		std::uint32_t rawVertex = 0;  // descriptors BeginTechnique receives, before State::ModifyShaderLookup
		std::uint32_t rawPixel = 0;
		std::uint32_t derivedPass = 0;  // the pass descriptor derived from the property (kNotDerived if it cannot be)
		// Which technique produced Ineligible::Technique (kRefractionReject for the refraction rejection).
		std::uint32_t rejectedTechnique = 62;  // 62 = never set, so an empty field cannot read as `none`
		// CS_DCLF_DECALS: 0, or the decal group (1 = hint 2, opaque; 2 = hint 3, blended) with alphaBlendMode (FUN_14150bc80) and
		// alphaBlendWriteMode as the group function and SetupGeometry leave them. The depth-bias mode is frame state (Coordinator).
		std::uint32_t decalGroup = 0;
		std::uint32_t decalBlendMode = 0;
		std::uint32_t decalWriteMode = 0;
		// An opaque object (hint 0, not 2) drawn through decal group 1 without depth bias (CS_DCLF_OPAQUE_EFFECTS: an opaque effect).
		bool opaqueInDecalGroup = false;
		// CS_DCLF_TB_TERRAIN under Terrain Blending: 0, or the terrain group (1 kMultiTextureLandscape, 2 kNoTransparencyMultiSample),
		// as TerrainBlending.cpp's BSBatchRenderer__RenderPassImmediately hook sorts them.
		std::uint32_t terrainGroup = 0;
		// CS_DCLF_FORWARD: a blended object the forward epoch draws, with the engine's alphaBlendMode (FUN_14150bc80) and depth flags.
		bool forward = false;
		std::uint32_t forwardBlendMode = 0;
		bool forwardDepthTest = false, forwardDepthWrite = false;
		// CS_DCLF_FIRST_PERSON_DECALS: a first-person effect decal drawn forward: its decal group (1 or 2, bias in kRasterDepthBiasShift).
		std::uint32_t forwardDecalGroup = 0;
		// CS_DCLF_REFRACTION: drawn as GetRenderPasses' Utility RENDER_NORMAL pass (also `forward`), variants RENDER_NORMAL_FALLOFF
		// (kRefractionFalloff, bit 16) and RENDER_NORMAL_CLAMP (kNonProjectiveShadows, bit 13). Its Lighting descriptors are never drawn.
		bool refraction = false;
		bool refractionFalloff = false, refractionClamp = false;
		// A material port's object (its kind, Extensions::MaterialPorts; 0: Lighting's): pass and descriptors are its shader's, technique 0
		// (CS_DCLF_EFFECTS: the Effect port's, Features/Effects).
		std::uint8_t port = 0;
		// ProjectedUV (CS_DCLF_PROJECTED_UV): needs the per-object texture matrix, pixel parameters and projected textures.
		bool projectedUV = false;
	};

	/** @brief LightingDescriptors::rejectedTechnique for the refraction rejection, which has no technique. */
	inline constexpr std::uint32_t kRefractionReject = 63;

	/** @brief LightingDescriptors::derivedPass when the derivation leaves the object native. */
	inline constexpr std::uint32_t kNotDerived = ~0u;

	/** @brief FadeStateOf: the object has a fade-sensitive flag but no fade node. */
	inline constexpr std::uint8_t kFadeNoNode = 0x40;

	/** @brief kFadeNoNode (stays native) or 0. The LOD metric is not read: its fades are the draw's, so it is camera-independent. */
	std::uint8_t FadeStateOf(const SceneCapture::PropertyRecord* a_property);

	/**
	 * @brief Pass bits GetRenderPasses sets from per-frame engine state, not the property: ShadowDir (13), DefShadow (14) and the
	 * shadow light count (6-8) from the light assignment, and DoAlphaTest (20), which for alpha-tested geometry also follows the
	 * early-Z global other renders toggle (engine notes: GetRenderPasses runtime bits).
	 */
	inline constexpr std::uint32_t kRuntimePassBits = 0x1061c0u;
	/**
	 * @brief The bits a registered pass supplies over the derivation: the runtime bits, the screen-door fade (AdditionalAlphaMask,
	 * 23) and the snow conditions' bits 19 and 21.
	 */
	inline constexpr std::uint32_t kRegisteredPassBits = (1u << 20) | (1u << 23) | (1u << 19) | (1u << 21);
	/**
	 * @brief The pass's light-independent shadow bits (GetRenderPasses, AE 0x1414adfb0): ShadowDir where the sun's shadow may apply,
	 * DefShadow where deferred shadows do, and for DefShadow alone one shadow light in the count, which makes the technique bind the
	 * shadow mask. BuildDrawsCS decides per frame (kObjectSunTest, local shadow lights). a_property: a layer's property, else null.
	 */
	std::uint32_t StaticShadowBits(const SceneCapture::LeafView& a_leaf, bool a_settled, const SceneCapture::PropertyRecord* a_property = nullptr);
	/** @brief A multi-index shape's layer passes' accumulation hint (FUN_1414b2330), which their pass draw reads (FUN_1414f2ad0). */
	inline constexpr std::uint32_t kLayerHint = 12;
	/**
	 * @brief A BSMultiIndexTriShape's additional property, as the main registration takes it (FUN_1414b2330: any additional
	 * property; its passes draw the second index list). Null otherwise, or without a second list.
	 */
	RE::BSShaderProperty* LayerPropertyOf(const RE::BSGeometry& a_geometry);
	inline constexpr std::uint32_t kShadowBits = 0x61c0u;  // ShadowDir, DefShadow and the shadow light count

	/**
	 * @brief Inputs from which the draw (Lighting.hlsl, DCLFLodFade) computes the specular and envmap LOD fades BSFadeNode::OnVisible
	 * (FUN_14147b110) and GetRenderPasses (FUN_14147c470) give. DCLF keeps both features at any distance; past a fade's end the factor
	 * is 0, matching a dropped feature (engine notes: LOD fades). Uploaded once an epoch after the frame lighting (PS b13, c6-c12).
	 */
	struct LodFadeFrame
	{
		float eye[3]{};           // the main camera's world position
		float lodAdjust = 1.0f;   // its NiCamera::lodAdjust
		float specularStart = 0.0f, specularEnd = 0.0f, envmapStart = 0.0f, envmapEnd = 0.0f;
		float metricScale = 0.0f;     // the metric per scaled distance (0x141aa6300)
		float defaultScale = 0.0f;    // the distance scale when the LOD type's divisor is not positive (0x141ad2840)
		float metricOverride = 0.0f;  // the metric every node takes instead, when overridden (0x14332a254 against 0x141769578)
		float overridden = 0.0f;      // 1 when metricOverride applies
		float divisors[16]{};         // per LOD type (+0x153 & 0xF): the distance scales by lodAdjust / divisor (0x142032e00)
		// 1 when BSFadeNode::OnVisible updates the metric (0x142032dfd); otherwise nodes keep their fades.
		float fadesOn = 0.0f;
		// GetRenderPasses' refraction LOD fade thresholds (Utility.hlsl, DCLF_REFRACTION).
		float refractionStart = 0.0f, refractionEnd = 0.0f;
		float pad = 0.0f;
	};
	static_assert(sizeof(LodFadeFrame) == 128);
	/** @brief This frame's, from the main camera and the engine's settings. Render thread. */
	LodFadeFrame SampleLodFadeFrame();
	/** @brief The draw's LOD fade word (DCLFLodFadeFlagsOf, Common/DCLFObjects.hlsli): LOD type and the fades applied. */
	inline constexpr std::uint32_t kLodFadeTypeMask = 0xFu;
	inline constexpr std::uint32_t kLodFadeSpecular = 1u << 4;  // MaterialData.y, and SSRParams.w with kLodFadeSsr
	inline constexpr std::uint32_t kLodFadeEnvmap = 1u << 5;    // MaterialData.x
	inline constexpr std::uint32_t kLodFadeSsr = 1u << 6;       // the pass writes SSRParams.w (render flag 2 clear)
	/**
	 * @brief The fade node's world bound centre and LOD type in w, plus kLodFadeHeld when the fades do not apply (no fade-sensitive
	 * flag, or kIgnoreFade, which BSFadeNode::OnVisible skips). w < 0: no fade node.
	 */
	std::array<float, 4> LodFadeNodeOf(const RE::BSShaderProperty* a_property);
	inline constexpr float kLodFadeHeld = 16.0f;
	/** @brief Whether a LodFadeNodeOf row has the draw fade the property's LOD fades. */
	inline bool LodFadesApply(const std::array<float, 4>& a_node) { return a_node[3] >= 0.0f && a_node[3] < kLodFadeHeld; }

	/** @brief Pass descriptor of a BSRenderPass (passEnum minus the Lighting shader's base). */
	inline constexpr std::uint32_t PassDescriptorOf(std::uint32_t a_passEnum) { return a_passEnum - 0x4800002Du; }

	/** @brief Technique GetRenderPasses selects from the property flags (engine notes: technique table). */
	std::uint32_t SelectLightingTechnique(std::uint64_t a_flags);
	/**
	 * @brief The descriptor BSLightingShader::SetupTechnique (AE 0x1414db810) draws a pass with: LODLandNoise (18) is LODLand (9)
	 * unless the land noise global (0x142032fdb) is set, and ParallaxOcc (7) is None (0) unless the parallax occlusion global
	 * (0x142035500) is. Its shaders, samplers and what SetupGeometry reads (+0x94) are that technique's.
	 */
	std::uint32_t SetupTechniqueDescriptor(std::uint32_t a_pass);
	/** @brief SetupTechnique's VS/PS split, then State::ModifyShaderLookup; the pixel Deferred bit only when a_deferred. */
	void LightingShaderDescriptors(std::uint32_t a_pass, bool a_deferred, std::uint32_t& a_vertex, std::uint32_t& a_pixel);
	/** @brief The pixel descriptor's Deferred bit (ShaderCache.h, LightingShaderFlags::Deferred): the G-buffer variant. */
	inline constexpr std::uint32_t kLightingPixelDeferred = 1u << 4;
	/** @brief Whether the technique is one the water reflection's cube map draws (dclf-architecture.md, "The census"). */
	inline constexpr bool LodLightingTechnique(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t technique = (a_passDescriptor >> 24) & 0x3f;
		return technique == 9 || technique == 13 || technique == 15 || technique == 18;
	}
	/** @brief The Lighting techniques by id, and kRefractionReject's name. */
	constexpr std::array<std::string_view, 20> kLightingTechniqueNames{ "none", "envmap", "glowmap", "parallax", "facegen",
		"facegenRGBTint", "hair", "parallaxOcc", "MTLand", "LODLand", "snow", "multilayerParallax", "treeAnim", "LODObjects",
		"multiIndexSparkle", "LODObjectHD", "eye", "cloud", "LODLandNoise", "MTLandLODBlend" };
	inline std::string_view LightingTechniqueName(std::uint32_t a_technique)
	{
		return a_technique == 63 ? std::string_view("refraction") : a_technique < kLightingTechniqueNames.size() ? kLightingTechniqueNames[a_technique] : std::string_view("?");
	}

	/**
	 * @brief A member's main pass built from the object alone (PrimaryCull::MembershipPass): what GetRenderPasses would register.
	 */
	struct AccumulatedPass
	{
		std::uint32_t technique = 0;  // pass descriptor (key minus the Lighting base): the batch group's key, what SetupTechnique receives
		std::uint32_t subPass = 0;    // PassGroup list 0-4; the renderer draws 1, 3 and 4 with alpha testing
		std::uint32_t passEnum = 0;   // the pass's own passEnum when the tables were built (diagnostics)
		// Accumulation hint (BSRenderPass+0x1C): the batch renderer's geometry group, and for decals which of the two decal groups.
		std::uint32_t hint = 0;
		// LODMode as a row of the engine's partition table (index + singleLevel * 4): which skin partitions the main camera draws.
		std::uint32_t lodRow = 3;
		// A synthetic pass whose technique carries the sun's bits for the GPU to drop on a cascade miss (kObjectSunTest).
		bool sunTest = false;
		// A resident object's pass: the accumulate phase patches its record once and keeps the patch across frames.
		bool resident = false;
		// A resident pass under a fade root: the distance BuildDraws tests (kObjectFadeTest); 0 when the root's fade needs no test.
		float fadeDistance = 0.0f;
		// A resident tree's pass: BuildDraws applies BSTreeNode::OnVisible's height test (kObjectHeightTest).
		bool heightTest = false;
	};

	/**
	 * @brief A membership pass's derived pass descriptor with the inputs it came from (property record's key, material, flags,
	 * fade state); derived again when any differs. Lives in the geometry's tracked entry (Coordinator::Tracked::membershipDerived).
	 */
	struct MembershipDerived
	{
		const void* property = nullptr;
		const void* material = nullptr;
		std::uint64_t flags = 0;
		std::uint8_t fadeState = 0;
		std::uint32_t derivedPass = kNotDerived;

		bool operator==(const MembershipDerived&) const = default;
	};

	inline constexpr std::uint32_t kPassDoAlphaTest = 1u << 20;           // pass descriptor DoAlphaTest
	inline constexpr std::uint32_t kPassAdditionalAlphaMask = 1u << 23;  // pass descriptor AdditionalAlphaMask (screen-door fade)
	inline constexpr std::uint32_t kPassProjectedUV = 1u << 15;          // pass descriptor ProjectedUV

	/**
	 * @brief Hair (6) with ProjectedUV: SetupGeometry writes the projected-UV constants (VS TextureProj, PS ProjectedUVParams 1-3) for
	 * every technique but Hair, so the native draw uses stale ones (dclf-validation.md, "In the engine"). DCLF draws such hair without projection.
	 */
	inline bool HairProjection(std::uint32_t a_descriptor)
	{
		return ((a_descriptor >> 24) & 0x3f) == 6 && (a_descriptor & kPassProjectedUV) != 0;
	}

	/**
	 * @brief The descriptor DCLF draws a pass in batch list a_subPass with. The renderer alpha-tests lists 1, 3 and 4 whatever the
	 * technique says (engine notes, batch renderer), and GetRenderPasses' DoAlphaTest comes and goes with the early-Z global, blending
	 * and the last build's fade while the pixels do not (EQUAL depth against an alpha-tested prepass), so DCLF always adds the discard
	 * there, keeping the object DCLF's every frame.
	 */
	inline std::uint32_t DrawnPassDescriptor(std::uint32_t a_descriptor, std::uint32_t a_subPass)
	{
		if (HairProjection(a_descriptor))
			a_descriptor &= ~kPassProjectedUV;
		return (a_subPass == 1 || a_subPass == 3 || a_subPass == 4) ? (a_descriptor | kPassDoAlphaTest) : a_descriptor;
	}

	/** @brief The engine's alphaBlendWriteMode for the blended group (FUN_1414b3390 sets 11 before FUN_1414b39d0's draws). */
	inline constexpr std::uint32_t kForwardWriteMode = 11;

	/**
	 * @brief The native main pass's descriptors for this geometry, derived without running GetRenderPasses; returns why the object
	 * stays native, or None. Camera-independent. Reads the records (the mirror's on the scene work, LiveLeaf's elsewhere).
	 *
	 * @param a_accumulated This frame's accumulated pass, or null; supplies kRegisteredPassBits.
	 * @param a_layer a_property is the geometry's layer (LayerPropertyOf), drawn with hint 12.
	 */
	Ineligible DeriveLightingDescriptors(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf,
		const AccumulatedPass* a_accumulated, LightingDescriptors& a_out, bool a_layer = false);

	/**
	 * @brief alphaBlendMode as BSShader's alpha setup (AE FUN_14150bc80) picks it from a blending NiAlphaProperty's functions, or
	 * 0 for a pair it leaves alone. (Its mode 1 for a non-blending property with alpha below 1 is not modelled: not eligible.) The Lighting
	 * derive's and the material ports' (the Effect port's forward and decal groups).
	 */
	std::uint32_t AlphaBlendModeOf(const SceneCapture::AlphaRecord& a_alpha);

	/** @brief A particle geometry's type (BSGeometry::Type kParticles or kStripParticles: NiParticles, drawn from its particle data). */
	inline constexpr bool ParticleGeometryType(std::uint8_t a_type) { return a_type == 1 || a_type == 2; }
	/**
	 * @brief The palette rows class a member's block is its geometry type's (Api::GeometryTypeContribution::itemClass, while the
	 * contribution's toggle is on; None for none): a skinless leaf whose capture is a complete particle system (dclf-architecture.md,
	 * "Particles"), exactly an NiParticleSystem with particle data, local or world space (a world-space system's transform is the identity
	 * with its parent's scale, FUN_140dd4db0), for aspect flags (FUN_140e51320's other path) an NiPSysData with velocities, and a capacity
	 * (the block's units).
	 */
	Api::FrameValueItemClass PaletteItemClassOf(const SceneCapture::LeafView& a_leaf);
}
