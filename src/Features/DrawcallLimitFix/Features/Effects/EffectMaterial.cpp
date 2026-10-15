// The Effect port's material part: BSEffectShader::SetupMaterial (the non-membrane half) on a snapshot, pure, in the port's space
// (EffectVSLayout, EffectPSLayout). And the port's registration (Api::MaterialPortContribution): the property's capture and shading
// (EffectProperty.cpp), its descriptors and variants (EffectDescriptors.cpp) and this evaluation.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectDescriptors.h"

#include <bit>
#include <cstring>

namespace DCLF::Features::Effects
{
	namespace
	{
		// BSEffectShaderMaterial's size (its Create's allocation, CommonLib's sizeof).
		constexpr std::uint32_t kEffectMaterialBytes = 0x88;
		// BSEffectShaderMaterial (0x88 bytes, its Create 0x1414e9c80): the fields SetupMaterial (0x1414ea6c0) reads.
		constexpr std::uint16_t kFalloff = 0x38;  // falloffStartAngle, falloffStopAngle, falloffStartOpacity, falloffStopOpacity
		constexpr std::uint16_t kBaseColor = 0x48;
		constexpr std::uint16_t kSourceTexture = 0x58;
		constexpr std::uint16_t kGreyscaleTexture = 0x60;
		constexpr std::uint16_t kSoftFalloffDepth = 0x68;
		constexpr std::uint16_t kBaseColorScale = 0x6c;
		constexpr std::uint16_t kClampMode = 0x80;
		constexpr std::uint16_t kLightingInfluence = 0x81;
		// The texture-transform buffers' first offset and scale (BSShaderMaterial +0x0c/+0x10, +0x1c/+0x20; 8 bytes a buffer), as Lighting's.
		constexpr std::uint16_t kTexcoordOffset = 0x0c;
		constexpr std::uint16_t kTexcoordScale = 0x1c;
		// 1/255 as the engine holds it (0x141ad2810).
		constexpr std::uint32_t kInverse255Bits = 0x3b808081u;
		// BSGraphics::TextureFilterMode: SetupTechnique gives t0 anisotropic filtering with Texture, SetupMaterial t4 bilinear.
		constexpr std::uint32_t kFilterBilinear = 1;
		constexpr std::uint32_t kFilterAnisotropic = 3;
		// The texture fields a snapshot resolves: the source and the greyscale palette.
		constexpr std::uint16_t kEffectTextureFields[] = { kSourceTexture, kGreyscaleTexture };

		/** @brief Where each Effect variable lives in an effect MaterialRecord's blocks (the port's space: MakeStageLayout of its rows). */
		const StageLayout& EffectVSLayout()
		{
			static const StageLayout layout = MakeStageLayout(kEffectVSVariables, kEffectVSSized, kEffectVSScratch);
			return layout;
		}

		const StageLayout& EffectPSLayout()
		{
			static const StageLayout layout = MakeStageLayout(kEffectPSVariables, {}, {});
			return layout;
		}

		/**
		 * @brief BSEffectShader::SetupMaterial (0x1414ea6c0, the non-membrane half) and SetupTechnique's filter modes for an effect material's
		 * snapshot, into a record of kind kMaterialKindEffect (EffectVSLayout / EffectPSLayout order). LightingInfluence.y holds
		 * softFalloffDepth itself (the pixel stage divides the frame's near * far by it). False: a membrane pass, a greyscale pass without
		 * its palette, or a snapshot that is not an effect material's. The vanilla frame's sources alone (the snapshot's and the record's
		 * through the evaluation's calls: Core/Extensions/MaterialPorts.h).
		 */
		bool EvaluateEffect(Api::MaterialPortEvaluation& a_port)
		{
			const std::uint32_t pass = a_port.passDescriptor;
			if (a_port.Port() != kMaterialKindEffect || a_port.Size() < kEffectMaterialBytes || !a_port.FrameSampled())
				return false;
			// The membrane half (the TESEffectShader membranes: normal and noise textures, no constants) is not ported.
			if (pass & EffectFlag::Membrane)
				return false;
			a_port.ResetRecord(kMaterialKindEffect);

			const auto& vs = EffectVSLayout();
			const auto& ps = EffectPSLayout();
			float* const vsFloats = a_port.VSFloats();
			float* const psFloats = a_port.PSFloats();
			const auto bits = [&](std::size_t a_offset) { return a_port.At<std::uint32_t>(a_offset); };
			const auto setPS = [&](std::uint32_t a_variable, std::uint32_t a_component, float a_value) {
				psFloats[ps.offset[a_variable] + a_component] = a_value;
			};
			const auto copyVS = [&](std::uint32_t a_variable, std::uint32_t a_component, std::size_t a_offset) {
				const std::uint32_t value = bits(a_offset);
				std::memcpy(&vsFloats[vs.offset[a_variable] + a_component], &value, sizeof(value));
			};
			const auto copyPS = [&](std::uint32_t a_variable, std::uint32_t a_component, std::size_t a_offset) {
				const std::uint32_t value = bits(a_offset);
				std::memcpy(&psFloats[ps.offset[a_variable] + a_component], &value, sizeof(value));
			};

			if (pass & EffectFlag::Texture) {
				// SetupTechnique: t0 anisotropic. SetupMaterial: the source texture with the material's clamp mode, when there is one (the
				// engine binds its renderer texture's view, null without one), and TexcoordOffset from the buffer the frame reads.
				a_port.SetFilterMode(0, kFilterAnisotropic);
				if (a_port.HasTexture(kSourceTexture))
					a_port.BindTexture(0, a_port.View(kSourceTexture), a_port.At<std::uint8_t>(kClampMode));
				const std::size_t buffer = std::size_t(a_port.TextureTransformBuffer() & 1u) * 8;
				copyVS(kEffectVSTexcoordOffset, 0, kTexcoordOffset + buffer);
				copyVS(kEffectVSTexcoordOffset, 1, kTexcoordOffset + 4 + buffer);
				copyVS(kEffectVSTexcoordOffset, 2, kTexcoordScale + buffer);
				copyVS(kEffectVSTexcoordOffset, 3, kTexcoordScale + 4 + buffer);
			}
			if (pass & (EffectFlag::GrayscaleToColor | EffectFlag::GrayscaleToAlpha)) {
				// The greyscale palette: dereferenced without a test by the engine (a null one would crash its draw), so not one DCLF draws.
				if (!a_port.HasTexture(kGreyscaleTexture))
					return false;
				a_port.BindTexture(4, a_port.View(kGreyscaleTexture), 0);
				a_port.SetFilterMode(4, kFilterBilinear);
			}

			// BaseColor: the base colour times its scale, or with GrayscaleToColor the raw colour and the scale apart (the palette's).
			const float scale = a_port.At<float>(kBaseColorScale);
			if (!(pass & EffectFlag::GrayscaleToColor)) {
				setPS(kEffectPSBaseColor, 0, scale * a_port.At<float>(kBaseColor));
				setPS(kEffectPSBaseColor, 1, a_port.At<float>(kBaseColor + 4) * scale);
				setPS(kEffectPSBaseColor, 2, a_port.At<float>(kBaseColor + 8) * scale);
				copyPS(kEffectPSBaseColor, 3, kBaseColor + 12);
			} else {
				for (std::uint32_t c = 0; c < 4; ++c)
					copyPS(kEffectPSBaseColor, c, kBaseColor + 4 * c);
				copyPS(kEffectPSBaseColorScale, 0, kBaseColorScale);
			}
			setPS(kEffectPSLightingInfluence, 0, static_cast<float>(a_port.At<std::uint8_t>(kLightingInfluence)) * std::bit_cast<float>(kInverse255Bits));
			if (pass & EffectFlag::Soft) {
				// The engine writes near * far / softFalloffDepth, the camera's planes as SetupTechnique left them. DCLF keeps the depth itself
				// here and the pixel stage divides the frame's product by it (Effect.hlsl, DCLF_BINDLESS: the same product and quotient), so
				// the record does not change with the camera.
				copyPS(kEffectPSLightingInfluence, 1, kSoftFalloffDepth);
				copyVS(kEffectVSSoftMaterialParams, 0, kSoftFalloffDepth);
			}
			if (pass & EffectFlag::Falloff)
				for (std::uint32_t c = 0; c < 4; ++c)
					copyVS(kEffectVSFalloffData, c, kFalloff + 4 * c);
			return true;
		}

		// The port's callbacks (Api::MaterialPortContribution).
		void DeriveEffect(Api::MaterialPortDerivation& a_derivation)
		{
			a_derivation.reason = DeriveEffectDescriptors(a_derivation.property, a_derivation.leaf, *a_derivation.out);
		}

		std::uint32_t EffectVariants(const Api::MaterialPortDerivation& a_derivation)
		{
			return EffectVariantsOf(a_derivation.property, a_derivation.leaf);
		}

		bool EvaluateEffectPort(Api::MaterialPortEvaluation& a_evaluation)
		{
			// No Community Shaders hook on BSEffectShader's SetupMaterial: the vanilla frame's sources alone.
			return EvaluateEffect(a_evaluation);
		}
	}

	void RegisterPort(Api::InstallContext& a_context)
	{
		// The first port registered (no feature before this one registers one), so kind 1: the kind its records carry (kMaterialKindEffect).
		static_assert(kMaterialKindEffect == 1);
		static const Api::MaterialPortContribution port{
			.name = "effect",
			.shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::Effect),
			.objectClass = Api::ObjectClass::Effect,
			.propertyRtti = []() -> const void* { return REL::Relocation<const RE::NiRTTI*>{ RE::BSEffectShaderProperty::Ni_RTTI }.get(); },
			.materialVtable = []() -> const void* { return reinterpret_cast<const void*>(RE::VTABLE_BSEffectShaderMaterial[0].address()); },
			.materialBytes = kEffectMaterialBytes,
			.textureFields = kEffectTextureFields,
			.vs = { .variables = kEffectVSVariables, .sized = kEffectVSSized, .scratch = kEffectVSScratch,
				.groups = { kEffectVSGroups[0], kEffectVSGroups[1], kEffectVSGroups[2] },
				.first = { kEffectVSFirstVariable[0], kEffectVSFirstVariable[1], kEffectVSFirstVariable[2] }, .names = kEffectVSNames },
			.ps = { .variables = kEffectPSVariables, .groups = { kEffectPSGroups[0], kEffectPSGroups[1], kEffectPSGroups[2] },
				.first = { kEffectPSFirstVariable[0], kEffectPSFirstVariable[1], kEffectPSFirstVariable[2] }, .names = kEffectPSNames },
			// Pass enum + 0x4000002C; the DEFERRED pixel bit; no ProjectedUV binding (a variant); TexcoordOffset (VS 9) from the frame; no
			// Lighting technique row; BSShader_BeginTechnique's raw permutation; no signature component; the emittance colour listed
			// whatever the flags; the billboards' turned shapes and the particle systems are its leaves.
			.rules = { .passEnumBase = kEffectPassEnumBase, .deferredPixelBit = EffectFlag::Deferred, .projectedPassBit = 0, .frameVSVariable = kEffectVSTexcoordOffset,
				.techniqueRows = false, .rawPermutation = true, .signatureComponents = false, .emissiveAlwaysListed = true, .turnedByBillboards = true,
				.particleGeometry = true },
			.capture = &CaptureEffectProperty,
			.derive = &DeriveEffect,
			.evaluate = &EvaluateEffectPort,
			.shading = &SampleEffectShading,
			.variants = &EffectVariants,
			.variantNames = kEffectVariantNames,
			.emissiveOf = &EffectEmissive,
		};
		if (a_context.RegisterMaterialPort(port) != kMaterialKindEffect)
			stl::report_and_fail("Drawcall Limit Fix: the Effect port did not take material kind 1 (a feature before Effects registered a port)");
	}
}
