#pragma once

// The Effect port's descriptors (its material port's derive and variants: EffectMaterial.cpp): a BSEffectShaderProperty geometry's pass
// and shader descriptors, or why it stays native, from the capture records. Pure functions of the records, called on any thread Core
// classifies, joins or runs a membership pass on. Included by the feature's sources only.

#include <array>
#include <cstdint>
#include <string_view>

namespace DCLF
{
	struct LightingDescriptors;
	enum class Ineligible : std::uint8_t;
	namespace Api
	{
		class InstallContext;
	}
	namespace SceneCapture
	{
		struct PropertyRecord;
		struct LeafView;
	}
}

namespace DCLF::Features::Effects
{
	/**
	 * @brief A BSEffectShaderProperty geometry's pass as its GetRenderPasses (0x1414bd890) and descriptor function (FUN_1414bde90)
	 * make it in the main modes, with its shader descriptors: the blended mesh (hint 1: a_out.forward) or the decal (hints 2 and 3:
	 * a_out.decalGroup, the DEFERRED pixel stage), either skinned through the palette path. EffectOpaque for a mesh without a blending
	 * alpha property, EffectVariant for a variant not ported. Sets a_out.port (kMaterialKindEffect) on success: the Effect port's derive.
	 */
	Ineligible DeriveEffectDescriptors(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, LightingDescriptors& a_out);

	/**
	 * @brief CS_DCLF_EFFECTS: the effect variants DCLF does not draw, a bit each. Any but kEffectVariantBlendPair is
	 * Ineligible::EffectVariant; the blend pair is too once the alpha property blends (a non-blending one is EffectOpaque first).
	 */
	enum EffectVariantBit : std::uint32_t
	{
		kEffectVariantShape = 1u << 0,          // any shape but a BSTriShape or a particle system (dynamic shapes)
		kEffectVariantSkinned = 1u << 1,        // EffectSkinned disagrees with the leaf's skin instance, or CS_DCLF_SKINNED off
		kEffectVariantDecal = 1u << 2,          // kDecal / kDynamicDecal with CS_DCLF_DECALS off, or one that registers no pass
		kEffectVariantSkyObject = 1u << 3,      // bit 45: hint 0x12, geometry group 13
		kEffectVariantWireframe = 1u << 4,      // bit 48
		kEffectVariantCloudLighting = 1u << 5,  // kEffectLighting with kCloudLOD: the cloud light is its light 0 (FUN_1414fcf80)
		kEffectVariantLightingNoLlf = 1u << 6,  // kEffectLighting without Light Limit Fix (PerGeometry PLight*)
		kEffectVariantBlood = 1u << 7,          // kWeaponBlood
		kEffectVariantProjectedUv = 1u << 8,    // kProjectedUV
		kEffectVariantBlendPair = 1u << 9,      // a blending pair the engine's alpha setup (FUN_14150bc80) leaves as it finds it
		// Particle rejection causes (ParticleRejectionOf): toggle off; not exactly an NiParticleSystem (strip, mesh); no data or
		// capacity; aspect flags without NiPSysData velocities; no blending alpha property (blend mode is the frame's); decal flags.
		kEffectVariantParticle = 1u << 10,
		kEffectVariantParticleClass = 1u << 11,
		kEffectVariantParticleData = 1u << 12,
		kEffectVariantParticleAligned = 1u << 13,
		kEffectVariantParticleUnblended = 1u << 14,
		kEffectVariantParticleDecal = 1u << 15,
		kEffectVariantCount = 16
	};
	constexpr std::array<std::string_view, kEffectVariantCount> kEffectVariantNames{ "shape", "skinned", "decal", "sky-object", "wireframe",
		"cloud-lighting", "lighting-without-llf", "blood", "projected-uv", "blend-pair", "particle: toggle off", "particle: class",
		"particle: no data", "particle: aligned", "particle: unblended", "particle: decal" };
	/** @brief Every particle cause bit. */
	inline constexpr std::uint32_t kEffectVariantParticles = kEffectVariantParticle | kEffectVariantParticleClass | kEffectVariantParticleData |
	                                                         kEffectVariantParticleAligned | kEffectVariantParticleUnblended | kEffectVariantParticleDecal;
	/** @brief The EffectVariantBit set of an effect property on a leaf, from the records. */
	std::uint32_t EffectVariantsOf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf);
	/** @brief The effect pass descriptor's Skinned bit (FUN_1414bde90): the property's kSkinned and a skinned vertex description. */
	bool EffectSkinned(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf);
	/** @brief The particle cause bits (kEffectVariantParticles) of a particle geometry's pass with a_flags property flags: 0 when taken. */
	std::uint32_t ParticleRejectionOf(const SceneCapture::LeafView& a_leaf, std::uint64_t a_flags);
	/**
	 * @brief Install: the "light limit fix" classify input the variants read (Integrations/LightLimitFix's: loaded), needed by name. Unbound
	 * (the integration not installed) it reads false: kEffectVariantLightingNoLlf.
	 */
	void NeedLightLimitFix(Api::InstallContext& a_context);
}
