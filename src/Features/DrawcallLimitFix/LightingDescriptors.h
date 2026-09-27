#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "Records.h"  // ObjectTreeAnim

namespace DCLF
{
	/** @brief Why an object stays on the native render loop. */
	enum class Ineligible : std::uint8_t
	{
		None,
		NotTriShape,         // BSDynamicTriShape, BSMultiIndexTriShape, LOD shapes, ...
		Skinned,             // has a skin instance
		NoRendererData,      // no GPU buffers yet
		NotLightingShader,   // effect, water, sky, grass, ...
		Technique,           // technique outside the supported set
		AlphaBlend,          // transparency
		Decal,               // decal batches
		ProjectedUV,         // snow/moss projection
		UnsupportedParent,   // under a BSOrderedNode
		Hidden,              // app-culled or hidden this frame (per frame)
		Fading,              // fade node not fully faded in (per frame)
		Actor,               // part of an actor's 3D (carried items; interiors keep actors in the rooms)
		Lod,                 // object or landscape LOD
		UnstableBuffer,      // a vertex or index buffer DXVK cannot make stable (the game can map it)
		SkinShape,           // skinned, but not a shape the skinned path takes: dismember instance, several partitions, too many bones
		Billboard,           // under an NiBillboardNode: the main cull turns it to the camera
		Switch,              // under an NiSwitchNode that does not select it this frame (or CS_DCLF_SWITCH_NODES off)
		TerrainNoBlend,      // Terrain Blending redraws it after its terrain (kNoTransparencyMultiSample)
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
		"terrain-no-blend",
	};

	struct LightingDescriptors
	{
		std::uint32_t technique = 0;  // descriptor bits 24-29, as GetRenderPasses selects it
		std::uint32_t pass = 0;       // pass descriptor (BSRenderPass::passEnum - 0x4800002D), before the VS/PS split
		std::uint32_t vertex = 0;     // final descriptors, as the native deferred draw looks the shaders up
		std::uint32_t pixel = 0;
		std::uint32_t rawVertex = 0;  // descriptors BeginTechnique receives, before State::ModifyShaderLookup
		std::uint32_t rawPixel = 0;
		float specularLODFade = 1.0f;  // what GetRenderPasses stores in the property for the PS constants
		float envmapLODFade = 1.0f;
		std::uint32_t derivedPass = 0;  // the pass descriptor derived from the property (kNotDerived if it cannot be)
		float derivedSpecularLODFade = 1.0f;  // the LOD fades the derivation computed, beside the property's (derivedPass)
		float derivedEnvmapLODFade = 1.0f;
		// Which technique produced Ineligible::Technique, so coverage can be widened against a histogram
		// rather than a guess. kRefractionReject marks the refraction rejection, which is not a technique.
		std::uint32_t rejectedTechnique = 62;  // 62 = never set, so an empty field cannot read as `none`
		// CS_DCLF_DECALS: 0 for anything that is not a decal, else the decal group (1 = accumulation hint
		// 2, the engine's opaque decal group; 2 = hint 3, the blended one) and the alpha-property half of
		// the engine's fixed-function state for it: alphaBlendMode as BSShader's alpha setup
		// (FUN_14150bc80) picks it from the blend functions, and alphaBlendWriteMode as the group function
		// and SetupGeometry leave it. The depth-bias mode is frame state and SceneStore adds it.
		std::uint32_t decalGroup = 0;
		std::uint32_t decalBlendMode = 0;
		std::uint32_t decalWriteMode = 0;
		// The pass descriptor's ProjectedUV bit (CS_DCLF_PROJECTED_UV): the object needs the per-object
		// texture matrix and pixel parameters, and the projected textures on its pipeline.
		bool projectedUV = false;
	};

	/** @brief LightingDescriptors::rejectedTechnique for the refraction rejection, which has no technique. */
	inline constexpr std::uint32_t kRefractionReject = 63;

	/** @brief LightingDescriptors::derivedPass when the derivation leaves the object native. */
	inline constexpr std::uint32_t kNotDerived = ~0u;

	/** @brief FadeStateOf: the object has a fade-sensitive flag but no fade node, or a non-finite metric. */
	inline constexpr std::uint8_t kFadeNoNode = 0x40;
	inline constexpr std::uint8_t kFadeInvalid = 0x80;

	/**
	 * @brief The whole camera dependence of the classification, in one byte.
	 *
	 * Two bits say whether the specular and environment-map LOD fades have run out, which is all the fade
	 * metric contributes to the derived technique. An object with none of the fade-sensitive flags has no
	 * camera dependence and returns 0 without reading the fade node. Comparing this per frame is what lets
	 * the rest of the classification be cached.
	 */
	std::uint8_t FadeStateOf(const RE::BSShaderProperty* a_property);
	/** @brief FadeStateOf's reading of the fade node alone: its LOD metric past the specular and envmap fade ends, or kFadeInvalid. */
	std::uint8_t LodFadeStateOf(const RE::BSFadeNode* a_fadeNode);
	/** @brief Whether the property's derivation reads its fade node's LOD metric (FadeStateOf is not constant for it). */
	bool FadeSensitive(const RE::BSShaderProperty* a_property);

	/**
	 * @brief Pass descriptor bits GetRenderPasses sets from per-frame engine state rather than from the
	 * property: ShadowDir (13), DefShadow (14) and the shadow light count (6-8) from the light and shadow
	 * assignment, and DoAlphaTest (20), which for alpha-tested geometry also depends on the early-Z
	 * global other renders toggle (engine notes: GetRenderPasses runtime bits).
	 */
	inline constexpr std::uint32_t kRuntimePassBits = 0x1061c0u;

	/** @brief Pass descriptor of a BSRenderPass (passEnum minus the Lighting shader's base). */
	inline constexpr std::uint32_t PassDescriptorOf(std::uint32_t a_passEnum) { return a_passEnum - 0x4800002Du; }

	/** @brief Re-reads the [LightingShader] LOD fade settings (once per frame). */
	void RefreshLodFadeSettings();

	/** @brief Technique GetRenderPasses selects from the property flags (engine notes: technique table). */
	std::uint32_t SelectLightingTechnique(std::uint64_t a_flags);

	/**
	 * @brief Derives the vertex and pixel descriptors the native main (deferred) pass uses for this
	 * geometry, without running GetRenderPasses. Returns the reason when Phase 1 does not handle it.
	 */
	/**
	 * @brief Where the main-camera accumulator holds a geometry's lighting pass this frame.
	 * technique is the batch group's key, the pass descriptor SetupTechnique receives (it can differ from
	 * the pass's own passEnum: the accumulator adds DoAlphaTest when it registers the pass).
	 */
	struct AccumulatedPass
	{
		const RE::BSRenderPass* pass = nullptr;
		std::uint32_t technique = 0;  // pass descriptor (key minus the Lighting base)
		std::uint32_t subPass = 0;    // PassGroup list 0-4; the renderer draws 1, 3 and 4 with alpha testing
		std::uint32_t passEnum = 0;   // the pass's own passEnum when the tables were built (diagnostics)
		// The pass's accumulation hint (BSRenderPass+0x1C): which geometry group of the batch renderer it
		// was drawn from, and for decals which of the two decal groups. Kept because the pass pointer is
		// not dereferenced again once the tables are built.
		std::uint32_t hint = 0;
		// Where in its pass-group chain the pass sits, so that decals can be drawn in the engine's order
		// (group, technique bucket, list, chain) rather than in whatever order the culling appends.
		std::uint32_t chainIndex = 0;
		// Whether the object's fade was the native loop's when the pass was registered
		// (PassCapture::FadingAtRegistration: a hint-10 pass, a blended fade, or any fade with CS_DCLF_FADING
		// off) - the same moment the native loop was or was not told to leave it to DCLF (PassCapture::Withhold).
		// The accumulate phase's fading verdict reads this rather than the fade node later in the frame, so the
		// two decisions cannot disagree.
		bool fading = false;
		// The pass's LODMode as a row of the engine's partition table (index + singleLevel * 4): which skin
		// partitions the main camera draws (SceneStore::SkinPartitionMask).
		std::uint32_t lodRow = 3;
		// A synthetic pass (PrimaryCull) whose technique carries the sun's bits for the GPU to drop on a cascade
		// miss (kObjectSunTest); the engine's registered passes carry the engine's own bits.
		bool sunTest = false;
		// A resident object's pass (PrimaryCull, dclf-cull-job-elimination.md "Phase 4 in detail"): the accumulate phase
		// patches its record once and keeps the patch across frames, instead of restoring it at the next walk.
		bool resident = false;
		// A resident pass under a fade root: the distance BuildDraws tests (kObjectFadeTest, PrimaryCull::FadeDistanceOf);
		// 0 when the root's fade needs no test.
		float fadeDistance = 0.0f;
		// A resident tree's pass: BuildDraws applies BSTreeNode::OnVisible's height test (kObjectHeightTest).
		bool heightTest = false;
	};

	inline constexpr std::uint32_t kPassDoAlphaTest = 1u << 20;           // pass descriptor DoAlphaTest
	inline constexpr std::uint32_t kPassAdditionalAlphaMask = 1u << 23;  // pass descriptor AdditionalAlphaMask (screen-door fade)

	/**
	 * @brief The pass descriptor DCLF draws a pass registered with a_descriptor in batch list a_subPass with.
	 *
	 * The renderer draws lists 1, 3 and 4 with alpha testing, whatever the registered technique says (engine
	 * notes, batch renderer). The technique's DoAlphaTest bit is not a property of the object:
	 * BSLightingShaderProperty::GetRenderPasses sets it for an alpha-tested property only while the early-Z
	 * global is set, or the object is alpha-blended, or its alpha times its fade is below one for the camera
	 * the passes were last built for - and it keeps that build until the state changes. So it comes and goes
	 * from frame to frame while the pixels on screen do not: the native main pass tests depth EQUAL against
	 * an alpha-tested prepass either way. DO_ALPHA_TEST only adds the discard, so DCLF draws every pass in an
	 * alpha-test list with it, which keeps the object DCLF's in every frame. It used to leave the frames
	 * without the bit native (alpha-test-state), and each switch between the two left a frame in which the
	 * native loop had been told not to draw the object and DCLF did not either.
	 */
	inline constexpr std::uint32_t kPassProjectedUV = 1u << 15;  // pass descriptor ProjectedUV

	/**
	 * @brief The Hair technique (6) with ProjectedUV: BSLightingShader::SetupGeometry writes the projected-UV
	 * constants (VS TextureProj, PS ProjectedUVParams 1-3) for every technique but Hair, so the native draw shades
	 * the projection from whatever the previous projected draw left in them - another object's snow or moss
	 * (docs/development/bugs-found-by-parity.md). DCLF draws such hair without the projection.
	 */
	inline bool HairProjection(std::uint32_t a_descriptor)
	{
		return ((a_descriptor >> 24) & 0x3f) == 6 && (a_descriptor & kPassProjectedUV) != 0;
	}

	inline std::uint32_t DrawnPassDescriptor(std::uint32_t a_descriptor, std::uint32_t a_subPass)
	{
		if (HairProjection(a_descriptor))
			a_descriptor &= ~kPassProjectedUV;
		return (a_subPass == 1 || a_subPass == 3 || a_subPass == 4) ? (a_descriptor | kPassDoAlphaTest) : a_descriptor;
	}

	/**
	 * @param a_accumulated The geometry's accumulated pass this frame (SceneStore::FindAccumulatedPass), or
	 * null. When present its technique is the pass descriptor (it is what the draw uses); the derivation
	 * from the property is kept in derivedPass for comparison. Without it the derivation stands, with its
	 * guesses for the kRuntimePassBits.
	 */
	/**
	 * @param a_wantDerived Compute derivedPass even when the accumulator supplies the real one.
	 *
	 * For an object the accumulator holds, the property derivation's ONLY surviving effect is
	 * derivedPass, which nothing but the CS_DCLF_DERIVE_PROBE diagnostic reads: the pass descriptor comes
	 * from the accumulated technique, the flag edits the derivation makes are not used afterwards, and
	 * the two LOD fades it computes are overwritten from the property two lines later. Skipping it there
	 * removes the fade metric, SelectLightingTechnique, ten flag tests and a virtual GetFeature() call
	 * from every eligible object, every frame.
	 */
	/**
	 * @brief The tree-animation constants for one object, as BSLightingShader::SetupGeometry computes
	 *        them (engine notes: Func6 at 1414dd040, case 0xc).
	 *
	 * Reads the BSTreeNode the property's fade node downcasts to; leaves the engine's defaults when
	 * there is none. It deliberately does NOT perform SetupGeometry's write back of
	 * previousWindTimer = windTimer: the native draw still does that, and doing it here as well would
	 * advance every tree's animation twice a frame.
	 */
	void DeriveTreeAnim(const RE::BSShaderProperty& a_property, ObjectTreeAnim& a_out);

	/** @brief CS_DCLF_DECALS=1: decals (accumulation hints 2 and 3) are eligible, drawn by the second pass. */
	bool DecalsEnabled();

	/** @brief CS_DCLF_SKINNED=1: single-partition NiSkinInstance shapes are eligible, palettes from the engine. */
	bool SkinnedEnabled();

	/**
	 * @brief CS_DCLF_SWITCH_NODES=1: a leaf under an NiSwitchNode is eligible in the frames every switch on its
	 * path selects it (SceneStore::SwitchSelects). Trees and harvestables hang under one.
	 */
	bool SwitchNodesEnabled();

	/**
	 * @brief CS_DCLF_SKIN_PARTITIONS=1 (with skinned): skins of several partitions are eligible - the LOD
	 * partitions of trees and the dismember partitions of actor bodies - drawn one draw per partition the
	 * engine would draw (SkinPartitionMask).
	 */
	bool SkinPartitionsEnabled();

	/** @brief CS_DCLF_ACTORS=1: geometry under an actor's 3D is eligible, and the FacegenRGBTint technique (skin). */
	bool ActorsEnabled();

	/**
	 * @brief CS_DCLF_FADING=1: an object fading in or out is eligible while the engine fades it with the
	 * screen-door mask (pass descriptor AdditionalAlphaMask, the fade in MaterialData.z) in an opaque group.
	 * Blended fades (accumulation hint 9) and the LOD cross-fade copies (hint 10) stay native
	 * (PassCapture::FadingAtRegistration).
	 */
	bool FadingEnabled();

	/**
	 * @brief CS_DCLF_LOD_CROSSFADE=1: an object in a LOD cross-fade stays DCLF's - its own pass draws the new
	 * level as a settled object's does - and only the engine's hint-10 copy of the old level is left to the
	 * native loop. Off, the whole object is the native loop's until the crossing ends.
	 */
	bool LodCrossfadeEnabled();

	/** @brief CS_DCLF_PROJECTED_UV=1: kProjectedUV objects (snow and moss projection) are eligible. */
	bool ProjectedUvEnabled();

	/**
	 * @brief CS_DCLF_MTLAND=1: the MTLand and MTLandLODBlend techniques (terrain) are eligible - unless
	 * Terrain Blending is on and DCLF is drawing into the frame, because that feature intercepts every
	 * terrain pass and redraws it blended with its own depth state, which an opaque owned draw would break.
	 * Off the hybrid path the tables and their parity still exercise the derivation.
	 */
	bool MtLandEnabled();
	/** @brief Whether Terrain Blending draws its terrain after the opaque pass, into a frame DCLF draws in. */
	bool TerrainBlendingDefersTerrain();

	Ineligible DeriveLightingDescriptors(const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry,
		const AccumulatedPass* a_accumulated, LightingDescriptors& a_out, bool a_wantDerived = true);
}
