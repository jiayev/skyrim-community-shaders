// MaterialPort's Effect part: BSEffectShader::SetupMaterial (the non-membrane half) on a snapshot. Pure. And the Effect port's material port
// contribution, Core's built-in until Features/Effects registers it (Extensions::CoreMaterialPorts): the property's capture, its descriptors
// and variants (LightingDescriptors), this evaluation and its shading row (LightingConstants' SampleEffectShading).

#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPort.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"

#include "Features/DrawcallLimitFix/Scene/Ports/EffectConstants.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

#include <bit>
#include <cstring>

namespace DCLF::MaterialPort
{
	namespace
	{
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

		// The port's callbacks (Api::MaterialPortContribution).
		void CaptureEffectProperty(const Api::MaterialPortCapture& a_capture)
		{
			// What the effect's descriptor (FUN_1414bde90) and PropertyColor read of the property and its material, taken here where the
			// property holds the material. GetRenderPasses tests a texture path by its string's pointer (FUN_140cec900 against null), not by
			// the texture.
			a_capture.emissive = static_cast<const RE::BSEffectShaderProperty&>(a_capture.property).emittanceColor;
			if (const auto* material = a_capture.property.material) {
				const auto* bytes = reinterpret_cast<const std::byte*>(material);
				const auto pathAt = [&](std::size_t a_offset) { return *reinterpret_cast<const char* const*>(bytes + a_offset) != nullptr; };
				a_capture.feature = (pathAt(0x70) ? kEffectHasSourcePath : 0u) | (pathAt(0x78) ? kEffectHasGreyscalePath : 0u);
			}
		}

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
			return EvaluateEffect(a_evaluation.snapshot, a_evaluation.passDescriptor, a_evaluation.frame.vanilla, a_evaluation.out);
		}

		void SampleEffectPortShading(Api::MaterialPortShading& a_shading)
		{
			SampleEffectShading(a_shading.property, a_shading.member, a_shading.out);
		}
	}

	bool EvaluateEffect(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const VanillaFrame& a_frame, MaterialRecord& a_out)
	{
		if (a_snapshot.port != kMaterialKindEffect || a_snapshot.size < kEffectMaterialBytes || !a_frame.sampled)
			return false;
		// The membrane half (the TESEffectShader membranes: normal and noise textures, no constants) is not ported.
		if (a_passDescriptor & EffectFlag::Membrane)
			return false;
		a_out.vs.Reset();
		a_out.ps.Reset();
		a_out.textures.fill(nullptr);
		a_out.addressModes.fill(0);
		a_out.filterModes.fill(kUnwrittenFilterMode);
		a_out.textureWritten = 0;
		a_out.featureTextures.fill(nullptr);
		a_out.kind = kMaterialKindEffect;

		const auto& vs = EffectVSLayout();
		const auto& ps = EffectPSLayout();
		const auto bits = [&](std::size_t a_offset) { return a_snapshot.At<std::uint32_t>(a_offset); };
		const auto setPS = [&](std::uint32_t a_variable, std::uint32_t a_component, float a_value) {
			a_out.ps.floats[ps.offset[a_variable] + a_component] = a_value;
		};
		const auto copyVS = [&](std::uint32_t a_variable, std::uint32_t a_component, std::size_t a_offset) {
			const std::uint32_t value = bits(a_offset);
			std::memcpy(&a_out.vs.floats[vs.offset[a_variable] + a_component], &value, sizeof(value));
		};
		const auto copyPS = [&](std::uint32_t a_variable, std::uint32_t a_component, std::size_t a_offset) {
			const std::uint32_t value = bits(a_offset);
			std::memcpy(&a_out.ps.floats[ps.offset[a_variable] + a_component], &value, sizeof(value));
		};

		if (a_passDescriptor & EffectFlag::Texture) {
			// SetupTechnique: t0 anisotropic. SetupMaterial: the source texture with the material's clamp mode, when there is one (the
			// engine binds its renderer texture's view, null without one), and TexcoordOffset from the buffer the frame reads.
			a_out.filterModes[0] = kFilterAnisotropic;
			if (a_snapshot.HasTexture(kSourceTexture)) {
				a_out.textures[0] = a_snapshot.View(kSourceTexture);
				a_out.addressModes[0] = a_snapshot.At<std::uint8_t>(kClampMode);
				a_out.textureWritten |= 1u << 0;
			}
			const std::size_t buffer = std::size_t(a_frame.textureTransformBuffer & 1u) * 8;
			copyVS(kEffectVSTexcoordOffset, 0, kTexcoordOffset + buffer);
			copyVS(kEffectVSTexcoordOffset, 1, kTexcoordOffset + 4 + buffer);
			copyVS(kEffectVSTexcoordOffset, 2, kTexcoordScale + buffer);
			copyVS(kEffectVSTexcoordOffset, 3, kTexcoordScale + 4 + buffer);
		}
		if (a_passDescriptor & (EffectFlag::GrayscaleToColor | EffectFlag::GrayscaleToAlpha)) {
			// The greyscale palette: dereferenced without a test by the engine (a null one would crash its draw), so not one DCLF draws.
			if (!a_snapshot.HasTexture(kGreyscaleTexture))
				return false;
			a_out.textures[4] = a_snapshot.View(kGreyscaleTexture);
			a_out.addressModes[4] = 0;
			a_out.filterModes[4] = kFilterBilinear;
			a_out.textureWritten |= 1u << 4;
		}

		// BaseColor: the base colour times its scale, or with GrayscaleToColor the raw colour and the scale apart (the palette's).
		const float scale = a_snapshot.At<float>(kBaseColorScale);
		if (!(a_passDescriptor & EffectFlag::GrayscaleToColor)) {
			setPS(kEffectPSBaseColor, 0, scale * a_snapshot.At<float>(kBaseColor));
			setPS(kEffectPSBaseColor, 1, a_snapshot.At<float>(kBaseColor + 4) * scale);
			setPS(kEffectPSBaseColor, 2, a_snapshot.At<float>(kBaseColor + 8) * scale);
			copyPS(kEffectPSBaseColor, 3, kBaseColor + 12);
		} else {
			for (std::uint32_t c = 0; c < 4; ++c)
				copyPS(kEffectPSBaseColor, c, kBaseColor + 4 * c);
			copyPS(kEffectPSBaseColorScale, 0, kBaseColorScale);
		}
		setPS(kEffectPSLightingInfluence, 0, static_cast<float>(a_snapshot.At<std::uint8_t>(kLightingInfluence)) * std::bit_cast<float>(kInverse255Bits));
		if (a_passDescriptor & EffectFlag::Soft) {
			// The engine writes near * far / softFalloffDepth, the camera's planes as SetupTechnique left them. DCLF keeps the depth itself
			// here and the pixel stage divides the frame's product by it (Effect.hlsl, DCLF_BINDLESS: the same product and quotient), so
			// the record does not change with the camera.
			copyPS(kEffectPSLightingInfluence, 1, kSoftFalloffDepth);
			copyVS(kEffectVSSoftMaterialParams, 0, kSoftFalloffDepth);
		}
		if (a_passDescriptor & EffectFlag::Falloff)
			for (std::uint32_t c = 0; c < 4; ++c)
				copyVS(kEffectVSFalloffData, c, kFalloff + 4 * c);
		return true;
	}
}

namespace DCLF::Extensions
{
	std::span<const Api::MaterialPortContribution> CoreMaterialPorts()
	{
		// The first of Core's built-in ports, so kind 1: the kind its records carry (kMaterialKindEffect; checked at install).
		static_assert(kMaterialKindEffect == 1);
		static const Api::MaterialPortContribution ports[] = { {
			.shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::Effect),
			.objectClass = Api::ObjectClass::Effect,
			.propertyRtti = []() -> const void* { return REL::Relocation<const RE::NiRTTI*>{ RE::BSEffectShaderProperty::Ni_RTTI }.get(); },
			.materialVtable = []() -> const void* { return reinterpret_cast<const void*>(RE::VTABLE_BSEffectShaderMaterial[0].address()); },
			.materialBytes = MaterialPort::kEffectMaterialBytes,
			.textureFields = MaterialPort::kEffectTextureFields,
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
			.capture = &MaterialPort::CaptureEffectProperty,
			.derive = &MaterialPort::DeriveEffect,
			.evaluate = &MaterialPort::EvaluateEffectPort,
			.shading = &MaterialPort::SampleEffectPortShading,
			.variants = &MaterialPort::EffectVariants,
			.variantNames = kEffectVariantNames,
		} };
		return ports;
	}
}
