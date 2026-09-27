#include "LightingDescriptors.h"

#include "ShaderCache.h"
#include "Switches.h"
#include "Toggles.h"
#include "State.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "Features/TerrainBlending.h"
#include "IndirectDraws.h"

namespace DCLF
{
	namespace
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using LightingFlag = SIE::ShaderCache::LightingShaderFlags;

		constexpr std::uint64_t Bit(Flag a_flag) { return static_cast<std::uint64_t>(a_flag); }
		constexpr std::uint32_t Bit(LightingFlag a_flag) { return static_cast<std::uint32_t>(a_flag); }

		/**
		 * @brief CS_DCLF_TREES=1: bring the tree-animation technique into coverage.
		 *
		 * Trees are the whole of the `technique` rejection class outdoors. The histogram of what
		 * Ineligible::Technique actually rejects reads, in the Whiterun exterior, treeAnim(12)=1237
		 * against MTLandLODBlend(19)=96 and MTLand(8)=4 - so one technique is 92% of it.
		 *
		 * It is behind a switch because TreeAnim animates vertices in the vertex shader from
		 * TreeParams (PerGeometry c8) and WindTimers (c9), and DCLF's PerGeometry constants are
		 * per PIPELINE, taken from one template object. If TreeParams varies per tree, the template is
		 * wrong for every other tree on that pipeline - the same shape of defect as the culled
		 * lighting template. Capture parity is what answers that, per variable.
		 */
		bool TreesEnabled()
		{
			return Toggles::Get().Active().trees;
		}

		// Techniques DCLF supports. The rest stay native.
		bool IsSupportedTechnique(std::uint32_t a_technique)
		{
			switch (static_cast<Technique>(a_technique)) {
			case Technique::None:
			case Technique::Envmap:
			case Technique::Glowmap:
			case Technique::Parallax:
				return true;
			case Technique::TreeAnim:
				return TreesEnabled();
			// Actor skin (bodies, hands): the Lighting shader's SKIN path with TintColor, a PerMaterial constant.
			// NPC faces (Facegen: the tint and detail maps at t3/t4), hair (Hair: TintColor) and eyes (Eye: the eye
			// centres, material VS constants): everything they add is written by SetupMaterial, which the material
			// records take from the engine's own evaluation. A face part's positions are FaceSnapshots'.
			case Technique::FacegenRGBTint:
			case Technique::Facegen:
			case Technique::Hair:
			case Technique::Eye:
				return ActorsEnabled();
			case Technique::MTLand:
			case Technique::MTLandLODBlend:
				return MtLandEnabled();
			default:
				return false;
			}
		}

		// [LightingShader] thresholds GetRenderPasses compares the fade node's LOD metric against
		// (engine notes: LOD fades). Defaults are the engine's.
		struct LodFadeSettings
		{
			float specularStart = 0.09f;
			float specularEnd = 0.10f;
			float envmapStart = 0.09f;
			float envmapEnd = 0.10f;
		};
		LodFadeSettings lodFade;

		float ReadSetting(const char* a_name, float a_default)
		{
			auto* setting = RE::GetINISetting(a_name);
			return setting ? setting->GetFloat() : a_default;
		}

		// FUN_14147c470: false when the feature has faded out; otherwise the fade factor in a_fade.
		// a_metric is the fade node's current LOD metric (+0x144); the previous one (+0x148) only
		// feeds the engine's render-pass cache invalidation, which DCLF does not need.
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

		float FadeNodeLodMetric(const RE::BSFadeNode* a_fadeNode)
		{
			// BSFadeNode+0x144 (RUNTIME_DATA unk144) holds a float despite CommonLib's integer type.
			return std::bit_cast<float>(a_fadeNode->GetRuntimeData().unk144);
		}


		// BSLightingShader::SetupTechnique splits the pass descriptor (engine notes, SetupTechnique).
		constexpr std::uint32_t VertexDescriptorFromPass(std::uint32_t d)
		{
			return (d & 0x48007u) | ((d & 0x20a00u) ? 0x200u : 0u) | (d & 0x3f000000u);
		}

		// Community Shaders replaces the pixel split (TruePBR.cpp, BSLightingShader_GetPixelTechnique) so that
		// bits 3-5 (its TruePbr/Deferred flags) survive; only the shadow light count (bits 6-8) is dropped.
		constexpr std::uint32_t PixelDescriptorFromPass(std::uint32_t d)
		{
			std::uint32_t p = d & ~0x1c0u;
			if ((p & 0x4u) == 0)  // Skinned is kept only with ModelSpaceNormals
				p &= ~0x2u;
			return p | 1u;  // VC is always set
		}
	}

	std::uint8_t FadeStateOf(const RE::BSShaderProperty* a_property)
	{
		// The camera dependence of the derivation, reduced to what the classification actually reads. The
		// LOD metric feeds DeriveLightingDescriptors in exactly three ways - reject on !isfinite, clear
		// kSpecular past its fade end, clear kEnvMap past its - so two bits and an invalid marker capture
		// all of it. The fade *floats* the metric also produces are overwritten from the property by
		// RefreshFrameConstants before any draw reads them, so they do not belong in the witness.
		//
		// An object with none of the fade-sensitive flags has no camera dependence at all and returns 0
		// without touching the node, which is the common case.
		if (!a_property)
			return 0;
		const std::uint64_t f = a_property->flags.underlying();
		if (!(f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))))
			return 0;
		const auto* fadeNode = a_property->fadeNode;
		if (!fadeNode)
			return kFadeNoNode;
		return LodFadeStateOf(fadeNode);
	}

	std::uint8_t LodFadeStateOf(const RE::BSFadeNode* a_fadeNode)
	{
		const float metric = FadeNodeLodMetric(a_fadeNode);
		if (!std::isfinite(metric))
			return kFadeInvalid;
		return static_cast<std::uint8_t>((lodFade.specularEnd < metric ? 1u : 0u) | (lodFade.envmapEnd < metric ? 2u : 0u));
	}

	bool FadeSensitive(const RE::BSShaderProperty* a_property)
	{
		return a_property && a_property->fadeNode &&
		       (a_property->flags.underlying() & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap)));
	}

	void RefreshLodFadeSettings()
	{
		lodFade.specularStart = ReadSetting("fSpecularLODFadeStart:LightingShader", 0.09f);
		lodFade.specularEnd = ReadSetting("fSpecularLODFadeEnd:LightingShader", 0.10f);
		lodFade.envmapStart = ReadSetting("fEnvmapLODFadeStart:LightingShader", 0.09f);
		lodFade.envmapEnd = ReadSetting("fEnvmapLODFadeEnd:LightingShader", 0.10f);
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

	namespace
	{
		// Globals BSLightingShader::SetupGeometry's TreeAnim case reads. Module-relative, like the other
		// engine addresses this feature depends on; a wrong one shows up at once as a capture-parity
		// mismatch on VS PerGeometry 4 or 5 rather than as a silent wrong value.
		// Resolved as addresses and read through, rather than as typed Relocations: these are plain data
		// globals, not the function or vtable pointers Relocation's accessors are shaped for.
		REL::Relocation<std::uintptr_t> treeWindFadeStart{ REL::Offset(0x2033100) };
		REL::Relocation<std::uintptr_t> treeWindFadeEnd{ REL::Offset(0x2033104) };
		REL::Relocation<std::uintptr_t> treeWindTimerScale{ REL::Offset(0x1ad28bc) };
		// A pointer to the object whose +0x304 holds the global wind magnitude.
		REL::Relocation<std::uintptr_t> treeWindSource{ REL::Offset(0x2033060) };
		// The fade node the engine treats as "no node" as well as null.
		REL::Relocation<std::uintptr_t> emptyFadeNode{ REL::Offset(0x332a2a0) };

		float GlobalFloat(const REL::Relocation<std::uintptr_t>& a_at)
		{
			return *reinterpret_cast<const float*>(a_at.address());
		}

		// BSFadeNode vtable slot 0x1f8/8: the downcast to BSTreeNode, null for anything that is not one.
		constexpr std::size_t kAsTreeNodeSlot = 0x1f8 / 8;

		const void* AsTreeNode(const RE::BSFadeNode* a_fadeNode)
		{
			if (!a_fadeNode || reinterpret_cast<std::uintptr_t>(a_fadeNode) == emptyFadeNode.address())
				return nullptr;
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(a_fadeNode);
			using Fn = const void* (*)(const RE::BSFadeNode*);
			return reinterpret_cast<Fn>(vtable[kAsTreeNodeSlot])(a_fadeNode);
		}

		// The engine's own square root, reproduced bit for bit (engine notes: Func6, case 0xc).
		float FastSqrt(float a_value)
		{
			// The engine shifts the bit pattern as a SIGNED integer (`sar`). For every real distance the
			// two shifts agree; they differ on a negative pattern, which is what a fern's uninitialised
			// distance field holds, and where the engine's result then clamps to the maximum amplitude.
			const auto bits = std::bit_cast<std::int32_t>(a_value);
			const float estimate = std::bit_cast<float>(static_cast<std::uint32_t>(0x5f3759df - (bits >> 1)));
			return (1.5f - a_value * 0.5f * estimate * estimate) * estimate * a_value;
		}

		float TreeNodeFloat(const void* a_node, std::size_t a_offset)
		{
			return *reinterpret_cast<const float*>(static_cast<const std::byte*>(a_node) + a_offset);
		}
	}

	void DeriveTreeAnim(const RE::BSShaderProperty& a_property, ObjectTreeAnim& a_out)
	{
		const void* node = AsTreeNode(a_property.fadeNode);

		a_out.treeParams[0] = 0.0f;
		a_out.treeParams[1] = *reinterpret_cast<const float*>(
			*reinterpret_cast<const std::uintptr_t*>(treeWindSource.address()) + 0x304);

		// Amplitude falls off with distance. The engine does NOT use sqrtf here: it takes the squared
		// distance at +0x158 through the 0x5f3759df fast inverse square root with one Newton step, and
		// multiplies back by x. Substituting std::sqrt agreed for every tree that clamps at one end of
		// the fade band or the other, and disagreed for the 600 draws a frame actually inside it - which
		// is exactly the population whose amplitude depends on the value rather than on the clamp.
		const float distance = node ? FastSqrt(TreeNodeFloat(node, 0x158)) : 0.0f;
		const float maxAmplitude = node ? TreeNodeFloat(node, 0x15c) : 1.0f;
		const float fadeStart = GlobalFloat(treeWindFadeStart);
		const float span = GlobalFloat(treeWindFadeEnd) - fadeStart;
		float amplitude = (1.0f - (distance - fadeStart) / span) * maxAmplitude;
		amplitude = std::max(amplitude, 0.0f);
		amplitude = std::min(amplitude, maxAmplitude);
		a_out.treeParams[2] = amplitude;
		a_out.treeParams[3] = node ? TreeNodeFloat(node, 0x160) : 1.0f;

		const float scale = GlobalFloat(treeWindTimerScale);
		a_out.windTimers[0] = node ? TreeNodeFloat(node, 0x164) * scale : 0.0f;
		a_out.windTimers[1] = node ? TreeNodeFloat(node, 0x168) * scale : 0.0f;
		// Diagnostics only (the shader reads xy): the raw inputs the amplitude came from, so a parity
		// mismatch on it can say whether the node or its distance was the thing that differed.
		a_out.windTimers[2] = node ? TreeNodeFloat(node, 0x158) : -1.0f;
		a_out.windTimers[3] = node ? TreeNodeFloat(node, 0x15c) : -1.0f;
	}

	bool DecalsEnabled()
	{
		return Toggles::Get().Active().decals;
	}

	bool SkinnedEnabled()
	{
		return Toggles::Get().Active().skinned;
	}

	bool SwitchNodesEnabled()
	{
		return Toggles::Get().Active().switchNodes;
	}

	bool SkinPartitionsEnabled()
	{
		return Toggles::Get().Active().skinPartitions;
	}

	bool ActorsEnabled()
	{
		return Toggles::Get().Active().actors;
	}

	bool FadingEnabled()
	{
		return Toggles::Get().Active().fading;
	}

	bool LodCrossfadeEnabled()
	{
		return Toggles::Get().Active().lodCrossfade;
	}

	bool ProjectedUvEnabled()
	{
		return Toggles::Get().Active().projectedUv;
	}

	bool TerrainBlendingDefersTerrain()
	{
		const auto& terrainBlending = globals::features::terrainBlending;
		return terrainBlending.loaded && terrainBlending.settings.Enabled && IndirectDraws::Hybrid();
	}

	bool MtLandEnabled()
	{
		return Toggles::Get().Active().mtLand && !TerrainBlendingDefersTerrain();
	}

	namespace
	{
		/**
		 * @brief alphaBlendMode as BSShader's alpha setup (AE FUN_14150bc80) chooses it from a blending
		 * NiAlphaProperty's source and destination functions, or 0 when the pair is one it leaves alone.
		 *
		 * The function also picks mode 1 for a non-blending property when the shader property's alpha is
		 * below 1 (a fading object); that case is not eligible here, so it is not modelled.
		 */
		std::uint32_t AlphaBlendModeOf(const RE::NiAlphaProperty& a_alpha)
		{
			using Function = RE::NiAlphaProperty::AlphaFunction;
			const auto source = a_alpha.GetSrcBlendMode();
			const auto dest = a_alpha.GetDestBlendMode();
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

	Ineligible DeriveLightingDescriptors(const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry,
		const AccumulatedPass* a_accumulated, LightingDescriptors& a_out, bool a_wantDerived)
	{
		std::uint64_t f = a_property.flags.underlying();

		if (f & (Bit(Flag::kLODObjects) | Bit(Flag::kHDLODObjects) | Bit(Flag::kLODLandscape)))
			return Ineligible::Lod;
		const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		if (f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) {
			// Decals, from what the engine was measured doing with them (CS_DCLF_DECAL_PROBE; engine notes:
			// decals). A Lighting decal pass carries accumulation hint 2 or 3, and the hint is which
			// geometry group draws it and with what state:
			//   hint 2: depth test+write with the opaque decal bias, blending off, write mode 10, the main
			//           pass's render flags (0x41: the alpha property is NOT applied);
			//   hint 3: depth test only with the blended decal bias, render flags 0x45, so the alpha
			//           property IS applied - blend mode from its functions, alpha test from its flag -
			//           and write mode 1 when the property has kZBufferWrite (SetupGeometry forces it),
			//           11 otherwise (what the group function set).
			// Both test depth against everything opaque and neither occludes anything its host does not,
			// which is why DCLF draws them in a second pass without writing depth (IndirectDraws). Without
			// an accumulated pass there is no hint, and a decal the engine culled is not worth a cull-only
			// candidate: single-phase culling has nothing to rescue.
			if (!DecalsEnabled() || !a_accumulated || !(f & Bit(Flag::kZBufferTest)))
				return Ineligible::Decal;
			if (a_accumulated->hint == 2) {
				if (alpha && alpha->GetAlphaBlending())
					return Ineligible::Decal;  // not measured in this group; the state would be a guess
				a_out.decalGroup = 1;
				a_out.decalBlendMode = 0;
				a_out.decalWriteMode = 10;
			} else if (a_accumulated->hint == 3) {
				// The fading case (alpha < 1 without blending) takes blend mode 1 in the engine and is
				// also what Ineligible::Fading covers; it is left to the native loop.
				if (!alpha || !alpha->GetAlphaBlending())
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
		// kSkinned selects the SKINNED permutation, whose vertex shader wants a palette; with the switch on
		// the palette comes from the geometry's skin instance (SceneStore), so a skinned property without
		// one would draw from nothing and stays native.
		if ((f & Bit(Flag::kSkinned)) && !(SkinnedEnabled() && a_geometry.GetGeometryRuntimeData().skinInstance))
			return Ineligible::Skinned;
		if ((f & Bit(Flag::kProjectedUV)) && !ProjectedUvEnabled())
			return Ineligible::ProjectedUV;
		// Terrain Blending holds these passes (a mesh opts out of the blend with this otherwise unused flag) and
		// redraws them after its terrain, testing EQUAL, so the terrain does not blend over them. DCLF draws
		// before that terrain (DrawcallLimitFix::AfterOpaquePass), so they stay native.
		if ((f & Bit(Flag::kNoTransparencyMultiSample)) && TerrainBlendingDefersTerrain())
			return Ineligible::TerrainNoBlend;
		// Refraction has its own LOD fade that removes the object entirely; leave it to the game.
		if (f & (Bit(Flag::kRefraction) | Bit(Flag::kTempRefraction))) {
			a_out.rejectedTechnique = kRefractionReject;
			return Ineligible::Technique;
		}

		// Blended geometry is drawn after the deferred composite, forward and sorted; the one exception is
		// the engine's blended decal group, which blends inside the G-buffer pass and is handled above.
		if (alpha && alpha->GetAlphaBlending() && a_out.decalGroup != 2)
			return Ineligible::AlphaBlend;

		// The descriptor derived from the property, as GetRenderPasses builds it for an opaque, fully
		// faded-in object. Phase 5 (no native accumulation) depends on it; while the accumulator holds the
		// pass it is only compared with the drawn technique (SceneStore stats, derivation disagreements).
		std::uint32_t derived = 0;
		float specularFade = a_property.specularLODFade;
		float envmapFade = a_property.envmapLODFade;
		// Skipped entirely for an accumulated object unless the probe wants the comparison: see the
		// header. Everything below this point that the accumulated path reads is recomputed there.
		const bool deriveNeeded = !a_accumulated || a_wantDerived;
		const Ineligible derivedReason = !deriveNeeded ? Ineligible::None : [&] {
			// Distance LOD fades GetRenderPasses applies before choosing the technique: specular and the
			// environment map switch off past their [LightingShader] thresholds.
			// Without a fade node GetRenderPasses never computes these fades, so the property fields the draw
			// reads may be stale. Leave such objects native rather than guess.
			if (!a_property.fadeNode && (f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))))
				return Ineligible::Fading;
			if (const auto* fadeNode = a_property.fadeNode) {
				const float metric = FadeNodeLodMetric(fadeNode);
				// A non-finite metric leaves the engine's fades undefined (its clamp keeps the NaN); leave it native.
				if (!std::isfinite(metric))
					return Ineligible::Fading;
				if ((f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow))) && !LodFadeVisible(metric, lodFade.specularStart, lodFade.specularEnd, specularFade))
					f &= ~Bit(Flag::kSpecular);
				if ((f & Bit(Flag::kEnvMap)) && !LodFadeVisible(metric, lodFade.envmapStart, lodFade.envmapEnd, envmapFade)) {
					// kSnow keeps the envmap past its fade distance, but then the engine never writes the fade and
					// the draw reads a stale property field. Leave that case native.
					if (f & Bit(Flag::kSnow))
						return Ineligible::Fading;
					f &= ~Bit(Flag::kEnvMap);
				}
			}

			const std::uint32_t technique = SelectLightingTechnique(f);
			if (!IsSupportedTechnique(technique)) {
				a_out.rejectedTechnique = technique;
				return Ineligible::Technique;
			}

			std::uint32_t d = technique << 24;
			if (f & Bit(Flag::kVertexColors))
				d |= Bit(LightingFlag::VC);
			// GetRenderPasses copies the property's kSkinned straight into bit 1, whether or not the geometry has a
			// skin (static fish and buckets carry it).
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
			if (alpha && alpha->GetAlphaTesting())
				d |= Bit(LightingFlag::DoAlphaTest);
			if (f & Bit(Flag::kCharacterLighting))
				d |= Bit(LightingFlag::CharacterLight);

			// Community Shaders' GetRenderPasses hook (TruePBR.cpp): PBR materials swap Specular for TruePbr,
			// and glint turns on AnisoLighting.
			const auto* material = a_property.material;
			const bool isPbr = (f & Bit(Flag::kVertexLighting)) && material &&
			                   (material->GetFeature() == RE::BSShaderMaterial::Feature::kDefault ||
								   material->GetFeature() == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend);
			if (isPbr) {
				d |= Bit(LightingFlag::TruePbr);
				d &= ~Bit(LightingFlag::Specular);
				if (static_cast<const BSLightingShaderMaterialPBR*>(material)->glintParameters.enabled)
					d |= Bit(LightingFlag::AnisoLighting);
			}
			derived = d;
			return Ineligible::None;
		}();

		std::uint32_t d = 0;
		if (a_accumulated) {
			// The technique the accumulator registered the pass under is what SetupTechnique receives. Its fade
			// decisions and the property's fade values come from the same GetRenderPasses call, which the
			// engine only repeats when the light state changes; a fresh derivation can differ near a fade
			// threshold.
			d = a_accumulated->technique;
			if (!IsSupportedTechnique((d >> 24) & 0x3f)) {
				a_out.rejectedTechnique = (d >> 24) & 0x3f;
				return Ineligible::Technique;
			}
			// The screen-door fade: Lighting.hlsl discards against a 4x4 screen pattern and MaterialData.z, so
			// the object stays opaque and the Z-prepass (which keeps the alpha test) dithers identically.
			if ((d & Bit(LightingFlag::AdditionalAlphaMask)) && !FadingEnabled())
				return Ineligible::Fading;
			a_out.derivedSpecularLODFade = specularFade;
			a_out.derivedEnvmapLODFade = envmapFade;
			specularFade = a_property.specularLODFade;
			envmapFade = a_property.envmapLODFade;
			a_out.derivedPass = deriveNeeded && derivedReason == Ineligible::None ? derived : kNotDerived;
		} else {
			if (derivedReason != Ineligible::None)
				return derivedReason;
			// Without an accumulated pass the per-frame bits are unknown; the derivation's guesses stand.
			d = derived;
			a_out.derivedPass = derived;
		}

		uint vertex = VertexDescriptorFromPass(d);
		uint pixel = PixelDescriptorFromPass(d);
		a_out.rawVertex = vertex;
		a_out.rawPixel = pixel;
		globals::state->ModifyShaderLookup(RE::BSShader::Type::Lighting, vertex, pixel, true);

		a_out.technique = (d >> 24) & 0x3f;
		a_out.pass = d;
		a_out.projectedUV = (d & Bit(LightingFlag::ProjectedUV)) != 0;
		a_out.vertex = vertex;
		a_out.pixel = pixel;
		a_out.specularLODFade = specularFade;
		a_out.envmapLODFade = envmapFade;
		return Ineligible::None;
	}
}
