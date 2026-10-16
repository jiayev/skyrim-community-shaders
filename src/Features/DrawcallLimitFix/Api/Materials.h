#pragma once

// The material contributor contract (Api::InstallContext::RegisterMaterialContributor): a CS feature's hook on the Lighting shader's
// SetupMaterial (Core's Lighting port, kind 0), ported as data Core reads and the callbacks it makes per event. The engine's hook chain is
// two stages in a declared order:
//
// - Replace: a replacement around vanilla SetupMaterial (Integrations/TruePBR). It takes the passes it takes (takesPass) and writes the
//   record itself, or hands the pass back to vanilla (MaterialOutcome::HandBack); the first active Replace contributor that takes a pass
//   evaluates it, else vanilla does.
// - After: a hook outside vanilla and the replacement (Integrations/AdvancedSkin's t71/t74): every active one evaluates, in order, once the
//   pass is covered and no After contributor needs a retry (covers false: an input its frame sample does not have yet).
//
// Each contributor samples its part of the frame at the frame's start (an active bit, a held table, words of the frame witness: the
// signature probes evaluate again only when the witness moves), and may own a material class of its own captured as Lighting's
// (MaterialClass: CS's PBR classes). The feature texture registers a contributor binds are declared by it (textureRegisters); Core assigns
// them, in ascending register order over every contributor, onto MaterialRecord::featureTextures' frozen slots (Extensions::
// kFeatureTextureSlots: the row header's words and DCLFObjects.hlsli's decode, which Features/Manifest.cpp checks the result against).
//
// What Core reads per frame per object or pipeline is resolved once at install (the slots' registers, the signature bits); the callbacks run
// per (material, pass) evaluation, capture rewrite and signature probe, never per object per frame (Core/Extensions/Materials.h).

#include <cstdint>
#include <span>
#include <string_view>

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/Materials.h), which the features and integrations inside DCLF include for them.
	class MaterialContributorFrame;
	class MaterialContributorEvaluation;

	/** @brief Where a contributor sits in SetupMaterial's hook chain. */
	enum class MaterialStage : std::uint8_t
	{
		Replace,  // a replacement around vanilla SetupMaterial: takesPass, then evaluate (or a hand-back to vanilla)
		After,    // a hook outside it: covers, then evaluate, once the pass is covered
	};

	/** @brief What a contributor's evaluate made of the pass. */
	enum class MaterialOutcome : std::uint8_t
	{
		Made,       // the record is written
		Uncovered,  // a material the port does not cover: the material stays native
		HandBack,   // vanilla's: nothing written, vanilla SetupMaterial evaluates it (TruePBR's CHECK_PBR_TEXTURE). Replace only
	};

	/**
	 * @brief A material class of the contributor's own (not one of the vanilla Lighting classes), captured as Lighting's (kind 0). Called
	 * per material capture (MaterialPort::Capture: any thread the material is alive on, the engine-read window or its writer), never per
	 * frame. All three callbacks or none.
	 *
	 * - owns: whether a material of vtable a_vtable is of the class (CS's own classes: a vtable in this DLL's image).
	 * - bytes: the bytes a snapshot keeps of a_material (its class's size; at most MaterialPort::kMaxMaterialBytes, else uncovered).
	 * - textureFields: its NiSourceTexture fields (byte offsets) into a_out (MaterialPort::kMaxTextureFields of room); a_count is set, not
	 *   added to.
	 * - vanillaAsBase: a pass handed back (or no Replace contributor's) runs vanilla on it as BSLightingShaderMaterialBase: the base's four
	 *   texture fields, the snapshot's size (Extensions::MaterialClassVanillaAsBase); false: vanilla leaves it uncovered.
	 */
	struct MaterialClass
	{
		bool (*owns)(const void* a_vtable) = nullptr;
		std::uint32_t (*bytes)(const RE::BSShaderMaterial& a_material) = nullptr;
		void (*textureFields)(const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count) = nullptr;
		bool vanillaAsBase = true;
	};

	/**
	 * @brief A material contributor (Api::InstallContext::RegisterMaterialContributor). Copied (the name too; textureRegisters as given:
	 * static storage). Core orders the contributors by stage, then order, then registration (Extensions::MaterialContributors).
	 *
	 * - name: the install's failures' name for it.
	 * - stage, order: its place in the chain.
	 * - textureRegisters: the PS texture registers it binds per material (Lighting.hlsl's, the feature's compile-time constants, which
	 *   Features/Manifest.cpp checks against the decode), strictly ascending, none another contributor's range overlaps, none Core's, a frame
	 *   binding's, a frame texture's or a frame graph buffer's. Its local index n (MaterialContributorEvaluation::FeatureTexture) is the
	 *   register at position n; its slot is assigned at install (Extensions::FeatureTextureRegister).
	 * - witnessWords: the words its sample writes into the frame witness (MaterialContributorFrame::Word; at most
	 *   Extensions::kMaterialContributorWitnessWords over every contributor).
	 * - signatureBits: the pass descriptor bits its takesPass, covers or evaluate branch on, ORed into the material signature's
	 *   (MaterialSources::Signature, Extensions::MaterialSignatureBits): data, read per frame per slot.
	 * - materialClass (optional): its own class.
	 * - sample: the render thread at the frame's start (MaterialPort::SampleFrame, FrameGlobals::material), after Core's hook frame: its
	 *   active bit, held table and witness words. Cheap; no engine write.
	 * - takesPass (Replace; required): whether it takes the pass (called only while Active). Pure.
	 * - covers (After; required): whether its evaluate can give the material's record now; false: Retry (an input its frame sample has not
	 *   set up yet: the material is evaluated again with a later pass's frame). Called only while Active. Pure.
	 * - evaluate (required): the record (Replace: Made, Uncovered or HandBack; After: writes on top of the covered record, its outcome
	 *   ignored but Made expected). Pure; any thread (the scene lane or the join pool).
	 */
	struct MaterialContributor
	{
		std::string_view name;
		MaterialStage stage = MaterialStage::After;
		std::uint8_t order = 0;
		std::span<const std::uint32_t> textureRegisters;
		std::uint32_t witnessWords = 0;
		std::uint32_t signatureBits = 0;
		MaterialClass materialClass;
		void (*sample)(MaterialContributorFrame& a_frame) = nullptr;
		bool (*takesPass)(const MaterialContributorEvaluation& a_evaluation) = nullptr;
		bool (*covers)(const MaterialContributorEvaluation& a_evaluation) = nullptr;
		MaterialOutcome (*evaluate)(MaterialContributorEvaluation& a_evaluation) = nullptr;
	};
}
