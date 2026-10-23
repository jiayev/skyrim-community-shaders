// MaterialPort's vanilla part: BSLightingShader::SetupMaterial for the vanilla material classes, and the VanillaFrame sample
// (render thread, the frame's start). Evaluation is pure.

#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include <array>
#include <bit>
#include <cstring>
#include <initializer_list>

namespace DCLF::MaterialPort
{
	namespace
	{
		// BSLightingShader technique flags (+0x94) SetupMaterial (1414dc310) tests.
		constexpr std::uint32_t kModelSpaceNormals = 1u << 2;
		constexpr std::uint32_t kSpecular = 1u << 9;
		constexpr std::uint32_t kSoftLighting = 1u << 10;
		constexpr std::uint32_t kRimLighting = 1u << 11;
		constexpr std::uint32_t kBackLighting = 1u << 12;
		constexpr std::uint32_t kAmbientSpecular = 1u << 17;
		constexpr std::uint32_t kSnow = 1u << 21;
		constexpr std::uint32_t kCharacterLight = 1u << 22;

		// Lighting variable indices (ShaderCache.cpp GetVariableIndices). VS through 0x14202aeb8 +0x50 +i, PS through 0x14202aec0 +0x40 +i.
		constexpr std::uint32_t kVSLeftEyeCenter = 9;
		constexpr std::uint32_t kVSRightEyeCenter = 10;
		constexpr std::uint32_t kPSAmbientSpecular = 6;
		constexpr std::uint32_t kPSEnvmapData = 21;
		constexpr std::uint32_t kPSParallaxOccData = 22;
		constexpr std::uint32_t kPSTintColor = 23;
		constexpr std::uint32_t kPSLODTexParams = 24;
		constexpr std::uint32_t kPSSpecularColor = 25;
		constexpr std::uint32_t kPSSparkleParams = 26;
		constexpr std::uint32_t kPSMultiLayerParallaxData = 27;
		constexpr std::uint32_t kPSLightingEffectParams = 28;
		constexpr std::uint32_t kPSLandscape1to4IsSnow = 30;
		constexpr std::uint32_t kPSLandscape5to6IsSnow = 31;
		constexpr std::uint32_t kPSLandscape1to4IsSpecPower = 32;
		constexpr std::uint32_t kPSLandscape5to6IsSpecPower = 33;
		constexpr std::uint32_t kPSSnowRimLightParameters = 34;
		constexpr std::uint32_t kPSCharacterLightParams = 35;

		// BSLightingShaderMaterialBase fields SetupMaterial reads (offsets verified in 1414dc310).
		constexpr std::uint16_t kDiffuse = 0x48;
		constexpr std::uint16_t kDiffuseRenderTarget = 0x50;  // int, -1: the diffuse texture
		constexpr std::uint16_t kNormal = 0x58;
		constexpr std::uint16_t kRimSoftLighting = 0x60;
		constexpr std::uint16_t kSpecularBackLighting = 0x68;
		constexpr std::uint16_t kTextureClampMode = 0x70;

		// The module globals SampleVanillaFrame reads (AE 1.6.1170, SE 1.5.97); VanillaFrame's comments name each by its AE address.
		constexpr Engine::Offset kTechniqueByte12{ 0x2032fdb, 0x1e0de39 };
		constexpr Engine::Offset kTechniqueByte7{ 0x2035500, 0x1e102a0 };
		constexpr Engine::Offset kTextureTransformBuffer{ 0x2033180, 0x1e0dff0 };  // BSShaderManager::State+0x120
		constexpr Engine::Offset kLodTexParamsZ{ 0x2032fda, 0x1e0de38 };
		constexpr Engine::Offset kLandSnowZ{ 0x2035548, 0x1e102e8 };
		constexpr Engine::Offset kLandSnowDivisor{ 0x20355f0, 0x1e10390 };
		constexpr Engine::Offset kAmbientSpecularGlobal{ 0x203315c, 0x1e0dfcc };  // BSShaderManager::State+0xfc
		constexpr Engine::Offset kSnowRim0{ 0x2035590, 0x1e10330 };
		constexpr Engine::Offset kSnowRim1{ 0x20355a8, 0x1e10348 };
		constexpr Engine::Offset kSnowRim2{ 0x20355c0, 0x1e10360 };
		constexpr Engine::Offset kSnowRimW{ 0x20355d8, 0x1e10378 };
		constexpr Engine::Offset kCharacterLightGlobal{ 0x203316c, 0x1e0dfdc };  // BSShaderManager::State+0x10c
		constexpr Engine::Offset kFullBright{ 0x20330a6, 0x1e0df16 };  // BSShaderManager::State+0x46
		constexpr Engine::Offset kCharacterLightTarget{ 0x2033db0, 0x1e0eb50 };  // the character light's ImageSpaceTexture (ID 513464) +8
		constexpr Engine::Offset kEnvMaskDefault{ 0x328ccb8, 0x302c920 };
		constexpr Engine::Offset kRenderTargetViews{ 0x3289230, 0x3028f00 };  // the renderer's render targets' SRVs, 0x30 apart

		constexpr std::uint32_t kOneBits = 0x3f800000u;  // gFloatOne, 0x141ad2870
		constexpr std::uint32_t kZeroBits = 0u;

		/** @brief A vanilla material class: its vtable, its size (its Create's allocation) and its NiSourceTexture pointer fields. */
		struct VanillaClass
		{
			std::uintptr_t vtable = 0;
			std::uint32_t size = 0;
			std::uint32_t textureCount = 0;
			std::array<std::uint16_t, 16> textures{};

			bool IsTexture(std::uint16_t a_offset) const
			{
				for (std::uint32_t i = 0; i < textureCount; ++i)
					if (textures[i] == a_offset)
						return true;
				return false;
			}
		};

		VanillaClass MakeClass(const REL::VariantID& a_vtable, std::uint32_t a_size, std::initializer_list<std::uint16_t> a_extra)
		{
			VanillaClass c;
			c.vtable = a_vtable.address();
			c.size = a_size;
			for (const auto offset : { kDiffuse, kNormal, kRimSoftLighting, kSpecularBackLighting })
				c.textures[c.textureCount++] = offset;
			for (const auto offset : a_extra)
				c.textures[c.textureCount++] = offset;
			return c;
		}

		const std::array<VanillaClass, 14>& Classes()
		{
			// Sizes from each Create (BSLightingShaderMaterialLandscape::Create 1414bb140 allocates 0x158, LODLandscape 1414bb7e0 200).
			// CommonLib's LODLandscape puts its floats at 0xb4; SetupMaterial reads them at 0xb8..0xc0 (the noise texture is 0xb0).
			// In Engine::LightingMaterialVtables' order.
			const auto& v = Engine::LightingMaterialVtables();
			static const std::array<VanillaClass, 14> classes{
				MakeClass(v[0], 0xa0, {}),                                                        // BSLightingShaderMaterial
				MakeClass(v[1], 0xa0, {}),                                                        // Base
				MakeClass(v[2], 0xb8, { 0xa0, 0xa8 }),                                            // Envmap: env, mask
				MakeClass(v[3], 0xd0, { 0xa0, 0xa8 }),                                            // Eye: env, mask
				MakeClass(v[4], 0xa8, { 0xa0 }),                                                  // Glowmap: glow
				MakeClass(v[5], 0xa8, { 0xa0 }),                                                  // Parallax: height
				MakeClass(v[6], 0xb0, { 0xa0 }),                                                  // ParallaxOcc: height
				MakeClass(v[7], 0xb8, { 0xa0, 0xa8, 0xb0 }),                                      // Facegen: tint, detail, subsurface
				MakeClass(v[8], 0xb0, {}),                                                        // FacegenTint
				MakeClass(v[9], 0xb0, {}),                                                        // HairTint
				MakeClass(v[10], 0x158, { 0xa8, 0xb0, 0xb8, 0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0, 0xf8, 0x100 }),  // Landscape: diffuse[5], normal[5], overlay, noise
				MakeClass(v[11], 0xc8, { 0xa0, 0xa8, 0xb0 }),                                     // LODLandscape: parent diffuse, parent normal, noise
				MakeClass(v[12], 0xb0, {}),                                                       // Snow
				MakeClass(v[13], 0xd0, { 0xa0, 0xa8, 0xb0 }),                                     // MultiLayerParallax: layer, env, mask
			};
			return classes;
		}

		const VanillaClass* ClassOf(const void* a_vtable)
		{
			const auto vtable = reinterpret_cast<std::uintptr_t>(a_vtable);
			for (const auto& c : Classes())
				if (c.vtable == vtable)
					return &c;
			return nullptr;
		}

		/**
		 * @brief SetupMaterial's writes against a snapshot: a constant lands at the variable's StageLayout offset plus its component;
		 * a texture slot is written when anything stores to PSTexture; a filter mode when anything stores to PSTextureFilterMode. Any read the engine would do outside the class, or through a null texture it dereferences, fails.
		 */
		struct Port
		{
			const MaterialSnapshot& m;
			const VanillaClass& c;
			const VanillaFrame& frame;
			MaterialRecord& out;
			const StageLayout& vs = LightingVSLayout();
			const StageLayout& ps = LightingPSLayout();
			bool failed = false;

			Port(const MaterialSnapshot& a_m, const VanillaClass& a_c, const VanillaFrame& a_frame, MaterialRecord& a_out) :
				m(a_m), c(a_c), frame(a_frame), out(a_out) {}

			std::uint32_t Bits(std::size_t a_offset)
			{
				if (a_offset + sizeof(std::uint32_t) > c.size)
					failed = true;
				return m.At<std::uint32_t>(a_offset);
			}
			float Float(std::size_t a_offset) { return std::bit_cast<float>(Bits(a_offset)); }

			// A texture field the engine tests for null first.
			bool Has(std::uint16_t a_offset)
			{
				if (!c.IsTexture(a_offset)) {
					failed = true;
					return false;
				}
				return m.HasTexture(a_offset);
			}
			// A texture field the engine dereferences without a test (FUN_1414e0460, FUN_1414e06b0, the inline binds): the view
			// ViewOf resolved (rendererTexture +0x48, then +0x10; null when the texture has no renderer texture).
			ID3D11ShaderResourceView* Texture(std::uint16_t a_offset)
			{
				if (!Has(a_offset)) {
					failed = true;
					return nullptr;
				}
				return m.View(a_offset);
			}

			void PS(std::uint32_t a_variable, std::uint32_t a_component, std::uint32_t a_bits)
			{
				std::memcpy(&out.ps.floats[ps.offset[a_variable] + a_component], &a_bits, sizeof(a_bits));
			}
			void PSFloat(std::uint32_t a_variable, std::uint32_t a_component, float a_value) { PS(a_variable, a_component, std::bit_cast<std::uint32_t>(a_value)); }
			void PSCopy(std::uint32_t a_variable, std::uint32_t a_component, std::size_t a_offset) { PS(a_variable, a_component, Bits(a_offset)); }
			void VSCopy(std::uint32_t a_variable, std::uint32_t a_component, std::size_t a_offset)
			{
				const std::uint32_t bits = Bits(a_offset);
				std::memcpy(&out.vs.floats[vs.offset[a_variable] + a_component], &bits, sizeof(bits));
			}

			void SetTexture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view)
			{
				out.textures[a_slot] = a_view;
				out.textureWritten |= 1u << a_slot;
			}
			void SetAddress(std::uint32_t a_slot, std::uint32_t a_mode)
			{
				out.addressModes[a_slot] = a_mode;
			}
			void SetFilter(std::uint32_t a_slot, std::uint32_t a_mode) { out.filterModes[a_slot] = a_mode; }

			/** @brief FUN_1414e0460(slot, texture, material): the texture's view, and the material's textureClampMode (+0x70). */
			void Bind(std::uint32_t a_slot, std::uint16_t a_offset)
			{
				SetTexture(a_slot, Texture(a_offset));
				SetAddress(a_slot, Bits(kTextureClampMode));
			}
			/** @brief FUN_1414e06b0: the envmap cube in t4, with textureClampMode. */
			void BindCube(std::uint16_t a_offset) { Bind(4, a_offset); }
			/** @brief NiSourceTexture__sub (1414e0710): the envmap mask in t5, the default texture (0x14328ccb8) when there is none. */
			void BindMask(std::uint16_t a_offset)
			{
				SetTexture(5, Has(a_offset) ? m.View(a_offset) : frame.envMaskDefaultView);
				SetAddress(5, Bits(kTextureClampMode));
			}
			/** @brief A render target's SRV (0x143289230 + 0x30 * index); an index outside the array fails. */
			ID3D11ShaderResourceView* RenderTarget(std::int32_t a_index)
			{
				if (a_index < 0 || static_cast<std::uint32_t>(a_index) >= VanillaRenderTargets()) {
					failed = true;
					return nullptr;
				}
				return frame.renderTargetViews[static_cast<std::size_t>(a_index)];
			}
		};

		/** @brief The record before SetupMaterial: nothing written. */
		void ResetRecord(MaterialRecord& a_out)
		{
			a_out.vs.Reset();
			a_out.ps.Reset();
			a_out.textures.fill(nullptr);
			a_out.addressModes.fill(0);
			a_out.filterModes.fill(kUnwrittenFilterMode);
			a_out.textureWritten = 0;
			a_out.featureTextures = {};
		}

		/** @brief The +0x94 the draw has: SetupTechnique's remap (1414db810), idempotent, so a descriptor already remapped passes unchanged. */
		std::uint32_t RemapDescriptor(std::uint32_t a_passDescriptor, const VanillaFrame& a_frame)
		{
			std::uint32_t d = a_passDescriptor;
			if ((d & 0x3f000000u) == 0x12000000u) {
				if (!a_frame.techniqueByte12)
					d = (d & 0xc9ffffffu) | 0x9000000u;
			} else if ((d & 0x3f000000u) == 0x7000000u && !a_frame.techniqueByte7) {
				d &= 0xc0ffffffu;
			}
			return d;
		}

		/** @brief Techniques 8 (MTLand) and 0x13 (MTLandLODBlend). False: the engine would read past the layer arrays. */
		bool EvaluateLandscape(Port& p, std::uint32_t d, const VanillaFrame& a_frame)
		{
			p.PSCopy(kPSLODTexParams, 0, 0x148);
			p.PSCopy(kPSLODTexParams, 1, 0x14c);
			p.PS(kPSLODTexParams, 2, a_frame.lodTexParamsZ ? kOneBits : kZeroBits);
			p.PSCopy(kPSLODTexParams, 3, 0x150);
			// Base diffuse t0 and normal t7, inline with no address mode: SetupTechnique (1414db810, techniques 8/0x13) set every
			// slot's to 3 just before.
			p.SetTexture(0, p.Texture(kDiffuse));
			p.SetAddress(0, 3);
			p.SetTexture(7, p.Texture(kNormal));
			p.SetAddress(7, 3);
			// Layers: diffuse[i] (+0xa8) to t(i+1), normal[i] (+0xd0) to t(i+8), clamp mode 3; numLandscapeTextures (+0xa0) of them.
			const std::uint32_t count = p.Bits(0xa0);
			if (count > 5)
				return false;  // the engine would read past the arrays (diffuse[5] is normal[0])
			for (std::uint32_t i = 0; i < count; ++i) {
				p.SetTexture(i + 1, p.Texture(static_cast<std::uint16_t>(0xa8 + 8 * i)));
				p.SetAddress(i + 1, 3);
				p.SetTexture(i + 8, p.Texture(static_cast<std::uint16_t>(0xd0 + 8 * i)));
				p.SetAddress(i + 8, 3);
			}
			if (p.Has(0xf8)) {  // overlay: t13, clamp mode 0
				p.SetTexture(13, p.m.View(0xf8));
				p.SetAddress(13, 0);
			}
			if (p.Has(0x100))  // noise: t15 through FUN_1414e0460
				p.Bind(15, 0x100);
			// LandscapeTexture1to4IsSpecPower = +0x130..+0x13c; 5to6 = (+0x140, +0x144, 0, 0).
			p.PSCopy(kPSLandscape1to4IsSpecPower, 1, 0x134);
			p.PSCopy(kPSLandscape1to4IsSpecPower, 2, 0x138);
			p.PSCopy(kPSLandscape1to4IsSpecPower, 3, 0x13c);
			p.PSCopy(kPSLandscape1to4IsSpecPower, 0, 0x130);
			p.PSCopy(kPSLandscape5to6IsSpecPower, 1, 0x144);
			p.PSCopy(kPSLandscape5to6IsSpecPower, 0, 0x140);
			p.PS(kPSLandscape5to6IsSpecPower, 2, kZeroBits);
			p.PS(kPSLandscape5to6IsSpecPower, 3, kZeroBits);
			if (d & kSnow) {
				// 1to4IsSnow = +0x118..+0x124; 5to6IsSnow = (+0x128, +0x12c, byte 0x142035548, 1 / (float)int 0x1420355f0).
				p.PSCopy(kPSLandscape1to4IsSnow, 1, 0x11c);
				p.PSCopy(kPSLandscape1to4IsSnow, 2, 0x120);
				p.PSCopy(kPSLandscape1to4IsSnow, 3, 0x124);
				p.PSCopy(kPSLandscape1to4IsSnow, 0, 0x118);
				p.PSCopy(kPSLandscape5to6IsSnow, 1, 0x12c);
				p.PS(kPSLandscape5to6IsSnow, 2, a_frame.landSnowZ ? kOneBits : kZeroBits);
				p.PSCopy(kPSLandscape5to6IsSnow, 0, 0x128);
				// cvtdq2ps then divss (1414dc8d9).
				p.PSFloat(kPSLandscape5to6IsSnow, 3, std::bit_cast<float>(kOneBits) / static_cast<float>(a_frame.landSnowDivisor));
			}
			return true;
		}

		/**
		 * @brief The technique's own textures and constants. a_bindBase: t0/t1 at the end (every technique but MTLand). False: the
		 * landscape's layer count is past its arrays.
		 */
		bool EvaluateTechnique(Port& p, std::uint32_t d, const VanillaFrame& a_frame, bool& a_bindBase)
		{
			const std::uint32_t technique = (d >> 24) & 0x3f;
			switch (technique) {
			case 1:  // Envmap: cube, mask; EnvmapData.xy = (envMapScale +0xb0, the mask exists)
				p.BindCube(0xa0);
				p.BindMask(0xa8);
				p.PS(kPSEnvmapData, 1, p.Has(0xa8) ? kOneBits : kZeroBits);
				p.PSCopy(kPSEnvmapData, 0, 0xb0);
				break;
			case 2:  // Glowmap
				p.Bind(6, 0xa0);
				break;
			case 3:  // Parallax
				p.Bind(3, 0xa0);
				break;
			case 4:  // FaceGen: tint t3, detail t4, subsurface t12
				p.Bind(3, 0xa0);
				p.Bind(4, 0xa8);
				p.Bind(12, 0xb0);
				break;
			case 5:  // FaceGenRGBTint
			case 6:  // Hair: TintColor.xyz = +0xa0..+0xa8 (w unwritten)
				p.PSCopy(kPSTintColor, 2, 0xa8);
				p.PSCopy(kPSTintColor, 1, 0xa4);
				p.PSCopy(kPSTintColor, 0, 0xa0);
				break;
			case 7:  // ParallaxOcc: ParallaxOccData.xy = (+0xac scale, +0xa8 max passes)
				p.Bind(3, 0xa0);
				p.PSCopy(kPSParallaxOccData, 1, 0xa8);
				p.PSCopy(kPSParallaxOccData, 0, 0xac);
				break;
			case 8:     // MTLand
			case 0x13:  // MTLandLODBlend
				if (!EvaluateLandscape(p, d, a_frame))
					return false;
				a_bindBase = false;
				break;
			case 9:     // LODLand
			case 0x12:  // LODLandNoise: LODTexParams = (+0xb8, +0xbc, byte 0x142032fda, +0xc0)
				p.PSCopy(kPSLODTexParams, 0, 0xb8);
				p.PSCopy(kPSLODTexParams, 1, 0xbc);
				p.PS(kPSLODTexParams, 2, a_frame.lodTexParamsZ ? kOneBits : kZeroBits);
				p.PSCopy(kPSLODTexParams, 3, 0xc0);
				if ((d & 0x3f000000u) == 0x12000000u) {
					// The noise texture (+0xb0) when there is one; t15's clamp mode 3 and filter mode 1 either way.
					if (p.Has(0xb0))
						p.Bind(15, 0xb0);
					p.SetAddress(15, 3);
					p.SetFilter(15, 1);
				}
				break;
			case 0xb:  // MultilayerParallax: layer t8; MultiLayerParallaxData = +0xb8..+0xc4; cube, mask; EnvmapData.xy = (+0xc8, mask exists)
				p.Bind(8, 0xa0);
				p.PSCopy(kPSMultiLayerParallaxData, 1, 0xbc);
				p.PSCopy(kPSMultiLayerParallaxData, 2, 0xc0);
				p.PSCopy(kPSMultiLayerParallaxData, 3, 0xc4);
				p.PSCopy(kPSMultiLayerParallaxData, 0, 0xb8);
				p.BindCube(0xa8);
				p.BindMask(0xb0);
				p.PS(kPSEnvmapData, 1, p.Has(0xb0) ? kOneBits : kZeroBits);
				p.PSCopy(kPSEnvmapData, 0, 0xc8);
				break;
			case 0xe:  // MultiIndexSparkle: SparkleParams = +0xa0..+0xac
				p.PSCopy(kPSSparkleParams, 2, 0xa8);
				p.PSCopy(kPSSparkleParams, 3, 0xac);
				p.PSCopy(kPSSparkleParams, 1, 0xa4);
				p.PSCopy(kPSSparkleParams, 0, 0xa0);
				break;
			case 0x10:  // Eye: cube, mask; Left/RightEyeCenter.xyz = +0xb4.., +0xc0..; EnvmapData.xy = (+0xb0, mask exists)
				p.BindCube(0xa0);
				p.BindMask(0xa8);
				p.VSCopy(kVSLeftEyeCenter, 1, 0xb8);
				p.VSCopy(kVSLeftEyeCenter, 2, 0xbc);
				p.VSCopy(kVSLeftEyeCenter, 0, 0xb4);
				p.VSCopy(kVSRightEyeCenter, 1, 0xc4);
				p.VSCopy(kVSRightEyeCenter, 2, 0xc8);
				p.VSCopy(kVSRightEyeCenter, 0, 0xc0);
				p.PS(kPSEnvmapData, 1, p.Has(0xa8) ? kOneBits : kZeroBits);
				p.PSCopy(kPSEnvmapData, 0, 0xb0);
				break;
			default:  // 0, TreeAnim, LODObjects, LODObjectHD, Cloud, and anything above 0x13: nothing
				break;
			}
			return true;
		}

		/** @brief After the technique: the texcoord offset, then the feature flags' writes, the base textures and the IBL colour, in SetupMaterial's order. */
		void EvaluateFeatures(Port& p, std::uint32_t d, const VanillaFrame& a_frame, bool a_bindBase)
		{
			// TexcoordOffset: offset (+0xc/+0x10) and scale (+0x1c/+0x20) of the buffer the frame reads, 8 bytes per buffer.
			const std::size_t buffer = static_cast<std::size_t>(a_frame.textureTransformBuffer) * 8;
			p.VSCopy(kVSTexcoordOffset, 0, 0xc + buffer);
			p.VSCopy(kVSTexcoordOffset, 1, 0x10 + buffer);
			p.VSCopy(kVSTexcoordOffset, 2, 0x1c + buffer);
			p.VSCopy(kVSTexcoordOffset, 3, 0x20 + buffer);

			if (d & kSpecular) {
				// SpecularColor = (specularColor +0x38..+0x40 times specularColorScale +0x8c (mulss each), specularPower +0x88).
				const float scale = p.Float(0x8c);
				p.PSFloat(kPSSpecularColor, 0, p.Float(0x38) * scale);
				p.PSFloat(kPSSpecularColor, 1, p.Float(0x3c) * scale);
				p.PSFloat(kPSSpecularColor, 2, p.Float(0x40) * scale);
				p.PSCopy(kPSSpecularColor, 3, 0x88);
				if (d & kModelSpaceNormals) {
					// The specular map in t2 (FUN_1414e0460, then the same inline), filter mode 3.
					p.Bind(2, kSpecularBackLighting);
					p.Bind(2, kSpecularBackLighting);
					p.SetFilter(2, 3);
				}
			}
			if ((d & kAmbientSpecular) && a_frame.ambientSpecularWritten)
				for (std::uint32_t i = 0; i < 4; ++i)
					p.PSFloat(kPSAmbientSpecular, i, a_frame.ambientSpecular[i]);
			// Soft and rim lighting: the rim/soft texture in t12 (inline, textureClampMode), LightingEffectParams.xy = +0x90, +0x94.
			for (const auto flag : { kSoftLighting, kRimLighting }) {
				if (!(d & flag))
					continue;
				p.Bind(12, kRimSoftLighting);
				p.PSCopy(kPSLightingEffectParams, 0, 0x90);
				p.PSCopy(kPSLightingEffectParams, 1, 0x94);
			}
			if (d & kBackLighting)  // the back-lighting texture in t9 (inline)
				p.Bind(9, kSpecularBackLighting);
			if (d & kSnow) {
				for (std::uint32_t i = 0; i < 3; ++i)
					p.PSFloat(kPSSnowRimLightParameters, i, a_frame.snowRim[i]);
				p.PS(kPSSnowRimLightParameters, 3, a_frame.snowRimW ? kOneBits : kZeroBits);
			}
			if (a_bindBase) {
				// t0: the diffuse texture, or the render target the material names (+0x50); t1: the normal map. Both textureClampMode.
				const auto target = static_cast<std::int32_t>(p.Bits(kDiffuseRenderTarget));
				if (target == -1) {
					p.Bind(0, kDiffuse);
				} else {
					p.SetTexture(0, p.RenderTarget(target));
					p.SetAddress(0, p.Bits(kTextureClampMode));
				}
				p.Bind(1, kNormal);
			}
			for (std::uint32_t i = 0; i < 4; ++i)
				p.PSFloat(kPSIBLParams, i, a_frame.ibl[i]);
			if (d & kCharacterLight) {
				// t11: the character light's render target while its index is not negative, clamp mode 0.
				if (a_frame.characterLightTarget >= 0) {
					p.SetTexture(11, p.RenderTarget(a_frame.characterLightTarget));
					p.SetAddress(11, 0);
				}
				for (std::uint32_t i = 0; i < 4; ++i)
					p.PSFloat(kPSCharacterLightParams, i, a_frame.fullBright ? a_frame.characterLight[i] : 0.0f);
			}
		}
	}

	VanillaFrame SampleVanillaFrame()
	{
		EngineReadWindow::Touch("SampleVanillaFrame");
		VanillaFrame frame;
		auto raw = [](std::uintptr_t a_offset) { return reinterpret_cast<const std::byte*>(REL::Offset(a_offset).address()); };
		auto readByte = [&](std::uintptr_t a_offset) { return *reinterpret_cast<const std::uint8_t*>(raw(a_offset)); };
		auto readBytes = [&](void* a_out, std::uintptr_t a_offset, std::size_t a_bytes) { std::memcpy(a_out, raw(a_offset), a_bytes); };

		frame.techniqueByte12 = readByte(kTechniqueByte12);
		frame.techniqueByte7 = readByte(kTechniqueByte7);
		readBytes(&frame.textureTransformBuffer, kTextureTransformBuffer, sizeof(std::uint32_t));
		frame.lodTexParamsZ = readByte(kLodTexParamsZ);
		frame.landSnowZ = readByte(kLandSnowZ);
		readBytes(&frame.landSnowDivisor, kLandSnowDivisor, sizeof(std::int32_t));
		readBytes(frame.ambientSpecular.data(), kAmbientSpecularGlobal, sizeof(float) * 4);
		static const bool ambientSpecularRemoved = [] {
			// The site as the fix computes it (its ID and offset: +0x8cf in AE, +0x713 in SE, the same 0x20-byte AmbientSpecular write).
			const auto* code = reinterpret_cast<const std::uint8_t*>(REL::RelocationID(100563, 107298).address() + (REL::Module::IsSE() ? 0x713 : 0x8cf));
			return std::all_of(code, code + 0x20, [](std::uint8_t a_byte) { return a_byte == 0x90; });
		}();
		frame.ambientSpecularWritten = !ambientSpecularRemoved;
		readBytes(&frame.snowRim[0], kSnowRim0, sizeof(float));
		readBytes(&frame.snowRim[1], kSnowRim1, sizeof(float));
		readBytes(&frame.snowRim[2], kSnowRim2, sizeof(float));
		frame.snowRimW = readByte(kSnowRimW);
		readBytes(frame.characterLight.data(), kCharacterLightGlobal, sizeof(float) * 4);
		frame.fullBright = readByte(kFullBright);
		readBytes(&frame.characterLightTarget, kCharacterLightTarget, sizeof(std::int32_t));

		// The shader object's IBL colour: x at +0xcc, then +0xd0.. or +0xe0.. by the byte at +0xf0 (SetupMaterial reads 16 bytes and
		// writes the first three).
		if (const auto* shader = reinterpret_cast<const std::byte*>(EngineShaders::Lighting())) {
			frame.shaderKnown = true;
			std::memcpy(&frame.ibl[0], shader + 0xcc, sizeof(float));
			std::memcpy(&frame.ibl[1], shader + (*reinterpret_cast<const std::uint8_t*>(shader + 0xf0) ? 0xd0 : 0xe0), sizeof(float) * 3);
		}

		frame.envMaskDefaultView = ViewOf(*reinterpret_cast<const void* const*>(raw(kEnvMaskDefault)));
		// The renderer's render targets: SetupMaterial reads the SRV at 0x143289230 + 0x30 * index (1414dcdc9, 1414dcee8).
		for (std::uint32_t i = 0, count = VanillaRenderTargets(); i < count; ++i)
			frame.renderTargetViews[i] = *reinterpret_cast<ID3D11ShaderResourceView* const*>(raw(kRenderTargetViews + 0x30 * i));
		frame.sampled = true;
		return frame;
	}

	ID3D11ShaderResourceView* ViewOf(const void* a_texture)
	{
		// NiSourceTexture +0x48 (BSGraphics::Texture*, the renderer texture), then +0x10 (its SRV): FUN_1414e0460 and the inline binds.
		if (!a_texture)
			return nullptr;
		const auto* rendererTexture = *reinterpret_cast<const std::byte* const*>(static_cast<const std::byte*>(a_texture) + 0x48);
		return rendererTexture ? *reinterpret_cast<ID3D11ShaderResourceView* const*>(rendererTexture + 0x10) : nullptr;
	}

	void VanillaTextureFields(const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count)
	{
		a_count = 0;
		const auto* c = ClassOf(*reinterpret_cast<const void* const*>(&a_material));
		if (!c)
			return;
		for (std::uint32_t i = 0; i < c->textureCount && i < kMaxTextureFields; ++i)
			a_out[a_count++] = c->textures[i];
	}

	std::uint32_t VanillaClassSize(const RE::BSShaderMaterial& a_material)
	{
		const auto* c = ClassOf(*reinterpret_cast<const void* const*>(&a_material));
		return c ? c->size : 0;
	}

	bool EvaluateVanilla(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const VanillaFrame& a_frame, MaterialRecord& a_out)
	{
		if (!a_frame.sampled || !a_frame.shaderKnown)
			return false;
		// A contributor's class reaches here when its Replace hands the pass back (TruePBR's CHECK_PBR_TEXTURE): vanilla code on a
		// Base-derived object, its texture fields the base's, its size its own.
		const bool asBase = Extensions::MaterialClassVanillaAsBase(a_snapshot.materialClass);
		VanillaClass pbrBase;
		if (asBase) {
			pbrBase.size = a_snapshot.size;
			for (const auto offset : { kDiffuse, kNormal, kRimSoftLighting, kSpecularBackLighting })
				pbrBase.textures[pbrBase.textureCount++] = offset;
		}
		const auto* c = asBase ? &pbrBase : ClassOf(a_snapshot.vtable);
		if (!c || a_snapshot.size < c->size)
			return false;

		ResetRecord(a_out);

		const std::uint32_t d = RemapDescriptor(a_passDescriptor, a_frame);
		Port p(a_snapshot, *c, a_frame, a_out);
		bool bindBase = true;  // t0/t1 at the end: every technique but MTLand
		if (!EvaluateTechnique(p, d, a_frame, bindBase))
			return false;
		EvaluateFeatures(p, d, a_frame, bindBase);

		if (p.failed)
			return false;
		// A record holds an address mode only with a written texture.
		for (std::uint32_t slot = 0; slot < kPixelTextureSlots; ++slot)
			if (!((a_out.textureWritten >> slot) & 1))
				a_out.addressModes[slot] = 0;
		return true;
	}
}
