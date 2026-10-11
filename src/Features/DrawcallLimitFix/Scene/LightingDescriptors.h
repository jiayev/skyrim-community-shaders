#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "Records.h"  // ObjectTreeAnim

namespace DCLF::SceneCapture
{
	struct PropertyRecord;
	struct LeafView;
}

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
		MultiIndex,          // BSMultiIndexTriShape otherwise eligible: its main pass is the engine's (the snow layer), its shadow a tri-shape's
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
		"multi-index",
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

	/** @brief FadeStateOf: the object has a fade-sensitive flag but no fade node. */
	inline constexpr std::uint8_t kFadeNoNode = 0x40;

	/**
	 * @brief What the classification reads of the property's fading, in one byte: kFadeNoNode when it has a
	 * fade-sensitive flag (specular, envmap) but no fade node, which leaves it native; 0 otherwise. The LOD metric is not
	 * read: the fades it produces are the draw's (LodFadeFrame), so the derived technique does not depend on the camera.
	 */
	std::uint8_t FadeStateOf(const SceneCapture::PropertyRecord* a_property);

	/**
	 * @brief Pass descriptor bits GetRenderPasses sets from per-frame engine state rather than from the
	 * property: ShadowDir (13), DefShadow (14) and the shadow light count (6-8) from the light and shadow
	 * assignment, and DoAlphaTest (20), which for alpha-tested geometry also depends on the early-Z
	 * global other renders toggle (engine notes: GetRenderPasses runtime bits).
	 */
	inline constexpr std::uint32_t kRuntimePassBits = 0x1061c0u;
	/**
	 * @brief The bits a registered pass supplies over the derivation (DeriveLightingDescriptors): the runtime bits, the
	 * screen-door fade (AdditionalAlphaMask, 23) and the snow conditions' bits 19 and 21, which the derivation does not set.
	 */
	inline constexpr std::uint32_t kRegisteredPassBits = (1u << 20) | (1u << 23) | (1u << 19) | (1u << 21);
	/**
	 * @brief The shadow bits of the object's pass, as far as they do not depend on the frame's lights (GetRenderPasses, AE
	 * 0x1414adfb0): ShadowDir where the sun's shadow may apply, DefShadow where deferred shadows do, and where there is
	 * DefShadow without ShadowDir one shadow light in the count, which is what makes the technique bind the shadow mask (the
	 * pixel stage drops the count). The draw then decides per frame (BuildDrawsCS): ShadowDir when its bound meets a
	 * cascade (kObjectSunTest), DefShadow when that or a local shadow light reaches it (LocalShadowLights).
	 * a_property: the property whose pass it is (a multi-index layer's), null for the geometry's own. T6b1b: the records' (the mirror's
	 * on the scene work), the fade node's currentFade and screen-door byte its record's.
	 */
	std::uint32_t StaticShadowBits(const SceneCapture::LeafView& a_leaf, bool a_settled, const SceneCapture::PropertyRecord* a_property = nullptr);
	/** @brief A multi-index shape's layer passes' accumulation hint (FUN_1414b2330), which their pass draw reads (FUN_1414f2ad0). */
	inline constexpr std::uint32_t kLayerHint = 12;
	/**
	 * @brief A geometry's main-pass layer: a BSMultiIndexTriShape's additional property, as the main registration takes it
	 * (FUN_1414b2330: any additional property; the second index list is what its passes draw). Null for anything else, or
	 * when there is no second list to draw.
	 */
	RE::BSShaderProperty* LayerPropertyOf(const RE::BSGeometry& a_geometry);
	inline constexpr std::uint32_t kShadowBits = 0x61c0u;  // ShadowDir, DefShadow and the shadow light count

	/**
	 * @brief The main camera's specular and envmap LOD fades, computed by the draw (Lighting.hlsl, DCLFLodFade) from what
	 * BSFadeNode::OnVisible's fade value (FUN_14147b110) and GetRenderPasses' fade (FUN_14147c470) read. A DCLF object's
	 * technique keeps both features whatever the distance, so its descriptor, pipeline and material do not change with the
	 * camera; past a fade's end the draw's factor is 0, which is what dropping the feature draws (engine notes: LOD fades).
	 * Uploaded once an epoch after the frame lighting (PS b13, c6-c12).
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
		// 1 when BSFadeNode::OnVisible updates the metric at all (0x142032dfd); otherwise every node keeps the one it has, and
		// so do the property's fades, which the draw then reads as they are.
		float fadesOn = 0.0f;
		float pad[3]{};
	};
	static_assert(sizeof(LodFadeFrame) == 128);
	/** @brief This frame's, from the main camera and the engine's settings. Render thread. */
	LodFadeFrame SampleLodFadeFrame();
	/**
	 * @brief The draw's LOD fade word (DCLFLodFadeFlagsOf in Common/DCLFObjects.hlsli): the fade node's LOD type (its placement row's), and the
	 * fades the draw applies (the record's, BindlessObject::lodFades, while the node has them apply).
	 */
	inline constexpr std::uint32_t kLodFadeTypeMask = 0xFu;
	inline constexpr std::uint32_t kLodFadeSpecular = 1u << 4;  // MaterialData.y, and SSRParams.w with kLodFadeSsr
	inline constexpr std::uint32_t kLodFadeEnvmap = 1u << 5;    // MaterialData.x
	inline constexpr std::uint32_t kLodFadeSsr = 1u << 6;       // the pass writes SSRParams.w (render flag 2 clear)
	/**
	 * @brief The property's fade node: its world bound centre (the fade-out test's, SetFadeRow) and its LOD type in w (as a
	 * float), plus kLodFadeHeld when the draw's LOD fades do not apply: none of the fade-sensitive flags, or kIgnoreFade on the
	 * node, which BSFadeNode::OnVisible skips (its metric, and so the property's fades, stay as they are). w < 0: no fade node.
	 */
	std::array<float, 4> LodFadeNodeOf(const RE::BSShaderProperty* a_property);
	inline constexpr float kLodFadeHeld = 16.0f;
	/** @brief Whether a LodFadeNodeOf row has the draw fade the property's LOD fades. */
	inline bool LodFadesApply(const std::array<float, 4>& a_node) { return a_node[3] >= 0.0f && a_node[3] < kLodFadeHeld; }
	/** @brief The fade node's LOD metric the frame's camera gives it (FUN_14147b110's +0x144): the draw's, on the CPU. */
	float LodMetricOf(const LodFadeFrame& a_frame, const std::array<float, 4>& a_node);
	/** @brief GetRenderPasses' fade at a metric (FUN_14147c470): 1 before start, 0 past end, where the engine drops the feature. */
	float LodFadeAt(float a_metric, float a_start, float a_end);

	/** @brief Pass descriptor of a BSRenderPass (passEnum minus the Lighting shader's base). */
	inline constexpr std::uint32_t PassDescriptorOf(std::uint32_t a_passEnum) { return a_passEnum - 0x4800002Du; }

	/** @brief Re-reads the [LightingShader] LOD fade settings (once per frame). */

	/** @brief Technique GetRenderPasses selects from the property flags (engine notes: technique table). */
	std::uint32_t SelectLightingTechnique(std::uint64_t a_flags);
	/**
	 * @brief The descriptor BSLightingShader::SetupTechnique (AE 0x1414db810) draws a pass descriptor with: LODLandNoise (18) is
	 * LODLand (9) unless the land noise global (0x142032fdb) is set, and ParallaxOcc (7) is None (0) unless the parallax
	 * occlusion global (0x142035500) is. Its shaders, its samplers and what SetupGeometry reads (+0x94) are that technique's.
	 */
	std::uint32_t SetupTechniqueDescriptor(std::uint32_t a_pass);
	/**
	 * @brief The shader descriptors a pass descriptor draws with: SetupTechnique's split into the vertex and pixel descriptors, then
	 * Community Shaders' lookup change (State::ModifyShaderLookup), with the pixel stage's Deferred bit when a_deferred (the main
	 * view's deferred pass) and without it otherwise (a forward view: the water reflection's cube map).
	 */
	void LightingShaderDescriptors(std::uint32_t a_pass, bool a_deferred, std::uint32_t& a_vertex, std::uint32_t& a_pixel);
	/** @brief The pixel descriptor's Deferred bit (ShaderCache.h, LightingShaderFlags::Deferred): the G-buffer variant. */
	inline constexpr std::uint32_t kLightingPixelDeferred = 1u << 4;
	/**
	 * @brief Whether a pass descriptor's Lighting technique is one the water reflection's cube map draws (SelectLightingTechnique):
	 * LODLand, LODObjects, LODObjectHD, LODLandNoise (dclf-lod.md, "The census").
	 */
	inline constexpr bool LodLightingTechnique(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t technique = (a_passDescriptor >> 24) & 0x3f;
		return technique == 9 || technique == 13 || technique == 15 || technique == 18;
	}
	/** @brief The Lighting techniques by id (SelectLightingTechnique), and kRefractionReject's name. */
	constexpr std::array<std::string_view, 20> kLightingTechniqueNames{ "none", "envmap", "glowmap", "parallax", "facegen",
		"facegenRGBTint", "hair", "parallaxOcc", "MTLand", "LODLand", "snow", "multilayerParallax", "treeAnim", "LODObjects",
		"multiIndexSparkle", "LODObjectHD", "eye", "cloud", "LODLandNoise", "MTLandLODBlend" };
	inline std::string_view LightingTechniqueName(std::uint32_t a_technique)
	{
		return a_technique == 63 ? std::string_view("refraction") : a_technique < kLightingTechniqueNames.size() ? kLightingTechniqueNames[a_technique] : std::string_view("?");
	}

	/**
	 * @brief A member's main pass, built from the object alone (PrimaryCull::MembershipPass): what GetRenderPasses would
	 * register for it. technique is the batch group's key, the pass descriptor SetupTechnique receives.
	 */
	struct AccumulatedPass
	{
		std::uint32_t technique = 0;  // pass descriptor (key minus the Lighting base)
		std::uint32_t subPass = 0;    // PassGroup list 0-4; the renderer draws 1, 3 and 4 with alpha testing
		std::uint32_t passEnum = 0;   // the pass's own passEnum when the tables were built (diagnostics)
		// The pass's accumulation hint (BSRenderPass+0x1C): which geometry group of the batch renderer it
		// would be drawn from, and for decals which of the two decal groups.
		std::uint32_t hint = 0;
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

	/**
	 * @brief T6b3e: a membership pass's derived pass descriptor (PrimaryCull::MembershipPass), with what it was derived from (the
	 * property record's key, material, flags and fade state): derived again when any of them differs. Kept in the geometry's tracked
	 * entry (SceneStore::Tracked::membershipDerived), so it lives and dies with it; the pass returns the entry to store.
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
	 * alpha-test list with it, which keeps the object DCLF's in every frame: leaving the frames without the bit
	 * native would leave, at each switch between the two, a frame in which the native loop has been told not to
	 * draw the object and DCLF does not either.
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
	 * @brief The tree-animation constants for one object, as BSLightingShader::SetupGeometry computes
	 *        them (engine notes: Func6 at 1414dd040, case 0xc).
	 *
	 * Reads the BSTreeNode the property's fade node downcasts to; leaves the engine's defaults when
	 * there is none. It deliberately does NOT perform SetupGeometry's write back of
	 * previousWindTimer = windTimer: the native draw still does that, and doing it here as well would
	 * advance every tree's animation twice a frame.
	 */
	void DeriveTreeAnim(const RE::BSShaderProperty& a_property, ObjectTreeAnim& a_out);

	/** @brief The BSTreeNode a property's fade node downcasts to, and its static row and clock now; null when there is none. */
	/** @brief A BSTreeNode's values into a_out (TreeStaticOf's, from the node itself: the render thread's tree seeds, T6b1a). */
	void TreeStaticOfNode(const void* a_node, TreeStatic& a_out);
	const void* TreeStaticOf(const RE::BSShaderProperty& a_property, TreeStatic& a_out);
	/** @brief The frame's tree clock inputs (render thread, any time after Main::Update). */
	TreeWindFrame SampleTreeWindFrame();

	/**
	 * @brief The MTLand and MTLandLODBlend techniques (terrain) are eligible when the mtLand toggle is on - unless
	 * Terrain Blending is on and DCLF is drawing into the frame, because that feature intercepts every
	 * terrain pass and redraws it blended with its own depth state, which an opaque owned draw would break.
	 */
	bool MtLandEnabled();
	/** @brief Whether Terrain Blending draws its terrain after the opaque pass, into a frame DCLF draws in (or this thread's snapshot). */
	bool TerrainBlendingDefersTerrain();
	/**
	 * @brief T6b3e: TerrainBlendingDefersTerrain's answer as a scene pass took it at its start, read in its place on a thread that installed
	 * it (TerrainBlendingSnapshotScope): the menu may switch Terrain Blending while the pass's evaluations run on the pool.
	 */
	inline thread_local const bool* terrainDefersSnapshot = nullptr;
	/** @brief T6b3e: a_defers is this thread's TerrainBlendingDefersTerrain while the scope lives (null: the feature's). */
	class TerrainBlendingSnapshotScope
	{
	public:
		explicit TerrainBlendingSnapshotScope(const bool* a_defers) :
			previous(terrainDefersSnapshot)
		{
			terrainDefersSnapshot = a_defers;
		}
		~TerrainBlendingSnapshotScope() { terrainDefersSnapshot = previous; }
		TerrainBlendingSnapshotScope(const TerrainBlendingSnapshotScope&) = delete;
		TerrainBlendingSnapshotScope& operator=(const TerrainBlendingSnapshotScope&) = delete;

	private:
		const bool* previous;
	};

	/**
	 * @brief The vertex and pixel descriptors the native main (deferred) pass uses for this geometry, derived without
	 * running GetRenderPasses; the reason the object stays native, or None.
	 *
	 * The descriptor is the property's, whatever the camera: the specular and envmap LOD fades are the draw's
	 * (LodFadeFrame). derivedPass is that derivation alone.
	 *
	 * @param a_accumulated The geometry's accumulated pass this frame (SceneStore::FindAccumulatedPass), or null. When
	 * present it gives the bits the property does not (kRegisteredPassBits); without it the derivation's guesses for them
	 * stand.
	 * @param a_layer a_property is the geometry's layer (LayerPropertyOf), whose passes the engine draws with hint 12.
	 *
	 * T6b1b: from the records (a Lighting property's, the geometry's leaf: the mirror's on the scene work, LiveLeaf's elsewhere).
	 */
	Ineligible DeriveLightingDescriptors(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf,
		const AccumulatedPass* a_accumulated, LightingDescriptors& a_out, bool a_layer = false);
}
