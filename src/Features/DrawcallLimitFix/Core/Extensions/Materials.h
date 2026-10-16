#pragma once

// The material contributor contract's Core side (Api/Materials.h): the contributors in their resolved order (ResolveMaterialContributors,
// at InstallFeatures), what their callbacks are handed (MaterialContributorFrame, MaterialContributorEvaluation), the feature texture
// slots assigned to their registers, and the calls Core's Lighting port makes per event: the frame's sample (SampleMaterialContributors),
// a material class's capture (MaterialClassOf and its bytes and fields) and SetupMaterial's chain (EvaluateReplace, MaterialRetryable,
// EvaluateAfter). Where Core decides per frame per object or pipeline it reads resolved data (FeatureTextureRegister, FeatureTextureSlotOf,
// MaterialSignatureBits), never calling a contributor.

#include "Features/DrawcallLimitFix/Api/Materials.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF
{
	// Core's record and the port's inputs (Scene/Ports: ConstantEvaluator.h, MaterialPort.h, the hook frame's header).
	struct MaterialRecord;
	namespace MaterialPort
	{
		struct MaterialSnapshot;
		struct HookFrame;
	}
}

namespace DCLF::Extensions
{
	/** @brief The most material contributors there may be (MaterialContributorSamples holds one sample each): grow it before more register. */
	inline constexpr std::size_t kMaterialContributorSlots = 4;
	/**
	 * @brief MaterialRecord::featureTextures' slots: the row header's feature words, frozen with DCLFObjects.hlsli's decode
	 * (DCLFMaterialTextureIndex: 71 to slot 32, 74 to 33, 80 and up to 34 and up). The contributors' registers, in ascending order, fill
	 * them from 0; one past them fails the install.
	 */
	inline constexpr std::uint32_t kFeatureTextureSlots = 14;
	/** @brief The frame witness words the contributors' samples write, all of them (each its witnessWords, in resolved order). */
	inline constexpr std::uint32_t kMaterialContributorWitnessWords = 4;
	/** @brief A snapshot of no contributor's class (MaterialSnapshot's class; a class is its contributor's resolved index + 1). */
	inline constexpr std::uint8_t kNoMaterialClass = 0;
	/** @brief A slot no contributor's register has (FeatureTextureRegister), a register no slot has (FeatureTextureSlotOf: -1). */
	inline constexpr std::uint32_t kNoFeatureTextureRegister = ~0u;

	/** @brief One contributor's frame sample (MaterialContributorFrame's writes). */
	struct MaterialContributorSample
	{
		bool active = false;
		std::shared_ptr<const void> held;  // shared between frames while unchanged (its identity is the sample's witness words' business)
	};
	/**
	 * @brief Every contributor's frame sample, by resolved index, and their witness words: MaterialPort::MaterialFrame's (sampled with it
	 * at the frame's start), read by the evaluations of that frame. The witness (MaterialPort::WitnessOf) takes ActiveBits and words.
	 */
	struct MaterialContributorSamples
	{
		std::array<MaterialContributorSample, kMaterialContributorSlots> contributors{};
		std::array<std::uint32_t, kMaterialContributorWitnessWords> words{};

		/** @brief Bit c: contributor c is active. */
		std::uint32_t ActiveBits() const
		{
			std::uint32_t bits = 0;
			for (std::size_t c = 0; c < contributors.size(); ++c)
				bits |= contributors[c].active ? 1u << c : 0u;
			return bits;
		}
	};

	/**
	 * @brief What one evaluation hands the contributors (the Lighting port's Evaluate, the retry test): the material's snapshot and its class
	 * (MaterialSnapshot's, from MaterialClassOf at its capture), the pass descriptor, and the frame's hook frame and samples (MaterialFrame's).
	 */
	struct MaterialContributorInputs
	{
		const MaterialPort::MaterialSnapshot& snapshot;
		std::uint8_t materialClass = kNoMaterialClass;
		std::uint32_t pass = 0;
		const MaterialPort::HookFrame& hook;
		const MaterialContributorSamples& samples;
	};
}

namespace DCLF::Api
{
	/**
	 * @brief A contributor's sample (render thread, the frame's start): the engine globals Core's hook frame sampled before it (hook), and
	 * its writes: its active bit (inactive: takesPass, covers and evaluate are not called this frame), a table it holds for the frame's
	 * evaluations (MaterialContributorEvaluation::Held), and its witness words (word a_index < its witnessWords: equal words, equal records).
	 */
	class MaterialContributorFrame
	{
	public:
		MaterialContributorFrame(const MaterialPort::HookFrame& a_hook, Extensions::MaterialContributorSample& a_sample, std::span<std::uint32_t> a_words) :
			hook(a_hook), sample(a_sample), words(a_words) {}

		const MaterialPort::HookFrame& hook;

		void SetActive(bool a_active) { sample.active = a_active; }
		void Hold(std::shared_ptr<const void> a_held) { sample.held = std::move(a_held); }
		void Word(std::uint32_t a_index, std::uint32_t a_value)
		{
			if (a_index < words.size())
				words[a_index] = a_value;
		}

	private:
		Extensions::MaterialContributorSample& sample;
		std::span<std::uint32_t> words;  // its witnessWords of MaterialContributorSamples::words
	};

	/**
	 * @brief A contributor's takesPass, covers or evaluate: the material's snapshot (its reads below, or snapshot's own: MaterialPort.h),
	 * the pass, the hook frame (its engine globals: the technique bytes, the texture-transform buffer, the character light, the render targets,
	 * the default textures), its own sample, and for evaluate the record it writes, in MaterialRecord's terms (as MaterialPortEvaluation's).
	 * takesPass and covers have no record: their writes are not called.
	 */
	class MaterialContributorEvaluation
	{
	public:
		MaterialContributorEvaluation(const Extensions::MaterialContributorInputs& a_inputs, std::size_t a_contributor, MaterialRecord* a_out);

		const MaterialPort::MaterialSnapshot& snapshot;
		const MaterialPort::HookFrame& hook;

		// The pass descriptor (the technique and flags BSLightingShader+0x94 would hold); its sample's active bit and held table (null: none,
		// or another type than it held).
		std::uint32_t Pass() const { return inputs.pass; }
		bool Active() const;
		template <class T>
		const T* Held() const
		{
			return static_cast<const T*>(HeldPointer());
		}
		// The snapshot: its material feature (BSShaderMaterial::GetFeature), bytes taken, class (kNoMaterialClass: none), whether it is of
		// this contributor's own class, the value at a_offset (zero past the bytes), whether the texture field at a_offset holds a texture,
		// the view resolved for it (null: none).
		std::uint32_t Feature() const;
		std::uint32_t Size() const;
		std::uint8_t MaterialClass() const { return inputs.materialClass; }
		bool OwnClass() const;
		template <class T>
		T At(std::size_t a_offset) const
		{
			T value{};
			Read(a_offset, &value, sizeof(T));
			return value;
		}
		bool HasTexture(std::uint16_t a_offset) const { return At<const void*>(a_offset) != nullptr; }
		ID3D11ShaderResourceView* View(std::uint16_t a_offset) const;

		// The record (evaluate only): cleared (constants unwritten, no textures, address modes 0, filter modes unwritten, no feature
		// textures; its kind left as it is: Lighting's); each stage's floats (Lighting's StageLayout order: LightingVSLayout,
		// LightingPSLayout); a pixel slot's filter mode, and its texture with its address mode (written); a feature texture of its own, by
		// local index (its textureRegisters' position; one past them is dropped).
		void ResetRecord();
		float* VSFloats();
		float* PSFloats();
		void SetFilterMode(std::uint32_t a_slot, std::uint32_t a_mode);
		void BindTexture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_addressMode);
		void FeatureTexture(std::uint32_t a_local, ID3D11ShaderResourceView* a_view);

	private:
		const Extensions::MaterialContributorInputs& inputs;
		std::size_t contributor;  // its resolved index
		MaterialRecord* out;      // null for takesPass and covers

		const void* HeldPointer() const;
		// a_bytes at a_offset into a_out when the snapshot has them (MaterialSnapshot::At's test), a_out left as it is otherwise.
		void Read(std::size_t a_offset, void* a_out, std::size_t a_bytes) const;
	};
}

namespace DCLF::Extensions
{
	/**
	 * @brief A contributor as registered and resolved (its descriptor, the name owned): its feature texture registers and first slot
	 * (assigned at resolve: its registers take slots [firstSlot, firstSlot + registers.size())), its witness words at
	 * MaterialContributorSamples::words[firstWord, firstWord + witnessWords), its class (kNoMaterialClass without materialClass).
	 */
	struct MaterialContributorEntry
	{
		std::string name;
		Api::MaterialStage stage = Api::MaterialStage::After;
		std::uint8_t order = 0;
		std::span<const std::uint32_t> registers;
		std::uint32_t firstSlot = 0;
		std::uint32_t witnessWords = 0, firstWord = 0;
		std::uint32_t signatureBits = 0;
		Api::MaterialClass materialClass;
		std::uint8_t classIndex = kNoMaterialClass;
		decltype(Api::MaterialContributor::sample) sample = nullptr;
		decltype(Api::MaterialContributor::takesPass) takesPass = nullptr;
		decltype(Api::MaterialContributor::covers) covers = nullptr;
		decltype(Api::MaterialContributor::evaluate) evaluate = nullptr;
	};

	/** @brief The resolved contributors and what Core reads of them as data. */
	struct MaterialContributorTable
	{
		std::array<MaterialContributorEntry, kMaterialContributorSlots> entries{};  // by resolved index: stage, then order, then registration
		std::size_t count = 0;
		// By slot: its register (kNoFeatureTextureRegister past featureTextures), ascending.
		std::array<std::uint32_t, kFeatureTextureSlots> featureTextureRegisters = [] {
			std::array<std::uint32_t, kFeatureTextureSlots> registers{};
			registers.fill(kNoFeatureTextureRegister);
			return registers;
		}();
		std::uint32_t featureTextures = 0;
		std::uint32_t signatureBits = 0;  // every contributor's
		std::uint32_t witnessWords = 0;   // every contributor's
		bool classes = false;             // any contributor has a material class
	};

	/**
	 * @brief Main thread, the end of InstallFeatures: a_registered (in registration order, validated by the registry) ordered by stage,
	 * then order, then registration; their slots assigned (every register ascending, from slot 0) and their witness words and classes.
	 */
	void ResolveMaterialContributors(std::span<const MaterialContributorEntry> a_registered);
	/** @brief Any thread, after InstallFeatures (empty before): the resolved table. */
	const MaterialContributorTable& MaterialContributors();
	/** @brief The resolved contributors, in order. */
	inline std::span<const MaterialContributorEntry> MaterialContributorList()
	{
		const auto& table = MaterialContributors();
		return std::span(table.entries).first(table.count);
	}

	// Data, any thread after InstallFeatures (the pipeline layouts, the material bindings, the main build's missing-texture report, the
	// capture parity, the signature).
	/** @brief The feature texture slots assigned (the registers' count): slots past it are never written. */
	inline std::uint32_t FeatureTextureCount() { return MaterialContributors().featureTextures; }
	/** @brief Slot a_slot's PS texture register (kNoFeatureTextureRegister past FeatureTextureCount). */
	inline std::uint32_t FeatureTextureRegister(std::uint32_t a_slot)
	{
		return a_slot < kFeatureTextureSlots ? MaterialContributors().featureTextureRegisters[a_slot] : kNoFeatureTextureRegister;
	}
	/** @brief The slot of PS texture register a_register (MaterialRecord::featureTextures' entry), or -1 for none. */
	inline int FeatureTextureSlotOf(std::uint32_t a_register)
	{
		const auto& table = MaterialContributors();
		for (std::uint32_t slot = 0; slot < table.featureTextures; ++slot)
			if (table.featureTextureRegisters[slot] == a_register)
				return static_cast<int>(slot);
		return -1;
	}
	/** @brief The pass bits the contributors branch on (MaterialSources::Signature ORs them into Core's). */
	inline std::uint32_t MaterialSignatureBits() { return MaterialContributors().signatureBits; }

	// The capture (MaterialPort::Capture, per material capture: any thread the material is alive on).
	/** @brief The class of a material of vtable a_vtable (a contributor's whose owns answers true, in resolved order), or kNoMaterialClass. */
	std::uint8_t MaterialClassOf(const void* a_vtable);
	/** @brief Class a_class's bytes of a_material (0 for kNoMaterialClass). */
	std::uint32_t MaterialClassBytes(std::uint8_t a_class, const RE::BSShaderMaterial& a_material);
	/** @brief Class a_class's texture fields of a_material into a_out (a_count set; 0 for kNoMaterialClass). */
	void MaterialClassTextureFields(std::uint8_t a_class, const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count);
	/** @brief Whether vanilla runs on class a_class's material as BSLightingShaderMaterialBase (false for kNoMaterialClass). */
	bool MaterialClassVanillaAsBase(std::uint8_t a_class);

	/**
	 * @brief Render thread, the frame's start (MaterialPort::SampleFrame, after the hook frame): every contributor's sample into a_out
	 * (each reset first: inactive, nothing held, its words zero), in resolved order.
	 */
	void SampleMaterialContributors(const MaterialPort::HookFrame& a_hook, MaterialContributorSamples& a_out);

	// SetupMaterial's chain (the Lighting port's Evaluate, pure; the scene lane or the join pool), in its order:
	//   o = EvaluateReplace(in, out); covered = o == Made || (o == HandBack && vanilla's evaluation);
	//   if (!covered || MaterialRetryable(in)) uncovered; else EvaluateAfter(in, out).
	/**
	 * @brief The first active Replace contributor whose takesPass takes a_inputs' pass: its evaluate's outcome into a_out; HandBack when none
	 * takes it (vanilla's).
	 */
	Api::MaterialOutcome EvaluateReplace(const MaterialContributorInputs& a_inputs, MaterialRecord& a_out);
	/** @brief Whether an active After contributor's covers is false for a_inputs (its input is not set up yet: evaluate again later). */
	bool MaterialRetryable(const MaterialContributorInputs& a_inputs);
	/** @brief Every active After contributor's evaluate into a_out (covered, and no retry), in order. */
	void EvaluateAfter(const MaterialContributorInputs& a_inputs, MaterialRecord& a_out);
}
