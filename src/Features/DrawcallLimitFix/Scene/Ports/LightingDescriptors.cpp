// LightingDescriptors.h's ports: technique selection, pass and shader descriptors, shadow bits, LOD fades and the Effect / particle
// acceptance. Pure functions of the records and FrameGlobals; the Sample* functions are the render thread's.

#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

#include "ShaderCache.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "State.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "Features/LightLimitFix.h"
#include "Features/TerrainBlending.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Engine/Views/ShadowViews.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EffectConstants.h"

namespace DCLF
{
	namespace
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using LightingFlag = SIE::ShaderCache::LightingShaderFlags;

		constexpr std::uint64_t Bit(Flag a_flag) { return static_cast<std::uint64_t>(a_flag); }
		constexpr std::uint32_t Bit(LightingFlag a_flag) { return static_cast<std::uint32_t>(a_flag); }

		// A technique a feature contributes (Api::TechniqueContribution; Features/TreeWind's TreeAnim) is DCLF's while its toggle is on.
		// The contributions are read once, the features installed before any scene work.
		bool ContributedTechniqueOn(std::uint32_t a_technique)
		{
			static const auto& techniques = Extensions::RegisteredTechniques();
			return a_technique < techniques.size() && techniques[a_technique].toggle && ActiveToggles().On(techniques[a_technique].toggle);
		}

		// Techniques DCLF supports. The rest stay native.
		bool IsSupportedTechnique(std::uint32_t a_technique)
		{
			switch (static_cast<Technique>(a_technique)) {
			case Technique::None:
			case Technique::Envmap:
			case Technique::Glowmap:
			case Technique::Parallax:
			// SetupMaterial writes their constants and textures (ParallaxOccData, MultiLayerParallaxData, layer and envmap maps),
			// which the material records take from the engine's evaluation.
			case Technique::ParallaxOcc:
			case Technique::MultilayerParallax:
				return true;
			// A multi-index shape's ice or snow layer (kMultiIndexSnow with kProjectedUV): SparkleParams is SetupMaterial's; SetupGeometry's
			// projected textures replace the slot-10 texture SetupTechnique binds.
			case Technique::MultiIndexSparkle:
				return ActiveToggles().layers;
			// Actor skin (SKIN path, TintColor), faces (Facegen: tint and detail maps at t3/t4), hair (TintColor) and eyes (eye
			// centres, material VS constants): all written by SetupMaterial. A face part's positions are FaceSnapshots'.
			case Technique::FacegenRGBTint:
			case Technique::Facegen:
			case Technique::Hair:
			case Technique::Eye:
				return ActiveToggles().actors;
			case Technique::MTLand:
			case Technique::MTLandLODBlend:
				return MtLandEnabled();
			// Object LOD (dclf-lod.md), drawn by visible segment ranges; only a BSSubIndexTriShape with the LOD object flags.
			case Technique::LODObjects:
			case Technique::LODObjectHD:
				return ActiveToggles().lodObjects;
			// Terrain LOD (dclf-lod.md): only a land block's shape under the LOD root (IsLodLand).
			case Technique::LODLand:
			case Technique::LODLandNoise:
				return ActiveToggles().lodTerrain;
			default:
				return ContributedTechniqueOn(a_technique);
			}
		}

		// FUN_14147c470: false when the feature has faded out, else the fade factor in a_fade. a_metric is the fade node's current
		// LOD metric (+0x144); the previous one (+0x148) only feeds the engine's render-pass cache invalidation.
		bool LodFadeVisible(float a_metric, float a_start, float a_end, float& a_fade)
		{
			if (a_end < a_metric)
				return false;
			if (a_metric <= a_start) {
				a_fade = 1.0f;
			} else {
				a_fade = std::clamp((a_metric - a_end) / (a_start - a_end), 0.0f, 1.0f);
			}
			return true;
		}

		// BSLightingShader::SetupTechnique splits the pass descriptor (engine notes, SetupTechnique).
		constexpr std::uint32_t VertexDescriptorFromPass(std::uint32_t d)
		{
			return (d & 0x48007u) | ((d & 0x20a00u) ? 0x200u : 0u) | (d & 0x3f000000u);
		}

		// Community Shaders' pixel split (TruePBR.cpp, BSLightingShader_GetPixelTechnique) keeps bits 3-5 (TruePbr/Deferred);
		// only the shadow light count (bits 6-8) is dropped.
		constexpr std::uint32_t PixelDescriptorFromPass(std::uint32_t d)
		{
			std::uint32_t p = d & ~0x1c0u;
			if ((p & 0x4u) == 0)  // Skinned is kept only with ModelSpaceNormals
				p &= ~0x2u;
			return p | 1u;  // VC is always set
		}
	}

	std::uint8_t FadeStateOf(const SceneCapture::PropertyRecord* a_property)
	{
		if (!a_property)
			return 0;
		const std::uint64_t f = a_property->flags;
		if (!(f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))))
			return 0;
		return a_property->fadeNode ? 0 : kFadeNoNode;
	}

	RE::BSShaderProperty* LayerPropertyOf(const RE::BSGeometry& a_geometry)
	{
		if (const_cast<RE::BSGeometry&>(a_geometry).GetType().get() != RE::BSGeometry::Type::kMultiIndexTriShape)
			return nullptr;
		const auto& data = static_cast<const RE::BSMultiIndexTriShape&>(a_geometry).GetMultiIndexTrishapeRuntimeData();
		if (!data.altIndexBuffer || !*reinterpret_cast<ID3D11Buffer* const*>(data.altIndexBuffer) || data.altPrimCount == 0)
			return nullptr;
		return data.additionalShaderProperty.get();
	}

	std::uint32_t StaticShadowBits(const SceneCapture::LeafView& a_leaf, bool a_settled, const SceneCapture::PropertyRecord* a_property)
	{
		const auto& g = FrameGlobals::Current();
		const auto* lighting = SceneCapture::LeafView::Lighting(a_property ? a_property : a_leaf.property);
		if (!lighting || !g.accumulator)
			return 0;
		const std::uint64_t flags = lighting->flags;
		const auto* fadeNode = lighting->fadeNode ? a_leaf.FadeNodeOf(lighting) : nullptr;
		// a_settled: fully faded in whatever the node holds (a member's pass: its fade is the GPU's).
		const float fade = fadeNode && !a_settled ? fadeNode->currentFade : 1.0f;
		const float alpha = lighting->materialAlpha * fade;  // the record's materialAlpha: 1 without a material
		const bool blended = a_leaf.AlphaBlending();
		// GetRenderPasses: local_164, local_167, local_165/local_168 and local_158.
		const bool translucent = alpha < 1.0f || blended;
		const bool screenDoor = g.screenDoorFades && fadeNode && fadeNode->fadeDoor && fade < 1.0f &&
		                        !(flags & (1ull << 19));
		constexpr std::uint64_t kOpaqueClasses = 0x800c000000ull;
		const bool eligible = !translucent || (screenDoor && !blended) || (flags & kOpaqueClasses);
		bool deferred = g.accumulatorDeferredShadow != 0;
		if (!screenDoor || blended)
			deferred = deferred && !((alpha < 1.0f || blended || (flags & (1ull << 33))) && !(flags & kOpaqueClasses));
		bool shadowDir = deferred && eligible && !g.noSunShadowDir;
		bool defShadow = deferred;
		if (!(flags & 0x800c000100ull) && !lighting->shadowPasses)
			shadowDir = defShadow = false;
		return (shadowDir ? 0x2000u : 0u) | (defShadow ? 0x4000u : 0u) | (defShadow && !shadowDir ? 0x40u : 0u);
	}

	LodFadeFrame SampleLodFadeFrame()
	{
		const auto& g = FrameGlobals::Current();
		LodFadeFrame frame;
		frame.eye[0] = g.eye[0];
		frame.eye[1] = g.eye[1];
		frame.eye[2] = g.eye[2];
		frame.lodAdjust = g.lodAdjust;
		frame.specularStart = g.specularStart;
		frame.specularEnd = g.specularEnd;
		frame.envmapStart = g.envmapStart;
		frame.envmapEnd = g.envmapEnd;
		frame.metricScale = g.metricScale;
		frame.defaultScale = g.fadeDefaultScale;
		frame.metricOverride = g.fadeSpecialA;
		frame.overridden = g.fadeSpecialA != g.fadeSpecialB ? 1.0f : 0.0f;
		for (std::uint32_t type = 0; type < 16; ++type)
			frame.divisors[type] = g.fadeTypeDivisors[type];
		frame.fadesOn = g.fadesOn ? 1.0f : 0.0f;
		frame.refractionStart = g.refractionStart;
		frame.refractionEnd = g.refractionEnd;
		return frame;
	}

	std::array<float, 4> LodFadeNodeOf(const RE::BSShaderProperty* a_property)
	{
		if (!a_property || !a_property->fadeNode)
			return { 0.0f, 0.0f, 0.0f, -1.0f };
		const auto* node = a_property->fadeNode;
		// A refraction property's own fade (fRefractionLODFade*, GetRenderPasses' unless kNoFade) is fade-sensitive too.
		const std::uint64_t flags = a_property->flags.underlying();
		const bool refractionFades = (flags & (Bit(Flag::kRefraction) | Bit(Flag::kTempRefraction))) && !(flags & Bit(Flag::kNoFade));
		const bool held = (!(flags & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))) && !refractionFades) ||
		                  node->GetFlags().any(RE::NiAVObject::Flag::kIgnoreFade);
		const auto& center = node->worldBound.center;
		return { center.x, center.y, center.z, static_cast<float>(Engine::At<std::uint8_t>(node, 0x153) & 0xF) + (held ? kLodFadeHeld : 0.0f) };
	}

	static_assert(kLightingPixelDeferred == static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::Deferred));

	void LightingShaderDescriptors(std::uint32_t a_pass, bool a_deferred, std::uint32_t& a_vertex, std::uint32_t& a_pixel)
	{
		uint vertex = VertexDescriptorFromPass(SetupTechniqueDescriptor(a_pass));
		uint pixel = PixelDescriptorFromPass(SetupTechniqueDescriptor(a_pass));
		// Forced, so the result does not depend on whether the deferred pass is running; the lookup only adds the bit.
		globals::state->ModifyShaderLookup(RE::BSShader::Type::Lighting, vertex, pixel, true);
		if (!a_deferred)
			pixel &= ~kLightingPixelDeferred;
		a_vertex = vertex;
		a_pixel = pixel;
	}

	std::uint32_t SetupTechniqueDescriptor(std::uint32_t a_pass)
	{
		const std::uint32_t technique = a_pass & 0x3f000000u;
		// The engine's bytes 0x2032fdb and 0x2035500 as the frame captured them.
		const auto& g = FrameGlobals::Current();
		if (technique == 0x12000000u && !g.techniqueByte12)
			return (a_pass & 0xc9ffffffu) | 0x9000000u;
		if (technique == 0x7000000u && !g.techniqueByte7)
			return a_pass & 0xc0ffffffu;
		return a_pass;
	}

	std::uint32_t SelectLightingTechnique(std::uint64_t f)
	{
		// Same order as BSLightingShaderProperty::GetRenderPasses; later tests win.
		std::uint32_t t = (f & Bit(Flag::kEnvMap)) ? 1u : 0u;
		if (f & Bit(Flag::kGlowMap))
			t = 2;
		if ((f & (Bit(Flag::kParallax) | Bit(Flag::kParallaxOcclusion))) == Bit(Flag::kParallax))
			t = 3;
		if (f & Bit(Flag::kFace))
			t = 4;
		if (f & Bit(Flag::kFaceGenRGBTint))
			t = 5;
		if (f & Bit(Flag::kHairTint))
			t = 6;
		if (f & Bit(Flag::kParallaxOcclusion))
			t = 7;
		if (f & Bit(Flag::kMultiTextureLandscape))
			t = 8;
		if (f & Bit(Flag::kNoLODLandBlend))
			t = 19;
		if (f & Bit(Flag::kLODLandscape))
			t = 18;
		if (f & Bit(Flag::kLODObjects))
			t = 13;
		if (f & Bit(Flag::kHDLODObjects))
			t = 15;
		if (f & Bit(Flag::kMultiLayerParallax))
			t = 11;
		if (f & Bit(Flag::kTreeAnim))
			t = 12;
		if ((f & (Bit(Flag::kMultiIndexSnow) | Bit(Flag::kProjectedUV))) == (Bit(Flag::kMultiIndexSnow) | Bit(Flag::kProjectedUV)))
			t = 14;
		if (f & Bit(Flag::kEyeReflect))
			t = 16;
		return t;
	}

	bool TerrainBlendingDefersTerrain()
	{
		if (terrainDefersSnapshot)
			return *terrainDefersSnapshot;
		const auto& terrainBlending = globals::features::terrainBlending;
		return terrainBlending.loaded && terrainBlending.settings.Enabled;
	}

	bool MtLandEnabled()
	{
		const auto toggles = ActiveToggles();
		return toggles.mtLand && (!TerrainBlendingDefersTerrain() || toggles.tbTerrain);
	}

	namespace
	{
		/**
		 * @brief alphaBlendMode as BSShader's alpha setup (AE FUN_14150bc80) picks it from a blending NiAlphaProperty's functions, or
		 * 0 for a pair it leaves alone. (Its mode 1 for a non-blending property with alpha below 1 is not modelled: not eligible.)
		 */
		std::uint32_t AlphaBlendModeOf(const SceneCapture::AlphaRecord& a_alpha)
		{
			// NiAlphaProperty::GetSrcBlendMode, GetDestBlendMode: the flags' bits 1-4 and 5-8.
			using Function = RE::NiAlphaProperty::AlphaFunction;
			const auto source = static_cast<Function>((a_alpha.flags >> 1) & 0xF);
			const auto dest = static_cast<Function>((a_alpha.flags >> 5) & 0xF);
			if (source == Function::kSrcAlpha && dest == Function::kInvSrcAlpha)
				return 1;
			if ((source == Function::kSrcAlpha && dest == Function::kOne) || (source == Function::kOne && dest == Function::kOne) ||
				(source == Function::kSrcAlpha && dest == Function::kInvDestAlpha))
				return 2;
			if ((source == Function::kZero && dest == Function::kSrcColor) || (source == Function::kDestColor && dest == Function::kZero))
				return 4;
			if (source == Function::kDestColor && dest == Function::kInvSrcAlpha)
				return 3;
			return 0;
		}
	}

	Ineligible DeriveLightingDescriptors(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf,
		const AccumulatedPass* a_accumulated, LightingDescriptors& a_out, bool a_layer)
	{
		std::uint64_t f = a_property.flags;

		// Terrain LOD (a land block's BSTriShape) and object LOD (a BSSubIndexTriShape, drawn by LodSegments' ranges) each by toggle.
		if ((f & Bit(Flag::kLODLandscape)) && !(ActiveToggles().lodTerrain && IsLodLand(a_property.flags, a_leaf.Type())))
			return Ineligible::Lod;
		if ((f & (Bit(Flag::kLODObjects) | Bit(Flag::kHDLODObjects))) && !(ActiveToggles().lodObjects && IsLodObject(a_property.flags, a_leaf.Type())))
			return Ineligible::Lod;
		const auto* alpha = a_leaf.alpha;
		const bool blending = a_leaf.AlphaBlending();
		if (a_layer) {
			// A multi-index shape's layer (engine notes, "The main modes' registration"): every pass goes into geometry group 2 with
			// hint 12, and FUN_1414b3bb0 draws it after the opaque decals with their state (depth test+write, bias mode 8 + b, no
			// blending, write mode 10, render flags 0x41: alpha property not applied). DCLF's decal group 3, between opaque and blended.
			if (!ActiveToggles().decals)
				return Ineligible::Decal;
			a_out.decalGroup = 3;
			a_out.decalBlendMode = 0;
			a_out.decalWriteMode = 10;
		} else if (f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) {
			// Decals (engine notes: decals). The accumulation hint picks the geometry group and state:
			//   hint 2: depth test+write, opaque decal bias, no blending, write mode 10, render flags 0x41 (alpha property NOT applied);
			//   hint 3: depth test only, blended decal bias, render flags 0x45 (alpha property applied: blend mode from its functions,
			//           alpha test from its flag), write mode 1 with kZBufferWrite (SetupGeometry forces it), else 11 (the group's).
			// Neither occludes anything its host does not, so DCLF draws them in a second pass without depth writes. Without an
			// accumulated pass the hint is GetRenderPasses' for the settled object: 2 + (material alpha < 1 or blending).
			if (!ActiveToggles().decals || !(f & Bit(Flag::kZBufferTest)))
				return Ineligible::Decal;
			const std::uint32_t hint = a_accumulated ? a_accumulated->hint : (a_property.materialAlpha < 1.0f || blending) ? 3u : 2u;
			if (hint == 2) {
				if (blending)
					return Ineligible::Decal;  // not measured in this group; the state would be a guess
				a_out.decalGroup = 1;
				a_out.decalBlendMode = 0;
				a_out.decalWriteMode = 10;
			} else if (hint == 3) {
				// The fading case (alpha < 1 without blending; engine blend mode 1) stays native.
				if (!blending)
					return Ineligible::Decal;
				const std::uint32_t blendMode = AlphaBlendModeOf(*alpha);
				if (blendMode == 0)
					return Ineligible::Decal;  // a blend function pair the engine's setup leaves as it finds it
				a_out.decalGroup = 2;
				a_out.decalBlendMode = blendMode;
				a_out.decalWriteMode = (f & Bit(Flag::kZBufferWrite)) ? 1u : 11u;
			} else {
				return Ineligible::Decal;
			}
		}
		// kSkinned selects the SKINNED permutation, whose palette comes from the geometry's skin instance: without one, native.
		if ((f & Bit(Flag::kSkinned)) && !(ActiveToggles().skinned && a_leaf.Skinned()))
			return Ineligible::Skinned;
		if ((f & Bit(Flag::kProjectedUV)) && !ActiveToggles().projectedUv)
			return Ineligible::ProjectedUV;
		// Terrain Blending replays every kMultiTextureLandscape pass after the opaque pass, blended over what lies beneath, then the
		// passes of meshes opting out with the otherwise unused kNoTransparencyMultiSample, testing EQUAL. DCLF draws both as it
		// does, in terrain groups 1 and 2 after its opaque and decal groups; without CS_DCLF_TB_TERRAIN both stay native (DCLF would
		// draw the meshes before the terrain blends over them). A decal or layer has a group of its own.
		if (TerrainBlendingDefersTerrain()) {
			const bool terrain = (f & Bit(Flag::kMultiTextureLandscape)) != 0;
			const bool noBlend = !terrain && (f & Bit(Flag::kNoTransparencyMultiSample));
			if ((terrain || noBlend) && a_out.decalGroup)
				return Ineligible::TerrainNoBlend;
			if (noBlend && !ActiveToggles().tbTerrain)
				return Ineligible::TerrainNoBlend;
			a_out.terrainGroup = terrain ? 1u : noBlend ? 2u : 0u;
		}
		// Refraction (GetRenderPasses, AE 0x1414adfb0): kRefraction / kTempRefraction has one main pass, a Utility RENDER_NORMAL pass
		// (hint 4: geometry group 5) Main::Draw draws into the refraction normals after the world (FUN_1414b4860) for ISRefraction,
		// none past its refraction LOD fade's end. The forward epoch draws it (drawcall-limit-fix.md, "Refraction"), fading as
		// GetRenderPasses would. A material alpha of 0 registers nothing: left native (so not drawn).
		if (f & (Bit(Flag::kRefraction) | Bit(Flag::kTempRefraction))) {
			if (!ActiveToggles().refraction || !ActiveToggles().forward || a_out.decalGroup || a_layer || a_out.terrainGroup || a_property.materialAlpha <= 0.0f) {
				a_out.rejectedTechnique = kRefractionReject;
				return Ineligible::Technique;
			}
			a_out.refraction = true;
			a_out.refractionFalloff = (f & Bit(Flag::kRefractionFalloff)) != 0;
			a_out.refractionClamp = (f & Bit(Flag::kNonProjectiveShadows)) != 0;
			// The draw's state (FUN_1414b4860): depth mode 1, tested and not written; blending off.
			a_out.forward = true;
			a_out.forwardBlendMode = 0;
			a_out.forwardDepthTest = true;
			a_out.forwardDepthWrite = false;
		}

		// Blended geometry draws after the deferred composite (except the blended decal group, above). CS_DCLF_FORWARD: the forward
		// epoch draws it as the engine's blended draw (FUN_1414b39d0) does: hint 1 (blended, or material alpha below one), alpha
		// property applied (render flag 4: FUN_14150bc80's mode; none for alpha below one without blending, whose property alpha is
		// the fade's), write mode 11, SetupGeometry's depth mode from kZBufferTest / kZBufferWrite, the forward pixel stage. A pair
		// FUN_14150bc80 leaves as it finds it (stale state) stays native, as does anything Terrain Blending holds.
		const bool translucent = blending || a_property.materialAlpha < 1.0f;
		if (a_out.refraction) {
			// The refraction draw's forward state (above): alpha property not applied.
		} else if (translucent && a_out.decalGroup == 0 && !a_layer && a_out.terrainGroup == 0 && ActiveToggles().forward) {
			const std::uint32_t blendMode = blending ? AlphaBlendModeOf(*alpha) : 0u;
			if (blending && blendMode == 0)
				return Ineligible::AlphaBlend;
			a_out.forward = true;
			a_out.forwardBlendMode = blendMode;
			a_out.forwardDepthTest = (f & Bit(Flag::kZBufferTest)) != 0;
			a_out.forwardDepthWrite = (f & Bit(Flag::kZBufferWrite)) != 0;
		} else if (blending && a_out.decalGroup != 2 && !a_layer) {
			return Ineligible::AlphaBlend;
		}

		// The descriptor as GetRenderPasses builds it for an opaque object whose specular and envmap LOD fades have not run out
		// (the draw fades them, LodFadeFrame), so it is the same at every distance.
		std::uint32_t derived = 0;
		const Ineligible derivedReason = [&] {
			// Without a fade node GetRenderPasses never computes the LOD fades, so the fields the draw reads may be stale: native.
			if (!a_property.fadeNode && (f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))))
				return Ineligible::Fading;

			const std::uint32_t technique = SelectLightingTechnique(f);
			if (!IsSupportedTechnique(technique)) {
				a_out.rejectedTechnique = technique;
				return Ineligible::Technique;
			}

			std::uint32_t d = technique << 24;
			if (f & Bit(Flag::kVertexColors))
				d |= Bit(LightingFlag::VC);
			// GetRenderPasses copies kSkinned into bit 1 whether or not there is a skin (static fish and buckets carry it).
			if (f & Bit(Flag::kSkinned))
				d |= Bit(LightingFlag::Skinned);
			// ProjectedUV is kProjectedUV itself; the snow conditions only add bits 19 and 21 on top (not derived).
			if (f & Bit(Flag::kProjectedUV))
				d |= Bit(LightingFlag::ProjectedUV);
			if (f & Bit(Flag::kModelSpaceNormals))
				d |= Bit(LightingFlag::ModelSpaceNormals);
			if (f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow)))
				d |= Bit(LightingFlag::Specular);
			if (f & Bit(Flag::kSoftLighting))
				d |= Bit(LightingFlag::SoftLighting);
			if (f & Bit(Flag::kRimLighting))
				d |= Bit(LightingFlag::RimLighting);
			if (f & Bit(Flag::kBackLighting))
				d |= Bit(LightingFlag::BackLighting);
			if (f & Bit(Flag::kAnisotropicLighting))
				d |= Bit(LightingFlag::AnisoLighting);
			// GetRenderPasses (1414adfb0) gives every decal without kMultiIndexSnow DoAlphaTest, whatever its alpha property.
			if (a_leaf.AlphaTesting() ||
				((f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) && !(f & Bit(Flag::kMultiIndexSnow))))
				d |= Bit(LightingFlag::DoAlphaTest);
			if (f & Bit(Flag::kCharacterLighting))
				d |= Bit(LightingFlag::CharacterLight);

			// Community Shaders' GetRenderPasses hook (TruePBR.cpp): PBR swaps Specular for TruePbr; glint adds AnisoLighting.
			const auto feature = static_cast<RE::BSShaderMaterial::Feature>(a_property.feature);
			const bool isPbr = (f & Bit(Flag::kVertexLighting)) && a_property.material &&
			                   (feature == RE::BSShaderMaterial::Feature::kDefault || feature == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend);
			if (isPbr) {
				d |= Bit(LightingFlag::TruePbr);
				d &= ~Bit(LightingFlag::Specular);
				if (a_property.glints)
					d |= Bit(LightingFlag::AnisoLighting);
			}
			derived = d;
			return Ineligible::None;
		}();

		if (derivedReason != Ineligible::None)
			return derivedReason;
		a_out.derivedPass = derived;
		std::uint32_t d = derived;
		if (a_accumulated) {
			// The registered pass gives only kRegisteredPassBits; the rest stays the derivation's (camera-independent), the shadow
			// bits StaticShadowBits'. A layer's pass has none: ShadowDir is the property's light mask naming the sun, and the
			// registrations write masks on the main property alone (FUN_1414b2140), so DefShadow goes too.
			d = (derived & ~kRegisteredPassBits) | (a_accumulated->technique & kRegisteredPassBits) | (a_layer ? 0u : StaticShadowBits(a_leaf, false, &a_property));
			// Screen-door fade: Lighting.hlsl discards against a 4x4 pattern and MaterialData.z, so the object stays opaque and the
			// Z-prepass (which keeps the alpha test) dithers identically.
		}

		a_out.rawVertex = VertexDescriptorFromPass(SetupTechniqueDescriptor(d));
		a_out.rawPixel = PixelDescriptorFromPass(SetupTechniqueDescriptor(d));
		std::uint32_t vertex = 0, pixel = 0;
		// Forward epoch draws are forward-shaded (no Deferred), as the engine's blended draws after the composite.
		LightingShaderDescriptors(d, !a_out.forward, vertex, pixel);

		a_out.technique = (d >> 24) & 0x3f;
		a_out.pass = d;
		a_out.projectedUV = (d & Bit(LightingFlag::ProjectedUV)) != 0;
		a_out.vertex = vertex;
		a_out.pixel = pixel;
		return Ineligible::None;
	}

	bool EffectSkinned(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf)
	{
		// FUN_1414bde90: kSkinned and the geometry's vertex description (+0x148) with a skin flag (VF_SKINNED's bits, 0x1004000000000000).
		constexpr std::uint64_t kVertexSkinned = 0x1004000000000000ull;
		const std::uint64_t vertexDesc = a_leaf.geometry ? a_leaf.geometry->vertexDesc : 0;
		return (a_property.flags & Bit(Flag::kSkinned)) && (vertexDesc & kVertexSkinned);
	}

	bool ParticleSystemTaken(const SceneCapture::LeafView& a_leaf)
	{
		constexpr auto kParticles = static_cast<std::uint8_t>(RE::BSGeometry::Type::kParticles);
		if (!Extensions::GeometryTypeOn(ActiveToggles(), kParticles) || !a_leaf.geometry || a_leaf.Type() != kParticles || a_leaf.Skinned())
			return false;
		const auto& p = a_leaf.geometry->particles;
		// Aligned quads with the velocities FUN_140e51320 reads for them.
		return p.system && p.data && (!p.aligned || p.velocities) && p.capacity > 0;
	}

	std::uint32_t ParticleRejectionOf(const SceneCapture::LeafView& a_leaf, std::uint64_t a_flags)
	{
		// ParticleSystemTaken's conditions, a bit each, then the property's: a particle pass is hint 1 whatever its alpha, so without
		// a blending alpha property its blend mode is the frame's property alpha (FUN_14150bc80); and no decal.
		if (!Extensions::GeometryTypeOn(ActiveToggles(), a_leaf.Type()))
			return kEffectVariantParticle;
		const auto* g = a_leaf.geometry;
		if (!g || a_leaf.Type() != static_cast<std::uint8_t>(RE::BSGeometry::Type::kParticles) || !g->particles.system || a_leaf.Skinned())
			return kEffectVariantParticleClass;
		std::uint32_t causes = 0;
		if (!g->particles.data || !g->particles.capacity)
			causes |= kEffectVariantParticleData;
		if (g->particles.aligned && !g->particles.velocities)
			causes |= kEffectVariantParticleAligned;
		if (!a_leaf.AlphaBlending())
			causes |= kEffectVariantParticleUnblended;
		if (a_flags & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal)))
			causes |= kEffectVariantParticleDecal;
		return causes;
	}

	std::uint32_t EffectVariantsOf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf)
	{
		// What BSEffectShaderProperty::GetRenderPasses (0x1414bd890) and BSEffectShader's setup do that the port does not. Sky object
		// (bit 45): hint 0x12, geometry group 13, SetupGeometry's own cull and alpha setup. Wireframe (bit 48): SetupGeometry's depth
		// and fill state. ProjectedUv: constants not ported.
		const std::uint64_t f = a_property.flags;
		std::uint32_t variants = 0;
		if (ParticleGeometryType(a_leaf.Type())) {
			variants |= ParticleRejectionOf(a_leaf, f);
		} else if (a_leaf.Type() != static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape)) {
			variants |= kEffectVariantShape;
		}
		// The palette path (CS_DCLF_SKINNED) draws a skinned effect when the descriptor's Skinned bit (EffectSkinned) matches the
		// leaf having a skin instance. kSkinned on a static shape without skinned vertices is no variant; any other mismatch is.
		if (EffectSkinned(a_property, a_leaf) != a_leaf.Skinned() || (a_leaf.Skinned() && !ActiveToggles().skinned))
			variants |= kEffectVariantSkinned;
		// Effect decals (hints 2 and 3) go to DCLF's decal groups (G-buffer pass, DEFERRED). kTempRefraction with kDynamicDecal
		// registers no pass at all (GetRenderPasses), so it stays a variant.
		if ((f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) &&
			(!ActiveToggles().decals || ((f & Bit(Flag::kTempRefraction)) && (f & Bit(Flag::kDynamicDecal)))))
			variants |= kEffectVariantDecal;
		if (f & Bit(Flag::kBillboard))
			variants |= kEffectVariantSkyObject;
		if (f & Bit(Flag::kWireframe))
			variants |= kEffectVariantWireframe;
		// kEffectLighting (the Lighting bit also needs global 0x142033508, set to 1 only by its initialiser): light 0 is FUN_1414fcf80's,
		// the sun (ShadowSceneNode +0x200) unless kCloudLOD picks the cloud light (+0x208); the port has the sun's alone. Point lights
		// are Light Limit Fix's clusters (Effect.hlsl, GetLightingColor), the pass's own PerGeometry lights without it.
		if (f & Bit(Flag::kEffectLighting)) {
			if (f & Bit(Flag::kCloudLOD))
				variants |= kEffectVariantCloudLighting;
			if (!globals::features::lightLimitFix.loaded)
				variants |= kEffectVariantLightingNoLlf;
		}
		// kWeaponBlood (BSTempEffectWeaponBlood) is ported: its AlphaTestRef (SetupGeometry: alpha threshold / 255 and two globals) is
		// the object record's and the frame's.
		if (f & Bit(Flag::kProjectedUV))
			variants |= kEffectVariantProjectedUv;
		if (a_leaf.AlphaBlending() && AlphaBlendModeOf(*a_leaf.alpha) == 0)
			variants |= kEffectVariantBlendPair;
		return variants;
	}

	Ineligible DeriveEffectDescriptors(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, LightingDescriptors& a_out)
	{
		// drawcall-limit-fix.md, "The Effect shader in the forward epoch".
		const std::uint64_t f = a_property.flags;
		const auto* alpha = a_leaf.alpha;
		const std::uint64_t vertexDesc = a_leaf.geometry ? a_leaf.geometry->vertexDesc : 0;
		const std::uint32_t variants = EffectVariantsOf(a_property, a_leaf);
		if (variants & ~kEffectVariantBlendPair)
			return Ineligible::EffectVariant;
		// Hint 1 (blended list, sorted with blended Lighting) for a blending alpha property, blend mode from FUN_14150bc80; without one,
		// hint 0 (G-buffer DEFERRED variant), or hint 1 by Falloff or property alpha alone (blend mode then follows the frame's alpha).
		// A decal takes hint 2 without a blending alpha property and 3 with one, whatever its alpha or Falloff; FUN_1414b2330 puts
		// them in geometry groups 3 and 4, the engine's opaque and blended decal groups (DCLF's 1 and 2).
		const bool decal = (f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) != 0;
		const bool blending = a_leaf.AlphaBlending();
		// CS_DCLF_OPAQUE_EFFECTS: hint 0 (G-buffer opaque batches, DEFERRED: forge coals, slag) drawn through decal group 1: depth by
		// the decal depth pass (LESS_EQUAL, no bias), colour with write mode 1 and no blending. Only a pass that is hint 0 every frame:
		// no Falloff, depth tested and written (SetupGeometry's mode 3). A property alpha below one makes the engine's pass hint 1,
		// blended by the frame's alpha: DCLF keeps it opaque (drawcall-limit-fix.md, "Opaque effects").
		static const bool opaqueEffects = SwitchOnUnlessZero(Switch::OpaqueEffects);
		const bool opaque = !blending && !decal;
		if (opaque && (!opaqueEffects || !ActiveToggles().decals || (f & Bit(Flag::kFalloff)) || !(f & Bit(Flag::kZBufferTest)) || !(f & Bit(Flag::kZBufferWrite)) ||
						  ParticleGeometryType(a_leaf.Type())))
			return Ineligible::EffectOpaque;
		// A taken particle system: hint 1, the forward epoch's.
		const bool particles = ParticleGeometryType(a_leaf.Type());
		if (variants & kEffectVariantBlendPair)
			return Ineligible::EffectVariant;  // a pair the alpha setup leaves as it finds it (only ever a blending one)
		const std::uint32_t blendMode = blending ? AlphaBlendModeOf(*alpha) : 0u;

		using namespace EffectFlag;
		std::uint32_t d = 0;
		if (f & Bit(Flag::kVertexColors))
			d |= Vc;
		// The source texture's path (FUN_140cec900 against null: the mirror's kEffectHasSourcePath).
		if (a_property.feature & kEffectHasSourcePath)
			d |= TexCoord | Texture;
		if (EffectSkinned(a_property, a_leaf))
			d |= Skinned;
		// Falloff for anything but particles. Particles with a texture and subtextures add TexCoordIndex and IndexedTexture (DCLF's
		// particle quads carry the IndexedTexCoord resolved: Effect.hlsl, DCLFParticleInput).
		if ((f & Bit(Flag::kFalloff)) && !particles)
			d |= Normals | Falloff;
		if (particles) {
			d |= Particles;
			if ((d & Texture) && a_leaf.geometry && a_leaf.geometry->particles.subtextures)
				d |= TexCoordIndex | IndexedTexture;
		}
		// The alpha property's functions (bits 1-4 source, 5-8 destination), blending or not: ONE as destination is AddBlend, SRCCOLOR
		// MultBlend, the pair (DESTCOLOR, INVSRCALPHA) MultBlendDecal. None without an alpha property (a decal's may have none).
		const std::uint32_t source = alpha ? (alpha->flags >> 1) & 0xF : ~0u, dest = alpha ? (alpha->flags >> 5) & 0xF : ~0u;
		if (dest == 0)
			d |= AddBlend;
		if (source == 4 && dest == 7)
			d |= MultBlendDecal;
		else if (dest == 2)
			d |= MultBlend;
		if (f & Bit(Flag::kSoftEffect))
			d |= Soft;
		// Effect.hlsl's BLOOD: alpha from the texture's green and the blood mask's colour.
		if (f & Bit(Flag::kWeaponBlood))
			d |= Blood;
		// Global 0x142033508 taken as its initialiser leaves it (1).
		if (f & Bit(Flag::kEffectLighting))
			d |= Lighting;
		if (a_property.feature & kEffectHasGreyscalePath) {
			if (f & Bit(Flag::kGrayscaleToPaletteColor))
				d |= GrayscaleToColor;
			if (f & Bit(Flag::kGrayscaleToPaletteAlpha))
				d |= GrayscaleToAlpha;
		}
		// Normals in the vertex description (either stream), no alpha property or an alpha-tested one, depth tested and written.
		constexpr std::uint64_t kVertexNormals = 0x200800000000000ull;
		if ((vertexDesc & kVertexNormals) && (!alpha || (alpha->flags & (1u << 9))) && (f & Bit(Flag::kZBufferTest)) && (f & Bit(Flag::kZBufferWrite)))
			d |= MotionVectorsNormals;

		// SetupTechnique's split (FUN_141521300, FUN_141521310), then State::ModifyShaderLookup: the three permutation bits off both,
		// Deferred on the pixel stage only in the deferred pass (decals, opaque effects), never for forward objects.
		constexpr std::uint32_t kPermutationBits = GrayscaleToColor | GrayscaleToAlpha | IgnoreTexAlpha;
		a_out = {};
		a_out.effect = true;
		a_out.technique = 0;
		a_out.pass = d;
		a_out.derivedPass = d;
		a_out.rawVertex = d & 0xffc7ffffu;
		a_out.rawPixel = ((d & Membrane) ? d : (d & ~Skinned)) & ~SkyObject;
		a_out.vertex = a_out.rawVertex & ~kPermutationBits;
		a_out.pixel = (a_out.rawPixel & ~kPermutationBits) | ((decal || opaque) ? Deferred : 0u);
		if (opaque) {
			a_out.opaqueEffect = true;
			a_out.decalGroup = 1u;
			a_out.decalBlendMode = 0u;
			a_out.decalWriteMode = 1u;
			return Ineligible::None;
		}
		if (decal) {
			// Decal group state (engine notes, "Decals"; BSEffectShader::SetupGeometry 0x1414eab10, hints 2 and 3): the group's depth
			// mode (3 opaque, 1 blended) and bias, which SetupGeometry leaves; alpha property applied in the blended group alone (render
			// flags 0x45 vs 0x41); write mode 11 without MotionVectorsNormals (SetupGeometry), else the group's (10 opaque, 11 blended).
			a_out.decalGroup = blending ? 2u : 1u;
			a_out.decalBlendMode = blendMode;
			a_out.decalWriteMode = (!blending && (d & MotionVectorsNormals)) ? 10u : 11u;
			return Ineligible::None;
		}
		a_out.forward = true;
		a_out.forwardBlendMode = blendMode;
		a_out.forwardDepthTest = (f & Bit(Flag::kZBufferTest)) != 0;
		a_out.forwardDepthWrite = (f & Bit(Flag::kZBufferWrite)) != 0;
		return Ineligible::None;
	}
}
