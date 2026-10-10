#include "Features/DrawcallLimitFix/Scene/MaterialPort.h"

#include "Features/DrawcallLimitFix/Scene/FrameGlobals.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>

#include "Features/Skin.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "TruePBR/BSLightingShaderMaterialPBRLandscape.h"

namespace DCLF::MaterialPort
{
	namespace
	{
		using PBR = BSLightingShaderMaterialPBR;
		using PBRLandscape = BSLightingShaderMaterialPBRLandscape;
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using Flag = SIE::ShaderCache::LightingShaderFlags;

		static_assert(sizeof(PBR) <= kMaxMaterialBytes);
		static_assert(sizeof(PBRLandscape) <= kMaxMaterialBytes);
		static_assert(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kTOTAL) <= kFeatureRenderTargets);

		constexpr std::uint32_t Bit(Flag a_flag) { return static_cast<std::uint32_t>(a_flag); }

		constexpr auto kFaceGen = static_cast<std::uint32_t>(RE::BSShaderMaterial::Feature::kFaceGen);
		constexpr auto kFaceGenRGBTint = static_cast<std::uint32_t>(RE::BSShaderMaterial::Feature::kFaceGenRGBTint);
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
		constexpr std::uint16_t kHashKey = Off(offsetof(RE::BSShaderMaterial, hashKey));
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

		/** @brief Element a_index of an array field (a_stride bytes each). */
		constexpr std::uint16_t Element(std::uint16_t a_base, std::uint32_t a_index, std::size_t a_stride)
		{
			return static_cast<std::uint16_t>(a_base + a_index * a_stride);
		}
		constexpr std::uint16_t Pointer(std::uint16_t a_base, std::uint32_t a_index) { return Element(a_base, a_index, sizeof(void*)); }
		constexpr std::uint16_t Float(std::uint16_t a_base, std::uint32_t a_index) { return Element(a_base, a_index, sizeof(float)); }
		constexpr std::uint16_t GlintOf(std::uint16_t a_base, std::uint32_t a_index) { return Element(a_base, a_index, sizeof(GlintParameters)); }

		/** @brief SetupTechniqueDescriptor (LightingDescriptors.cpp) from the frame's bytes; idempotent, so a held descriptor passes unchanged. */
		std::uint32_t HeldDescriptor(std::uint32_t a_pass, const FeatureFrame& a_frame)
		{
			const std::uint32_t technique = a_pass & 0x3f000000u;
			if (technique == 0x12000000u && !a_frame.techniqueByte12)
				return (a_pass & 0xc9ffffffu) | 0x9000000u;
			if (technique == 0x7000000u && !a_frame.techniqueByte7)
				return a_pass & 0xc0ffffffu;
			return a_pass;
		}

		Technique TechniqueOf(std::uint32_t a_held) { return static_cast<Technique>((a_held >> 24) & 0x3f); }

		void ResetRecord(MaterialRecord& a_out)
		{
			// What RunStandIn's Collect leaves where nothing writes.
			a_out.vs.Reset();
			a_out.ps.Reset();
			a_out.textures = {};
			a_out.addressModes = {};
			a_out.filterModes.fill(kUnwrittenFilterMode);
			a_out.textureWritten = 0;
			a_out.featureTextures = {};
		}

		/** @brief RendererShadowState's Set* calls against the stand-in, in MaterialRecord's terms. */
		struct Writer
		{
			MaterialRecord& out;

			// SetPSConstant<T>: sizeof(T) bytes at the variable's offset, the rest of the float4 untouched.
			void PS(std::uint32_t a_variable, const float* a_values, std::uint32_t a_count)
			{
				std::memcpy(&out.ps.floats[LightingPSLayout().offset[a_variable]], a_values, a_count * sizeof(float));
			}
			void PSBits(std::uint32_t a_variable, std::uint32_t a_bits)
			{
				std::memcpy(&out.ps.floats[LightingPSLayout().offset[a_variable]], &a_bits, sizeof(a_bits));
			}
			void VS(std::uint32_t a_variable, const float* a_values, std::uint32_t a_count)
			{
				std::memcpy(&out.vs.floats[LightingVSLayout().offset[a_variable]], a_values, a_count * sizeof(float));
			}
			// SetPSTexture + SetPSTextureAddressMode (+ SetPSTextureFilterMode).
			void Texture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_address)
			{
				out.textures[a_slot] = a_view;
				out.addressModes[a_slot] = a_address;
				out.textureWritten |= 1u << a_slot;
			}
			void Texture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_address, std::uint32_t a_filter)
			{
				Texture(a_slot, a_view, a_address);
				out.filterModes[a_slot] = a_filter;
			}
		};

		ID3D11ShaderResourceView* RenderTargetView(const FeatureFrame& a_frame, std::int32_t a_index)
		{
			// The engine indexes renderTargets unchecked; an index past the array has no defined view.
			return a_index >= 0 && static_cast<std::uint32_t>(a_index) < kFeatureRenderTargets ? a_frame.renderTargetViews[a_index] : nullptr;
		}

		/** @brief GlintParameters (TruePBR.h) at a_offset: enabled, then four floats. */
		struct Glint
		{
			bool enabled;
			float values[4];  // screenSpaceScale, 40 - logMicrofacetDensity, microfacetRoughness, densityRandomization
		};
		Glint ReadGlint(const MaterialSnapshot& a_snapshot, std::uint16_t a_offset)
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
		void Landscape(const MaterialSnapshot& a_snapshot, const FeatureFrame& a_frame, Writer& a_write)
		{
			const auto& ps = ShaderConstants::LightingPS::Get();
			// Every tile's slots, a missing texture by the default; the displacement and RMAOS layers go to TruePBR's own t80-t91
			// (extendedRendererState), which the stand-in does not capture and MaterialRecord does not hold.
			const auto orDefault = [&](std::uint16_t a_offset, ID3D11ShaderResourceView* a_default) {
				return a_snapshot.HasTexture(a_offset) ? a_snapshot.View(a_offset) : a_default;
			};
			for (std::uint32_t tile = 0; tile < kTiles; ++tile) {
				a_write.Texture(tile, orDefault(Pointer(kLandBaseColor, tile), a_frame.defaultBlackView), kWrap, kAnisotropic);
				a_write.Texture(kNormalStart + tile, orDefault(Pointer(kLandNormal, tile), a_frame.defaultNormalMapView), kWrap, kAnisotropic);
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
		void Object(const MaterialSnapshot& a_snapshot, std::uint32_t a_lightingFlags, const FeatureFrame& a_frame, Writer& a_write)
		{
			const auto& ps = ShaderConstants::LightingPS::Get();
			const auto clampMode = static_cast<std::uint32_t>(a_snapshot.At<std::int32_t>(kClampMode));
			const auto diffuseTarget = a_snapshot.At<std::int32_t>(kDiffuseTarget);
			a_write.Texture(0, diffuseTarget != -1 ? RenderTargetView(a_frame, diffuseTarget) : a_snapshot.View(kDiffuse), clampMode, kAnisotropic);
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

		ID3D11ShaderResourceView* ViewOfTexture(const RE::NiSourceTexture* a_texture)
		{
			const auto* texture = a_texture ? a_texture->rendererTexture : nullptr;
			return texture ? reinterpret_cast<ID3D11ShaderResourceView*>(texture->resourceView) : nullptr;
		}

		/** @brief Skin::skinExtraTextures as MaterialTexturesOf resolves each entry; render thread (the map's only user). */
		std::shared_ptr<const std::vector<SkinTextures>> SampleSkinTextures(ID3D11ShaderResourceView* a_black)
		{
			static std::shared_ptr<const std::vector<SkinTextures>> held;
			static std::vector<SkinTextures> scratch;
			scratch.clear();
			for (const auto& [hashKey, entry] : globals::features::skin.skinExtraTextures) {
				SkinTextures textures{ hashKey, a_black, a_black };
				if (entry.hasExtraTexture || entry.hasWetnessTexture) {
					textures.rfaos = ViewOfTexture(entry.rfaosTexture.get());
					textures.wetness = ViewOfTexture(entry.wetnessTexture.get());
				}
				scratch.push_back(textures);
			}
			std::sort(scratch.begin(), scratch.end(), [](const SkinTextures& a, const SkinTextures& b) { return a.hashKey < b.hashKey; });
			const auto same = [](const SkinTextures& a, const SkinTextures& b) { return a.hashKey == b.hashKey && a.rfaos == b.rfaos && a.wetness == b.wetness; };
			if (!held || !std::equal(held->begin(), held->end(), scratch.begin(), scratch.end(), same))
				held = std::make_shared<const std::vector<SkinTextures>>(scratch);
			return held;
		}

		const SkinTextures* FindSkinTextures(const FeatureFrame& a_frame, std::uint32_t a_hashKey)
		{
			if (!a_frame.skinTextures)
				return nullptr;
			const auto& table = *a_frame.skinTextures;
			const auto it = std::lower_bound(table.begin(), table.end(), a_hashKey, [](const SkinTextures& a, std::uint32_t k) { return a.hashKey < k; });
			return it != table.end() && it->hashKey == a_hashKey ? &*it : nullptr;
		}

		bool SkinApplies(const MaterialSnapshot& a_snapshot, const FeatureFrame& a_frame)
		{
			return a_frame.skin && (a_snapshot.feature == kFaceGen || a_snapshot.feature == kFaceGenRGBTint);
		}
	}

	FeatureFrame SampleFeatureFrame()
	{
		FeatureFrame frame;
		frame.truePBR = globals::features::truePBR.loaded;
		frame.skin = globals::features::skin.loaded;

		const auto& frameGlobals = FrameGlobals::Current();
		frame.techniqueByte12 = frameGlobals.techniqueByte12;
		frame.techniqueByte7 = frameGlobals.techniqueByte7;

		if (const auto* smState = globals::game::smState) {
			frame.textureTransformBuffer = smState->textureTransformCurrentBuffer;
			frame.characterLightEnabled = smState->characterLightEnabled;
			std::copy_n(smState->characterLightParams, 4, frame.characterLightParams.data());
		}
		static const REL::Relocation<RE::ImageSpaceTexture*> characterLightTexture{ RELOCATION_ID(513464, 391302) };
		frame.characterLightTarget = static_cast<std::int32_t>(characterLightTexture->renderTarget);

		if (const auto* renderer = globals::game::renderer) {
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			const std::size_t count = std::min<std::size_t>(std::size(targets), kFeatureRenderTargets);
			for (std::size_t i = 0; i < count; ++i)
				frame.renderTargetViews[i] = reinterpret_cast<ID3D11ShaderResourceView*>(targets[i].SRV);
		}

		if (const auto* graphicsState = globals::game::graphicsState) {
			const auto& state = graphicsState->GetRuntimeData();
			frame.defaultBlack = state.defaultTextureBlack.get();
			frame.defaultWhite = state.defaultTextureWhite.get();
			frame.defaultNormalMap = state.defaultTextureNormalMap.get();
			frame.defaultBlackView = ViewOfTexture(state.defaultTextureBlack.get());
			frame.defaultWhiteView = ViewOfTexture(state.defaultTextureWhite.get());
			frame.defaultNormalMapView = ViewOfTexture(state.defaultTextureNormalMap.get());
		}

		if (frame.skin)
			frame.skinTextures = SampleSkinTextures(frame.defaultBlackView);
		return frame;
	}

	bool PBRTakesPass(std::uint32_t a_passDescriptor, const FeatureFrame& a_frame)
	{
		if (!a_frame.truePBR)
			return false;
		const std::uint32_t held = HeldDescriptor(a_passDescriptor, a_frame);
		const auto technique = TechniqueOf(held);
		return technique != Technique::LODLand && technique != Technique::LODLandNoise && (held & Bit(Flag::TruePbr)) != 0;
	}

	bool EvaluatePBR(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const MaterialFrame& a_frame, MaterialRecord& a_out)
	{
		const auto& frame = a_frame.feature;
		if (!PBRTakesPass(a_passDescriptor, frame))
			return false;
		const std::uint32_t held = HeldDescriptor(a_passDescriptor, frame);
		const std::uint32_t lightingFlags = held & 0xffffffu;
		const auto technique = TechniqueOf(held);

		const bool landscape = technique == Technique::MTLand || technique == Technique::MTLandLODBlend;
		const bool object = technique == Technique::None || technique == Technique::TreeAnim;
		// The hook casts by technique; the port covers only the class that cast is right for.
		if (landscape && (a_snapshot.feature != kLandscapeFeature || a_snapshot.size < sizeof(PBRLandscape)))
			return false;
		if (object) {
			if (!a_snapshot.pbr || a_snapshot.size < sizeof(PBR))
				return false;
			// CHECK_PBR_TEXTURE: a missing diffuse, normal or RMAOS returns to the vanilla function (nothing written before it).
			if (!a_snapshot.HasTexture(kDiffuse) || !a_snapshot.HasTexture(kNormal) || !a_snapshot.HasTexture(kRmaos))
				return EvaluateVanilla(a_snapshot, a_passDescriptor, a_frame.vanilla, a_out);
		}
		if (a_snapshot.size < sizeof(RE::BSLightingShaderMaterialBase))
			return false;

		ResetRecord(a_out);
		Writer write{ a_out };
		if (landscape)
			Landscape(a_snapshot, frame, write);
		else if (object)
			Object(a_snapshot, lightingFlags, frame, write);

		// TexcoordOffset: the buffer index unmasked, as the hook reads it.
		const std::size_t buffer = frame.textureTransformBuffer;
		const float texcoord[4]{ a_snapshot.At<float>(kTexCoordOffset + buffer * 8), a_snapshot.At<float>(kTexCoordOffset + buffer * 8 + 4),
			a_snapshot.At<float>(kTexCoordScale + buffer * 8), a_snapshot.At<float>(kTexCoordScale + buffer * 8 + 4) };
		write.VS(kVSTexcoordOffset, texcoord, 4);

		if (lightingFlags & Bit(Flag::CharacterLight)) {
			// t11 only while the character light has a render target (its filter mode left alone); the params zero while disabled.
			if (frame.characterLightTarget >= 0)
				write.Texture(kCharacterLightSlot, RenderTargetView(frame, frame.characterLightTarget), kClamp);
			std::array<float, 4> params{};
			if (frame.characterLightEnabled)
				params = frame.characterLightParams;
			write.PS(ShaderConstants::LightingPS::Get().CharacterLightParams, params.data(), 4);
		}
		return true;
	}

	bool FeatureHooksCovered(const MaterialSnapshot& a_snapshot, const FeatureFrame& a_frame)
	{
		if (!SkinApplies(a_snapshot, a_frame))
			return true;
		const auto hashKey = a_snapshot.At<std::uint32_t>(kHashKey);
		return hashKey == 0 || FindSkinTextures(a_frame, hashKey) != nullptr;
	}

	void EvaluateFeatureHooks(const MaterialSnapshot& a_snapshot, std::uint32_t, const FeatureFrame& a_frame, MaterialRecord& a_out)
	{
		// Advanced Skin (Skin::MaterialTexturesOf, outermost on slot 4): FaceGen and FaceGenRGBTint materials only; no hash key, or
		// no extra texture, binds black to both. TerrainHelper's t92-t97 and ExtraFeatureDescriptor bits are not in the record
		// (the stand-in restores permutationData and does not capture its slots), so nothing of it is ported here.
		a_out.featureTextures = {};
		if (!SkinApplies(a_snapshot, a_frame))
			return;
		const auto hashKey = a_snapshot.At<std::uint32_t>(kHashKey);
		if (hashKey == 0) {
			a_out.featureTextures = { a_frame.defaultBlackView, a_frame.defaultBlackView };
			return;
		}
		if (const auto* textures = FindSkinTextures(a_frame, hashKey))
			a_out.featureTextures = { textures->rfaos, textures->wetness };
		// A key Advanced Skin has not set up yet: left null (FeatureHooksCovered is false for it).
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
