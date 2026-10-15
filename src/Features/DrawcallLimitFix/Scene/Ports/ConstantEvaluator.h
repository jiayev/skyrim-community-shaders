#pragma once

// The constant records DCLF evaluates in place of the engine's setup functions: ConstantBlock and the Lighting / Effect variable
// layouts, MaterialRecord (SetupMaterial), TechniqueConstants (SetupTechnique, a pure function of TechniqueInputs the render thread
// samples at the frame's start) and GeometryConstants (SetupGeometry).

#include <array>
#include <cstdint>
#include <cstring>

#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EffectConstants.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"

struct ID3D11ShaderResourceView;

namespace DCLF
{
	/** @brief Lighting shader constant variable counts (ShaderCache GetVariableIndices / ShaderConstants). */
	inline constexpr std::uint32_t kLightingVSVariables = 17;
	inline constexpr std::uint32_t kLightingPSVariables = 51;
	inline constexpr std::uint32_t kPixelTextureSlots = 16;

	/**
	 * @brief Per-material textures a Community Shaders hook on SetupMaterial binds outside the engine's slots:
	 * Advanced Skin's t71 (RFAOS) and t74 (wetness), Skin::MaterialTexturesOf; TruePBR's landscape displacement
	 * (t80-t85) and RMAOS (t86-t91) layers, its extendedRendererState. MaterialRecord::featureTextures holds them
	 * in this order.
	 */
	inline constexpr std::array<std::uint32_t, 14> kFeatureMaterialRegisters{ 71, 74, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91 };
	inline constexpr std::uint32_t kFeatureMaterialTextures = static_cast<std::uint32_t>(kFeatureMaterialRegisters.size());
	/** @brief Advanced Skin's entries of featureTextures (t71, t74): the first two. */
	inline constexpr std::uint32_t kSkinFeatureTextures = 2;
	/** @brief TruePBR's: its 12 slots (t80-t91) from this entry on, TruePBR::FirstExtendedTexture's first. */
	inline constexpr std::uint32_t kTruePBRFeatureTextures = 2;
	/** @brief The featureTextures entry of a pixel texture register, or -1. */
	inline constexpr int FeatureMaterialSlot(std::uint32_t a_register)
	{
		for (std::uint32_t i = 0; i < kFeatureMaterialTextures; ++i)
			if (kFeatureMaterialRegisters[i] == a_register)
				return static_cast<int>(i);
		return -1;
	}
	static_assert(FeatureMaterialSlot(71) == 0 && FeatureMaterialSlot(74) == 1 && FeatureMaterialSlot(80) == static_cast<int>(kTruePBRFeatureTextures) &&
				  FeatureMaterialSlot(91) == static_cast<int>(kFeatureMaterialTextures) - 1);
	static_assert(kFeatureMaterialTextures <= 32);  // a bit each in the build's row state (MainRows::MaterialState::features)

	/** @brief Bit pattern of constant components the engine did not write (a quiet NaN no engine code produces; a signaling NaN would be quietened by float copies). */
	inline constexpr std::uint32_t kUnwrittenBits = 0x7fdadbadu;  // quiet NaN: survives float copies unchanged

	/** @brief Filter mode value for a sampler slot nothing set (the draw inherits whatever the previous draw left). */
	inline constexpr std::uint32_t kUnwrittenFilterMode = 0xffffffffu;

	/** @brief Floats in a constant block: every offset a constant-table byte can express, plus a float4. */
	inline constexpr std::uint32_t kConstantBlockFloats = 256 + 4;

	/**
	 * @brief Where each Lighting variable lives in a ConstantBlock (StageLayout, Core/Extensions/MaterialPorts.h): the per-geometry point
	 * light arrays share the scratch region. Kind 0's space (Extensions::MaterialSpaceOf), built from the same rows (CoreLightingPort).
	 */
	const StageLayout& LightingVSLayout();
	const StageLayout& LightingPSLayout();
	/** @brief The same for the Effect shader's variables (EffectConstants.h): an effect MaterialRecord's blocks (the Effect port's space). */
	const StageLayout& EffectVSLayout();
	const StageLayout& EffectPSLayout();

	/** @brief One constant group's values in StageLayout order; unwritten components hold kUnwrittenBits. */
	struct ConstantBlock
	{
		alignas(16) std::array<float, kConstantBlockFloats> floats;

		void Reset();
		bool Written(std::uint32_t a_float) const { return std::bit_cast<std::uint32_t>(floats[a_float]) != kUnwrittenBits; }
		/** @brief Bit equality: kUnwrittenBits is a NaN, which a float comparison never finds equal to itself. */
		bool SameBits(const ConstantBlock& a_other) const { return std::memcmp(floats.data(), a_other.floats.data(), sizeof(floats)) == 0; }
		bool SameBits(const ConstantBlock& a_other, std::uint32_t a_float) const
		{
			return std::bit_cast<std::uint32_t>(floats[a_float]) == std::bit_cast<std::uint32_t>(a_other.floats[a_float]);
		}
	};

	/** @brief What BSLightingShader::SetupMaterial binds for one (material, pass descriptor) pair. */
	struct MaterialRecord
	{
		ConstantBlock vs;  // PerMaterial group
		ConstantBlock ps;
		std::array<ID3D11ShaderResourceView*, kPixelTextureSlots> textures{};
		std::array<std::uint32_t, kPixelTextureSlots> addressModes{};
		std::array<std::uint32_t, kPixelTextureSlots> filterModes{};  // kUnwrittenFilterMode where SetupMaterial leaves it
		std::uint32_t textureWritten = 0;
		// kFeatureMaterialRegisters: null where the feature binds nothing for the material (it leaves the previous
		// draw's binding; the draws then take the frame's).
		std::array<ID3D11ShaderResourceView*, kFeatureMaterialTextures> featureTextures{};
		// Which shader's SetupMaterial made it (EffectConstants.h, kMaterialKind*): the variable space of vs and ps.
		std::uint8_t kind = kMaterialKindLighting;

		/**
		 * @brief Value equality. Member-wise, NOT memcmp over the object: the struct is 16-aligned because of
		 * ConstantBlock, and its 12 bytes of trailing padding are never written, so memcmp would compare garbage.
		 */
		bool operator==(const MaterialRecord& a_other) const
		{
			return vs.SameBits(a_other.vs) && ps.SameBits(a_other.ps) &&
			       textures == a_other.textures && addressModes == a_other.addressModes &&
			       filterModes == a_other.filterModes && textureWritten == a_other.textureWritten && featureTextures == a_other.featureTextures &&
			       kind == a_other.kind;
		}
	};

	/**
	 * @brief What BSLightingShader::SetupTechnique writes for one pass descriptor: the PerTechnique
	 * groups (fog, colour output clamp) and the sampler filter modes. Ported (engine notes:
	 * SetupTechnique).
	 */
	struct TechniqueConstants
	{
		ConstantBlock vs;
		ConstantBlock ps;
		std::array<std::uint32_t, kPixelTextureSlots> filterModes{};  // kUnwrittenFilterMode where SetupTechnique leaves it
		bool shadowMask = false;  // the technique binds the shadow mask to t14 (clamp) and sets VPOSOffset
		ID3D11ShaderResourceView* shadowMaskTexture = nullptr;
	};

	/** @brief Texture slot SetupTechnique binds the shadow mask to. */
	inline constexpr std::uint32_t kShadowMaskSlot = 14;

	/**
	 * @brief Terrain LOD's HighDetailRange (VS PerTechnique) as DCLF draws it: the loaded grid's centre, absolute (the engine's less
	 * posAdjust; DCLF's vertex shader takes the draw's eye off it), and its half extents less 15 (dclf-lod.md, "Terrain LOD").
	 */
	void LodHighDetailRange(float* a_out);

	/** @brief Render thread (the frame's start, FrameGlobals::Capture; the parity): what SetupTechnique reads of the engine now. */
	TechniqueInputs SampleTechniqueInputs();
	/** @brief Pure: SetupTechnique's writes for a pass descriptor from sampled inputs. */
	void EvaluateTechnique(std::uint32_t a_passDescriptor, const TechniqueInputs& a_inputs, TechniqueConstants& a_out);
	/** @brief EvaluateTechnique from the frame's sample (FrameGlobals::Current().technique). */
	inline void EvaluateTechnique(std::uint32_t a_passDescriptor, TechniqueConstants& a_out)
	{
		EvaluateTechnique(a_passDescriptor, FrameGlobals::Current().technique, a_out);
	}

	/**
	 * @brief What EvaluateTechnique reads of a pass descriptor (the technique, and whether the shadow mask is bound):
	 * descriptors with the same key evaluate the same within a frame.
	 */
	inline std::uint32_t TechniqueKey(std::uint32_t a_passDescriptor)
	{
		const bool shadowLights = (a_passDescriptor & (1u << 13)) || (a_passDescriptor & 0x1c0u);
		const bool shadowMask = shadowLights && (a_passDescriptor & (1u << 14));
		return (((a_passDescriptor >> 24) & 0x3f) << 1) | (shadowMask ? 1u : 0u);
	}

	/** @brief What BSLightingShader::SetupGeometry writes into the PerGeometry groups for one pass. */
	struct GeometryConstants
	{
		ConstantBlock vs;
		ConstantBlock ps;
	};
}
