// TruePBR's material contributor: its SetupMaterial replacement (EvaluatePBR, pure), its classes' capture (any thread the material is
// alive on) and its frame sample (render thread, the frame's start).

#include "Features/DrawcallLimitFix/Integrations/TruePBR/TruePBRIntegration.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialHookFrame.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"

#include <cstddef>
#include <cstring>
#include <utility>

#include "Globals.h"
#include "ShaderCache.h"
#include "TruePBR.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "TruePBR/BSLightingShaderMaterialPBRLandscape.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace DCLF::Integrations::TruePBR
{
	namespace
	{
		using PBR = BSLightingShaderMaterialPBR;
		using PBRLandscape = BSLightingShaderMaterialPBRLandscape;
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using Flag = SIE::ShaderCache::LightingShaderFlags;
		using Evaluation = Api::MaterialContributorEvaluation;
		using MaterialPort::kMaxMaterialBytes;
		using MaterialPort::kMaxTextureFields;

		static_assert(sizeof(PBR) <= kMaxMaterialBytes);
		static_assert(sizeof(PBRLandscape) <= kMaxMaterialBytes);
		// Its registers are TruePBR's extended textures, in register order.
		static_assert(kTextureRegisters.size() == ::TruePBR::NumExtendedTextures && kTextureRegisters.front() == ::TruePBR::FirstExtendedTexture &&
					  kTextureRegisters.back() == ::TruePBR::FirstExtendedTexture + ::TruePBR::NumExtendedTextures - 1);

		constexpr std::uint32_t Bit(Flag a_flag) { return static_cast<std::uint32_t>(a_flag); }

		constexpr auto kLandscapeFeature = static_cast<std::uint32_t>(RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend);  // PBRLandscape::GetFeature

		constexpr auto kAnisotropic = static_cast<std::uint32_t>(RE::BSGraphics::TextureFilterMode::kAnisotropic);
		constexpr auto kBilinear = static_cast<std::uint32_t>(RE::BSGraphics::TextureFilterMode::kBilinear);
		constexpr auto kWrap = static_cast<std::uint32_t>(RE::BSGraphics::TextureAddressMode::kWrapSWrapT);
		constexpr auto kClamp = static_cast<std::uint32_t>(RE::BSGraphics::TextureAddressMode::kClampSClampT);

		constexpr std::uint32_t kVSTexcoordOffset = 11;
		constexpr std::uint32_t kCharacterLightSlot = 11;
		constexpr std::uint32_t kNormalStart = 7;  // TruePBR's landscape normals: t7..t12

		// Fields (the classes are this repo's: offsetof gives what TruePBR's compiled code reads).
		constexpr std::uint16_t Off(std::size_t a_offset) { return static_cast<std::uint16_t>(a_offset); }
		constexpr std::uint16_t kTexCoordOffset = Off(offsetof(RE::BSShaderMaterial, texCoordOffset));
		constexpr std::uint16_t kTexCoordScale = Off(offsetof(RE::BSShaderMaterial, texCoordScale));
		constexpr std::uint16_t kSpecularColor = Off(offsetof(RE::BSLightingShaderMaterialBase, specularColor));
		constexpr std::uint16_t kDiffuse = Off(offsetof(RE::BSLightingShaderMaterialBase, diffuseTexture));
		constexpr std::uint16_t kDiffuseTarget = Off(offsetof(RE::BSLightingShaderMaterialBase, diffuseRenderTargetSourceIndex));
		constexpr std::uint16_t kNormal = Off(offsetof(RE::BSLightingShaderMaterialBase, normalTexture));
		constexpr std::uint16_t kRimSoft = Off(offsetof(RE::BSLightingShaderMaterialBase, rimSoftLightingTexture));
		constexpr std::uint16_t kSpecularBack = Off(offsetof(RE::BSLightingShaderMaterialBase, specularBackLightingTexture));
		constexpr std::uint16_t kClampMode = Off(offsetof(RE::BSLightingShaderMaterialBase, textureClampMode));
		constexpr std::uint16_t kSpecularPower = Off(offsetof(RE::BSLightingShaderMaterialBase, specularPower));
		constexpr std::uint16_t kSpecularColorScale = Off(offsetof(RE::BSLightingShaderMaterialBase, specularColorScale));
		constexpr std::uint16_t kSubSurfaceRolloff = Off(offsetof(RE::BSLightingShaderMaterialBase, subSurfaceLightRolloff));
		constexpr std::uint16_t kRimLightPower = Off(offsetof(RE::BSLightingShaderMaterialBase, rimLightPower));

		constexpr std::uint16_t kPbrFlags = Off(offsetof(PBR, pbrFlags));
		constexpr std::uint16_t kCoatRoughness = Off(offsetof(PBR, coatRoughness));
		constexpr std::uint16_t kCoatSpecularLevel = Off(offsetof(PBR, coatSpecularLevel));
		constexpr std::uint16_t kFuzzColor = Off(offsetof(PBR, fuzzColor));
		constexpr std::uint16_t kFuzzWeight = Off(offsetof(PBR, fuzzWeight));
		constexpr std::uint16_t kGlint = Off(offsetof(PBR, glintParameters));
		constexpr std::uint16_t kRmaos = Off(offsetof(PBR, rmaosTexture));
		constexpr std::uint16_t kEmissive = Off(offsetof(PBR, emissiveTexture));
		constexpr std::uint16_t kDisplacement = Off(offsetof(PBR, displacementTexture));
		constexpr std::uint16_t kFeatures0 = Off(offsetof(PBR, featuresTexture0));
		constexpr std::uint16_t kFeatures1 = Off(offsetof(PBR, featuresTexture1));
		constexpr std::uint16_t kProjectedScale = Off(offsetof(PBR, projectedMaterialBaseColorScale));
		constexpr std::uint16_t kProjectedRoughness = Off(offsetof(PBR, projectedMaterialRoughness));
		constexpr std::uint16_t kProjectedSpecularLevel = Off(offsetof(PBR, projectedMaterialSpecularLevel));
		constexpr std::uint16_t kProjectedGlint = Off(offsetof(PBR, projectedMaterialGlintParameters));

		constexpr std::uint16_t kLandBaseColor = Off(offsetof(PBRLandscape, landscapeBaseColorTextures));
		constexpr std::uint16_t kLandIsPbr = Off(offsetof(PBRLandscape, isPbr));
		constexpr std::uint16_t kLandRoughness = Off(offsetof(PBRLandscape, roughnessScales));
		constexpr std::uint16_t kLandOverlay = Off(offsetof(PBRLandscape, terrainOverlayTexture));
		constexpr std::uint16_t kLandNoise = Off(offsetof(PBRLandscape, terrainNoiseTexture));
		constexpr std::uint16_t kLandNormal = Off(offsetof(PBRLandscape, landscapeNormalTextures));
		constexpr std::uint16_t kLandTexOffsetX = Off(offsetof(PBRLandscape, terrainTexOffsetX));
		constexpr std::uint16_t kLandTexOffsetY = Off(offsetof(PBRLandscape, terrainTexOffsetY));
		constexpr std::uint16_t kLandTexFade = Off(offsetof(PBRLandscape, terrainTexFade));
		constexpr std::uint16_t kLandDisplacement = Off(offsetof(PBRLandscape, landscapeDisplacementTextures));
		constexpr std::uint16_t kLandRmaos = Off(offsetof(PBRLandscape, landscapeRMAOSTextures));
		constexpr std::uint16_t kLandDisplacementScale = Off(offsetof(PBRLandscape, displacementScales));
		constexpr std::uint16_t kLandSpecularLevel = Off(offsetof(PBRLandscape, specularLevels));
		constexpr std::uint16_t kLandGlint = Off(offsetof(PBRLandscape, glintParameters));

		constexpr std::uint32_t kTiles = PBRLandscape::NumTiles;
		// The landscape's displacement layers are its feature textures from local 0, its RMAOS layers from local kTiles.
		static_assert(2 * kTiles == kTextureRegisters.size());

		/** @brief Element a_index of an array field (a_stride bytes each). */
		constexpr std::uint16_t Element(std::uint16_t a_base, std::uint32_t a_index, std::size_t a_stride)
		{
			return static_cast<std::uint16_t>(a_base + a_index * a_stride);
		}
		constexpr std::uint16_t Pointer(std::uint16_t a_base, std::uint32_t a_index) { return Element(a_base, a_index, sizeof(void*)); }
		constexpr std::uint16_t Float(std::uint16_t a_base, std::uint32_t a_index) { return Element(a_base, a_index, sizeof(float)); }
		constexpr std::uint16_t GlintOf(std::uint16_t a_base, std::uint32_t a_index) { return Element(a_base, a_index, sizeof(GlintParameters)); }

		/** @brief SetupTechniqueDescriptor (LightingDescriptors.cpp) from the frame's bytes; idempotent, so a held descriptor passes unchanged. */
		std::uint32_t HeldDescriptor(std::uint32_t a_pass, const MaterialPort::HookFrame& a_frame)
		{
			const std::uint32_t technique = a_pass & 0x3f000000u;
			if (technique == 0x12000000u && !a_frame.techniqueByte12)
				return (a_pass & 0xc9ffffffu) | 0x9000000u;
			if (technique == 0x7000000u && !a_frame.techniqueByte7)
				return a_pass & 0xc0ffffffu;
			return a_pass;
		}

		Technique TechniqueOf(std::uint32_t a_held) { return static_cast<Technique>((a_held >> 24) & 0x3f); }

		/** @brief RendererShadowState's Set* calls, in the evaluation's record terms. */
		struct Writer
		{
			Evaluation& out;

			// SetPSConstant<T>: sizeof(T) bytes at the variable's offset, the rest of the float4 untouched.
			void PS(std::uint32_t a_variable, const float* a_values, std::uint32_t a_count)
			{
				std::memcpy(out.PSFloats() + LightingPSLayout().offset[a_variable], a_values, a_count * sizeof(float));
			}
			void PSBits(std::uint32_t a_variable, std::uint32_t a_bits)
			{
				std::memcpy(out.PSFloats() + LightingPSLayout().offset[a_variable], &a_bits, sizeof(a_bits));
			}
			void VS(std::uint32_t a_variable, const float* a_values, std::uint32_t a_count)
			{
				std::memcpy(out.VSFloats() + LightingVSLayout().offset[a_variable], a_values, a_count * sizeof(float));
			}
			// SetPSTexture + SetPSTextureAddressMode (+ SetPSTextureFilterMode).
			void Texture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_address) { out.BindTexture(a_slot, a_view, a_address); }
			void Texture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_address, std::uint32_t a_filter)
			{
				Texture(a_slot, a_view, a_address);
				out.SetFilterMode(a_slot, a_filter);
			}
			// Its own register (kTextureRegisters' a_local): TruePBR's extendedRendererState.SetPSTexture.
			void FeatureTexture(std::uint32_t a_local, ID3D11ShaderResourceView* a_view) { out.FeatureTexture(a_local, a_view); }
		};

		/** @brief GlintParameters (TruePBR.h) at a_offset: enabled, then four floats. */
		struct Glint
		{
			bool enabled;
			float values[4];  // screenSpaceScale, 40 - logMicrofacetDensity, microfacetRoughness, densityRandomization
		};
		Glint ReadGlint(const Evaluation& a_snapshot, std::uint16_t a_offset)
		{
			Glint glint{};
			glint.enabled = a_snapshot.At<bool>(a_offset + offsetof(GlintParameters, enabled));
			glint.values[0] = a_snapshot.At<float>(a_offset + offsetof(GlintParameters, screenSpaceScale));
			glint.values[1] = 40.f - a_snapshot.At<float>(a_offset + offsetof(GlintParameters, logMicrofacetDensity));
			glint.values[2] = a_snapshot.At<float>(a_offset + offsetof(GlintParameters, microfacetRoughness));
			glint.values[3] = a_snapshot.At<float>(a_offset + offsetof(GlintParameters, densityRandomization));
			return glint;
		}

		// TruePBR::BSLightingShader_SetupMaterial, MTLand and MTLandLODBlend (BSLightingShaderMaterialPBRLandscape).
		void Landscape(const Evaluation& a_snapshot, const MaterialPort::HookFrame& a_frame, Writer& a_write)
		{
			const auto& ps = ShaderConstants::LightingPS::Get();
			// Every tile's slots, a missing texture by the default; the displacement (black) and RMAOS (white) layers go to TruePBR's own
			// t80-t91 (extendedRendererState: views only, sampled through the colour sampler, s0), its feature textures (kTextureRegisters).
			// Without them a full-PBR tile samples no RMAOS: AO 0 (no ambient), roughness 0.
			const auto orDefault = [&](std::uint16_t a_offset, ID3D11ShaderResourceView* a_default) {
				return a_snapshot.HasTexture(a_offset) ? a_snapshot.View(a_offset) : a_default;
			};
			for (std::uint32_t tile = 0; tile < kTiles; ++tile) {
				a_write.Texture(tile, orDefault(Pointer(kLandBaseColor, tile), a_frame.defaultBlackView), kWrap, kAnisotropic);
				a_write.Texture(kNormalStart + tile, orDefault(Pointer(kLandNormal, tile), a_frame.defaultNormalMapView), kWrap, kAnisotropic);
				a_write.FeatureTexture(tile, orDefault(Pointer(kLandDisplacement, tile), a_frame.defaultBlackView));
				a_write.FeatureTexture(kTiles + tile, orDefault(Pointer(kLandRmaos, tile), a_frame.defaultWhiteView));
			}
			if (a_snapshot.HasTexture(kLandOverlay))
				a_write.Texture(13, a_snapshot.View(kLandOverlay), kClamp, kAnisotropic);
			if (a_snapshot.HasTexture(kLandNoise))
				a_write.Texture(15, a_snapshot.View(kLandNoise), kWrap, kBilinear);

			std::uint32_t flags = 0;
			for (std::uint32_t tile = 0; tile < kTiles; ++tile) {
				if (!a_snapshot.At<bool>(kLandIsPbr + tile))
					continue;
				flags |= 1u << tile;
				const auto* displacement = a_snapshot.At<const void*>(Pointer(kLandDisplacement, tile));
				if (displacement != nullptr && displacement != a_frame.defaultBlack)
					flags |= 1u << (kTiles + tile);
				if (ReadGlint(a_snapshot, GlintOf(kLandGlint, tile)).enabled)
					flags |= 1u << (2 * kTiles + tile);
			}
			a_write.PSBits(ps.PBRFlags, flags);

			for (std::uint32_t tile = 0; tile < kTiles; ++tile) {
				const float params[3]{ a_snapshot.At<float>(Float(kLandRoughness, tile)),
					a_snapshot.At<float>(Float(kLandDisplacementScale, tile)), a_snapshot.At<float>(Float(kLandSpecularLevel, tile)) };
				a_write.PS(ps.PBRParams1 + tile, params, 3);
				const auto glint = ReadGlint(a_snapshot, GlintOf(kLandGlint, tile));
				a_write.PS(ps.LandscapeTexture1GlintParameters + tile, glint.values, 4);
			}

			const float lodTexParams[4]{ a_snapshot.At<float>(kLandTexOffsetX), a_snapshot.At<float>(kLandTexOffsetY), 1.f, a_snapshot.At<float>(kLandTexFade) };
			a_write.PS(ps.LODTexParams, lodTexParams, 4);
		}

		// TruePBR::BSLightingShader_SetupMaterial, None and TreeAnim (BSLightingShaderMaterialPBR), its textures checked present.
		void Object(const Evaluation& a_snapshot, std::uint32_t a_lightingFlags, const MaterialPort::HookFrame& a_frame, Writer& a_write)
		{
			const auto& ps = ShaderConstants::LightingPS::Get();
			const auto clampMode = static_cast<std::uint32_t>(a_snapshot.At<std::int32_t>(kClampMode));
			const auto diffuseTarget = a_snapshot.At<std::int32_t>(kDiffuseTarget);
			a_write.Texture(0, diffuseTarget != -1 ? MaterialPort::RenderTargetView(a_frame, diffuseTarget) : a_snapshot.View(kDiffuse), clampMode, kAnisotropic);
			a_write.Texture(1, a_snapshot.View(kNormal), clampMode, kAnisotropic);
			a_write.Texture(5, a_snapshot.View(kRmaos), clampMode, kAnisotropic);

			const auto pbrFlags = a_snapshot.At<std::uint32_t>(kPbrFlags);
			const auto has = [&](PBRFlags a_flag) { return (pbrFlags & static_cast<std::uint32_t>(a_flag)) != 0; };
			std::uint32_t shaderFlags = 0;
			const auto set = [&](PBRShaderFlags a_flag) { shaderFlags |= static_cast<std::uint32_t>(a_flag); };
			const auto color = [&](std::uint16_t a_offset, std::uint16_t a_fourth, float (&a_out)[4]) {
				a_out[0] = a_snapshot.At<float>(a_offset);
				a_out[1] = a_snapshot.At<float>(a_offset + 4);
				a_out[2] = a_snapshot.At<float>(a_offset + 8);
				a_out[3] = a_snapshot.At<float>(a_fourth);
			};

			if (has(PBRFlags::TwoLayer)) {
				set(PBRShaderFlags::TwoLayer);
				if (has(PBRFlags::InterlayerParallax))
					set(PBRShaderFlags::InterlayerParallax);
				if (has(PBRFlags::CoatNormal))
					set(PBRShaderFlags::CoatNormal);
				if (has(PBRFlags::ColoredCoat))
					set(PBRShaderFlags::ColoredCoat);
				float params2[4];  // coat colour (specularColor), strength (subSurfaceLightRolloff)
				color(kSpecularColor, kSubSurfaceRolloff, params2);
				a_write.PS(ps.PBRParams2, params2, 4);
				const float params3[4]{ a_snapshot.At<float>(kCoatRoughness), a_snapshot.At<float>(kCoatSpecularLevel), 0.f, 0.f };
				a_write.PS(ps.MultiLayerParallaxData, params3, 4);
			} else if (has(PBRFlags::HairMarschner)) {
				set(PBRShaderFlags::HairMarschner);
			} else {
				if (has(PBRFlags::Subsurface)) {
					set(PBRShaderFlags::Subsurface);
					float params2[4];  // subsurface colour (specularColor), opacity (subSurfaceLightRolloff)
					color(kSpecularColor, kSubSurfaceRolloff, params2);
					a_write.PS(ps.PBRParams2, params2, 4);
				}
				if (has(PBRFlags::Fuzz)) {
					set(PBRShaderFlags::Fuzz);
					float params3[4];
					color(kFuzzColor, kFuzzWeight, params3);
					a_write.PS(ps.MultiLayerParallaxData, params3, 4);
				} else {
					if (const auto glint = ReadGlint(a_snapshot, kGlint); glint.enabled) {
						set(PBRShaderFlags::Glint);
						a_write.PS(ps.MultiLayerParallaxData, glint.values, 4);
					}
					if ((a_lightingFlags & Bit(Flag::ProjectedUV)) != 0) {
						if (const auto glint = ReadGlint(a_snapshot, kProjectedGlint); glint.enabled) {
							set(PBRShaderFlags::ProjectedGlint);
							a_write.PS(ps.SparkleParams, glint.values, 4);
						}
					}
				}
			}

			const float projected1[4]{ a_snapshot.At<float>(kProjectedScale), a_snapshot.At<float>(kProjectedScale + 4), a_snapshot.At<float>(kProjectedScale + 8), 0.f };
			a_write.PS(ps.MaterialObjectRGBScale, projected1, 4);
			const float projected2[4]{ a_snapshot.At<float>(kProjectedRoughness), a_snapshot.At<float>(kProjectedSpecularLevel), 0.f, 0.f };
			a_write.PS(ps.ParallaxOccData, projected2, 4);

			// Optional maps: bound only when present and not the default they stand for.
			const auto optionalMap = [&](std::uint16_t a_offset, const void* a_default, std::uint32_t a_slot, PBRShaderFlags a_flag) {
				const auto* texture = a_snapshot.At<const void*>(a_offset);
				if (texture == nullptr || texture == a_default)
					return;
				a_write.Texture(a_slot, a_snapshot.View(a_offset), clampMode, kAnisotropic);
				set(a_flag);
			};
			optionalMap(kEmissive, a_frame.defaultBlack, 6, PBRShaderFlags::HasEmissive);
			optionalMap(kDisplacement, a_frame.defaultBlack, 4, PBRShaderFlags::HasDisplacement);
			optionalMap(kFeatures0, a_frame.defaultWhite, 12, PBRShaderFlags::HasFeaturesTexture0);
			optionalMap(kFeatures1, a_frame.defaultWhite, 9, PBRShaderFlags::HasFeaturesTexture1);

			a_write.PSBits(ps.PBRFlags, shaderFlags);

			const float params1[3]{ a_snapshot.At<float>(kSpecularColorScale), a_snapshot.At<float>(kRimLightPower), a_snapshot.At<float>(kSpecularPower) };
			a_write.PS(ps.PBRParams1, params1, 3);
		}
	}

	bool OwnClass(const void* a_vtable)
	{
		static const auto [begin, end] = [] {
			const auto base = reinterpret_cast<std::uintptr_t>(&__ImageBase);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
			return std::pair{ base, base + nt->OptionalHeader.SizeOfImage };
		}();
		const auto address = reinterpret_cast<std::uintptr_t>(a_vtable);
		return address >= begin && address < end;
	}

	void SampleFrame(Api::MaterialContributorFrame& a_frame)
	{
		// Which hook is installed and runs (Feature::loaded; the hooks are installed only for loaded features).
		a_frame.SetActive(globals::features::truePBR.loaded);
	}

	bool PBRTakesPass(const Evaluation& a_evaluation)
	{
		const std::uint32_t held = HeldDescriptor(a_evaluation.Pass(), a_evaluation.hook);
		const auto technique = TechniqueOf(held);
		return technique != Technique::LODLand && technique != Technique::LODLandNoise && (held & Bit(Flag::TruePbr)) != 0;
	}

	Api::MaterialOutcome EvaluatePBR(Evaluation& a_evaluation)
	{
		// Called only for a pass PBRTakesPass took, while TruePBR is loaded (Extensions::EvaluateReplace).
		const auto& frame = a_evaluation.hook;
		const std::uint32_t held = HeldDescriptor(a_evaluation.Pass(), frame);
		const std::uint32_t lightingFlags = held & 0xffffffu;
		const auto technique = TechniqueOf(held);

		const bool landscape = technique == Technique::MTLand || technique == Technique::MTLandLODBlend;
		const bool object = technique == Technique::None || technique == Technique::TreeAnim;
		// The hook casts by technique; the port covers only the class that cast is right for.
		if (landscape && (a_evaluation.Feature() != kLandscapeFeature || a_evaluation.Size() < sizeof(PBRLandscape)))
			return Api::MaterialOutcome::Uncovered;
		if (object) {
			if (!a_evaluation.OwnClass() || a_evaluation.Size() < sizeof(PBR))
				return Api::MaterialOutcome::Uncovered;
			// CHECK_PBR_TEXTURE: a missing diffuse, normal or RMAOS returns to the vanilla function (nothing written before it).
			if (!a_evaluation.HasTexture(kDiffuse) || !a_evaluation.HasTexture(kNormal) || !a_evaluation.HasTexture(kRmaos))
				return Api::MaterialOutcome::HandBack;
		}
		if (a_evaluation.Size() < sizeof(RE::BSLightingShaderMaterialBase))
			return Api::MaterialOutcome::Uncovered;

		a_evaluation.ResetRecord();
		Writer write{ a_evaluation };
		if (landscape)
			Landscape(a_evaluation, frame, write);
		else if (object)
			Object(a_evaluation, lightingFlags, frame, write);

		// TexcoordOffset: the buffer index unmasked, as the hook reads it.
		const std::size_t buffer = frame.textureTransformBuffer;
		const float texcoord[4]{ a_evaluation.At<float>(kTexCoordOffset + buffer * 8), a_evaluation.At<float>(kTexCoordOffset + buffer * 8 + 4),
			a_evaluation.At<float>(kTexCoordScale + buffer * 8), a_evaluation.At<float>(kTexCoordScale + buffer * 8 + 4) };
		write.VS(kVSTexcoordOffset, texcoord, 4);

		if (lightingFlags & Bit(Flag::CharacterLight)) {
			// t11 only while the character light has a render target (its filter mode left alone); the params zero while disabled.
			if (frame.characterLightTarget >= 0)
				write.Texture(kCharacterLightSlot, MaterialPort::RenderTargetView(frame, frame.characterLightTarget), kClamp);
			std::array<float, 4> params{};
			if (frame.characterLightEnabled)
				params = frame.characterLightParams;
			write.PS(ShaderConstants::LightingPS::Get().CharacterLightParams, params.data(), 4);
		}
		return Api::MaterialOutcome::Made;
	}

	void PBRTextureFields(const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count)
	{
		a_count = 0;
		const auto add = [&](std::size_t a_offset) {
			if (a_count < kMaxTextureFields)
				a_out[a_count++] = static_cast<std::uint16_t>(a_offset);
		};
		if (static_cast<std::uint32_t>(a_material.GetFeature()) == kLandscapeFeature) {
			for (std::uint32_t tile = 0; tile < kTiles; ++tile) {
				add(Pointer(kLandBaseColor, tile));
				add(Pointer(kLandNormal, tile));
				add(Pointer(kLandDisplacement, tile));
				add(Pointer(kLandRmaos, tile));
			}
			add(kLandOverlay);
			add(kLandNoise);
			return;
		}
		// The base class's four, then the PBR maps (TruePBR binds diffuse, normal, RMAOS, emissive, displacement, features 0 and 1).
		add(kDiffuse);
		add(kNormal);
		add(kRimSoft);
		add(kSpecularBack);
		add(kRmaos);
		add(kEmissive);
		add(kDisplacement);
		add(kFeatures0);
		add(kFeatures1);
	}

	std::uint32_t PBRMaterialBytes(const RE::BSShaderMaterial& a_material)
	{
		return static_cast<std::uint32_t>(a_material.GetFeature()) == kLandscapeFeature ? static_cast<std::uint32_t>(sizeof(PBRLandscape)) :
		                                                                                   static_cast<std::uint32_t>(sizeof(PBR));
	}

	// Landscape: 4 per tile and the overlay and noise; PBR: the base four and five maps.
	static_assert(4 * PBRLandscape::NumTiles + 2 <= kMaxTextureFields);
}
