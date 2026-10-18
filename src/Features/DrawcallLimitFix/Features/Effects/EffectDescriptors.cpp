// EffectDescriptors.h's ports: the Effect port's variants, the particle rejection and the descriptors. Pure functions of the records, the
// live toggles and the "light limit fix" classify input (whether Light Limit Fix is loaded).

#include "Features/DrawcallLimitFix/Features/Effects/EffectDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Effects
{
	namespace
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;

		constexpr std::uint64_t Bit(Flag a_flag) { return static_cast<std::uint64_t>(a_flag); }

		Api::ClassifyInputId lightLimitFix;  // install (NeedLightLimitFix)

		/** @brief How DeriveEffectDescriptors draws a pass it keeps: the decal, opaque or forward group, and the blend mode. */
		struct EffectDraw
		{
			bool decal = false;
			bool blending = false;
			bool opaque = false;
			bool particles = false;
			std::uint32_t blendMode = 0;
		};

		/** @brief Why the pass stays native (the variants, the opaque rule, the blend pair), or None with how it is drawn in a_draw. */
		Ineligible EffectDrawOf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, EffectDraw& a_draw)
		{
			const std::uint64_t f = a_property.flags;
			const auto* alpha = a_leaf.alpha;
			const std::uint32_t variants = EffectVariantsOf(a_property, a_leaf);
			if (variants & ~kEffectVariantBlendPair)
				return Ineligible::EffectVariant;
			// Hint 1 (blended list, sorted with blended Lighting) for a blending alpha property, blend mode from FUN_14150bc80; without one,
			// hint 0 (G-buffer DEFERRED variant), or hint 1 by Falloff or property alpha alone (blend mode then follows the frame's alpha).
			// A decal takes hint 2 without a blending alpha property and 3 with one, whatever its alpha or Falloff; FUN_1414b2330 puts
			// them in geometry groups 3 and 4, the engine's opaque and blended decal groups (DCLF's 1 and 2).
			a_draw.decal = (f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) != 0;
			a_draw.blending = a_leaf.AlphaBlending();
			// The opaque effects (Api::ObjectClass::OpaqueEffect, taken at install: CS_DCLF_OPAQUE_EFFECTS): hint 0 (G-buffer opaque batches,
			// DEFERRED: forge coals, slag) drawn through decal group 1: depth by
			// the decal depth pass (LESS_EQUAL, no bias), colour with write mode 1 and no blending. Only a pass that is hint 0 every frame:
			// no Falloff, depth tested and written (SetupGeometry's mode 3). A property alpha below one makes the engine's pass hint 1,
			// blended by the frame's alpha: DCLF keeps it opaque (dclf-architecture.md, "Opaque effects").
			const bool opaqueEffects = Extensions::ObjectClassTaken(Api::ObjectClass::OpaqueEffect);
			a_draw.opaque = !a_draw.blending && !a_draw.decal;
			if (a_draw.opaque && (!opaqueEffects || !Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Decal) || (f & Bit(Flag::kFalloff)) || !(f & Bit(Flag::kZBufferTest)) ||
									 !(f & Bit(Flag::kZBufferWrite)) || ParticleGeometryType(a_leaf.Type())))
				return Ineligible::EffectOpaque;
			// A taken particle system: hint 1, the forward epoch's.
			a_draw.particles = ParticleGeometryType(a_leaf.Type());
			if (variants & kEffectVariantBlendPair)
				return Ineligible::EffectVariant;  // a pair the alpha setup leaves as it finds it (only ever a blending one)
			a_draw.blendMode = a_draw.blending ? AlphaBlendModeOf(*alpha) : 0u;
			return Ineligible::None;
		}

		/** @brief The pass descriptor FUN_1414bde90 makes of the property, its alpha property and its geometry. */
		std::uint32_t EffectPassOf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, bool a_particles)
		{
			const std::uint64_t f = a_property.flags;
			const auto* alpha = a_leaf.alpha;
			const std::uint64_t vertexDesc = a_leaf.geometry ? a_leaf.geometry->vertexDesc : 0;
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
			if ((f & Bit(Flag::kFalloff)) && !a_particles)
				d |= Normals | Falloff;
			if (a_particles) {
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
			return d;
		}

		/** @brief The descriptors of pass a_pass drawn as a_draw: the shader descriptors and the opaque, decal or forward group's state. */
		void WriteEffectDescriptors(std::uint32_t a_pass, std::uint64_t a_flags, const EffectDraw& a_draw, LightingDescriptors& a_out)
		{
			using namespace EffectFlag;
			const std::uint32_t d = a_pass;
			// SetupTechnique's split (FUN_141521300, FUN_141521310), then State::ModifyShaderLookup: the three permutation bits off both,
			// Deferred on the pixel stage only in the deferred pass (decals, opaque effects), never for forward objects.
			constexpr std::uint32_t kPermutationBits = GrayscaleToColor | GrayscaleToAlpha | IgnoreTexAlpha;
			a_out = {};
			a_out.port = kMaterialKindEffect;
			a_out.technique = 0;
			a_out.pass = d;
			a_out.derivedPass = d;
			a_out.rawVertex = d & 0xffc7ffffu;
			a_out.rawPixel = ((d & Membrane) ? d : (d & ~Skinned)) & ~SkyObject;
			a_out.vertex = a_out.rawVertex & ~kPermutationBits;
			a_out.pixel = (a_out.rawPixel & ~kPermutationBits) | ((a_draw.decal || a_draw.opaque) ? Deferred : 0u);
			if (a_draw.opaque) {
				a_out.opaqueInDecalGroup = true;
				a_out.decalGroup = 1u;
				a_out.decalBlendMode = 0u;
				a_out.decalWriteMode = 1u;
				return;
			}
			if (a_draw.decal) {
				// Decal group state (engine notes, "Decals"; BSEffectShader::SetupGeometry 0x1414eab10, hints 2 and 3): the group's depth
				// mode (3 opaque, 1 blended) and bias, which SetupGeometry leaves; alpha property applied in the blended group alone (render
				// flags 0x45 vs 0x41); write mode 11 without MotionVectorsNormals (SetupGeometry), else the group's (10 opaque, 11 blended).
				a_out.decalGroup = a_draw.blending ? 2u : 1u;
				a_out.decalBlendMode = a_draw.blendMode;
				a_out.decalWriteMode = (!a_draw.blending && (d & MotionVectorsNormals)) ? 10u : 11u;
				return;
			}
			a_out.forward = true;
			a_out.forwardBlendMode = a_draw.blendMode;
			a_out.forwardDepthTest = (a_flags & Bit(Flag::kZBufferTest)) != 0;
			a_out.forwardDepthWrite = (a_flags & Bit(Flag::kZBufferWrite)) != 0;
		}
	}

	bool EffectSkinned(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf)
	{
		// FUN_1414bde90: kSkinned and the geometry's vertex description (+0x148) with a skin flag (VF_SKINNED's bits, 0x1004000000000000).
		constexpr std::uint64_t kVertexSkinned = 0x1004000000000000ull;
		const std::uint64_t vertexDesc = a_leaf.geometry ? a_leaf.geometry->vertexDesc : 0;
		return (a_property.flags & Bit(Flag::kSkinned)) && (vertexDesc & kVertexSkinned);
	}

	std::uint32_t ParticleRejectionOf(const SceneCapture::LeafView& a_leaf, std::uint64_t a_flags)
	{
		// PaletteItemClassOf's conditions, a bit each, then the property's: a particle pass is hint 1 whatever its alpha, so without
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
		if (EffectSkinned(a_property, a_leaf) != a_leaf.Skinned() || (a_leaf.Skinned() && !Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Skinned)))
			variants |= kEffectVariantSkinned;
		// Effect decals (hints 2 and 3) go to DCLF's decal groups (G-buffer pass, DEFERRED). kTempRefraction with kDynamicDecal
		// registers no pass at all (GetRenderPasses), so it stays a variant.
		if ((f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) &&
			(!Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Decal) || ((f & Bit(Flag::kTempRefraction)) && (f & Bit(Flag::kDynamicDecal)))))
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
			if (!Extensions::ClassifyInputOn(lightLimitFix))
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
		// dclf-architecture.md, "The Effect shader in the forward epoch": whether and how the pass is drawn, its descriptor, and the
		// descriptors and group state it is drawn with.
		EffectDraw draw;
		if (const Ineligible reason = EffectDrawOf(a_property, a_leaf, draw); reason != Ineligible::None)
			return reason;
		WriteEffectDescriptors(EffectPassOf(a_property, a_leaf, draw.particles), a_property.flags, draw, a_out);
		return Ineligible::None;
	}

	void NeedLightLimitFix(Api::InstallContext& a_context)
	{
		lightLimitFix = a_context.NeedClassifyInput("light limit fix");
	}
}
