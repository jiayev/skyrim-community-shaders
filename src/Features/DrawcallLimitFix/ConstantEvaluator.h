#pragma once

#include <array>
#include <cstdint>
#include <cstring>

struct ID3D11ShaderResourceView;

namespace DCLF
{
	/** @brief Lighting shader constant variable counts (ShaderCache GetVariableIndices / ShaderConstants). */
	inline constexpr std::uint32_t kLightingVSVariables = 17;
	inline constexpr std::uint32_t kLightingPSVariables = 51;
	inline constexpr std::uint32_t kPixelTextureSlots = 16;

	/**
	 * @brief Per-material textures a Community Shaders hook on SetupMaterial binds outside the engine's slots:
	 * Advanced Skin's t71 (RFAOS) and t74 (wetness), Skin::MaterialTexturesOf. MaterialRecord::featureTextures
	 * holds them in this order.
	 */
	inline constexpr std::array<std::uint32_t, 2> kFeatureMaterialRegisters{ 71, 74 };
	inline constexpr std::uint32_t kFeatureMaterialTextures = static_cast<std::uint32_t>(kFeatureMaterialRegisters.size());
	/** @brief The featureTextures entry of a pixel texture register, or -1. */
	inline constexpr int FeatureMaterialSlot(std::uint32_t a_register)
	{
		for (std::uint32_t i = 0; i < kFeatureMaterialTextures; ++i)
			if (kFeatureMaterialRegisters[i] == a_register)
				return static_cast<int>(i);
		return -1;
	}

	/** @brief Bit pattern of constant components the engine did not write (a quiet NaN no engine code produces; a signaling NaN would be quietened by float copies). */
	inline constexpr std::uint32_t kUnwrittenBits = 0x7fdadbadu;  // quiet NaN: survives float copies unchanged

	/** @brief Filter mode value for a sampler slot nothing set (the draw inherits whatever the previous draw left). */
	inline constexpr std::uint32_t kUnwrittenFilterMode = 0xffffffffu;

	/** @brief Floats in a constant block: every offset a constant-table byte can express, plus a float4. */
	inline constexpr std::uint32_t kConstantBlockFloats = 256 + 4;

	/**
	 * @brief Where each Lighting variable lives in a ConstantBlock (in floats) and how many floats it has.
	 * Matrices take 12, the per-geometry point light arrays share a scratch region, the rest 4.
	 */
	struct StageLayout
	{
		std::uint32_t count;
		std::array<std::uint8_t, 64> offset;
		std::array<std::uint8_t, 64> size;
	};
	const StageLayout& LightingVSLayout();
	const StageLayout& LightingPSLayout();

	/** @brief One constant group's values in StageLayout order; unwritten components hold kUnwrittenBits. */
	struct ConstantBlock
	{
		alignas(16) std::array<float, kConstantBlockFloats> floats;

		void Reset();
		bool Written(std::uint32_t a_float) const { return std::bit_cast<std::uint32_t>(floats[a_float]) != kUnwrittenBits; }
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

		/**
		 * @brief Value equality, for measuring whether a record could be cached across frames.
		 *
		 * Member-wise, NOT memcmp over the object. The members total 2340 bytes while the struct is
		 * 16-aligned because of ConstantBlock, so there are 12 bytes of trailing padding that nothing
		 * ever writes. memcmp compared that padding: a record built on the stack carried whatever
		 * garbage was there, the copy held in the probe map carried different garbage, and every single
		 * comparison came out unequal. That is what "104 evaluated, 0 unchanged, 104 changed" was
		 * reporting - the instrument, not the materials.
		 */
		bool operator==(const MaterialRecord& a_other) const
		{
			return vs.floats == a_other.vs.floats && ps.floats == a_other.ps.floats &&
			       textures == a_other.textures && addressModes == a_other.addressModes &&
			       filterModes == a_other.filterModes && textureWritten == a_other.textureWritten && featureTextures == a_other.featureTextures;
		}
	};

	/**
	 * @brief What BSLightingShader::SetupTechnique writes for one pass descriptor: the PerTechnique
	 * groups (fog, colour output clamp) and the sampler filter modes. Ported (engine notes:
	 * SetupTechnique), because it binds real shaders and so cannot run against stand-ins.
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

	/** @brief Evaluates SetupTechnique's writes for a pass descriptor from the current frame's fog and settings. */
	void EvaluateTechnique(std::uint32_t a_passDescriptor, TechniqueConstants& a_out);

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

	/**
	 * @brief Runs BSLightingShader's SetupMaterial / SetupGeometry outside the render loop.
	 *
	 * The shadow state's current shaders are swapped for stand-ins whose constant tables point at
	 * ConstantBlocks and whose constant buffers are null (so nothing is mapped), the shader's raw
	 * technique is set to the pass descriptor, and the renderer shadow state and the D3D11 constant
	 * buffer bindings are restored afterwards. The result is exactly what the native draw binds,
	 * including every Community Shaders hook on those functions.
	 */
	class ConstantEvaluator
	{
	public:
		static ConstantEvaluator& Get();

		/** @brief The BSLightingShader instance; learned from any lighting render pass. */
		void SetLightingShader(RE::BSShader* a_shader) { lightingShader = a_shader; }
		bool HasLightingShader() const { return lightingShader != nullptr; }
		RE::BSShader* GetLightingShader() const { return lightingShader; }

		/** @brief True while a stand-in call runs (hooks on the shader functions must ignore it). */
		static bool Evaluating() { return evaluating; }

		/** @brief Start of a frame's evaluations (CS_DCLF_EVAL=audit budget). */
		static void ResetFrameAudits() { auditsThisFrame = 0; }

		bool EvaluateMaterial(const RE::BSShaderMaterial* a_material, std::uint32_t a_passDescriptor, MaterialRecord& a_out);

		/**
		 * @brief SetupGeometry for a copy of a_templatePass (a lighting pass of a representative object,
		 * which supplies the scene light list the engine reads the sun from).
		 */
		bool EvaluateGeometry(const RE::BSRenderPass& a_templatePass, std::uint32_t a_passDescriptor, std::uint32_t a_renderFlags, GeometryConstants& a_out);

	private:
		template <class Call>
		bool RunStandIn(std::uint32_t a_level, std::uint32_t a_passDescriptor, ConstantBlock& a_vs, ConstantBlock& a_ps, Call&& a_call);

		RE::BSShader* lightingShader = nullptr;
		static inline bool evaluating = false;
		// CS_DCLF_EVAL=audit only: how many evaluations have been audited this frame, so the cost of the
		// snapshots stays bounded. Reset by ResetFrameAudits once per BuildFrame.
		static inline std::uint32_t auditsThisFrame = 0;
	};
}
