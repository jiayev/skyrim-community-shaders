// The material contributors' table, sample, capture and chain calls (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace DCLF::Extensions
{
	static_assert(std::tuple_size_v<decltype(MaterialRecord::featureTextures)> == kFeatureTextureSlots,
		"MaterialRecord::featureTextures is the row header's feature slots (kFeatureTextureSlots)");

	namespace
	{
		// Written only by ResolveMaterialContributors (InstallFeatures, before DCLF's threads exist), read-only after.
		MaterialContributorTable& Table()
		{
			static MaterialContributorTable table;
			return table;
		}

		/** @brief Class a_class's entry (its contributor's), or null for kNoMaterialClass and past the classes. */
		const MaterialContributorEntry* ClassEntry(std::uint8_t a_class)
		{
			const auto& table = Table();
			return a_class != kNoMaterialClass && a_class <= table.count ? &table.entries[a_class - 1] : nullptr;
		}
	}

	void ResolveMaterialContributors(std::span<const MaterialContributorEntry> a_registered)
	{
		auto& table = Table();
		// Stage, then order, then registration (the stable sort keeps registration order among equals).
		std::array<std::size_t, kMaterialContributorSlots> order{};
		const std::size_t count = std::min(a_registered.size(), order.size());
		for (std::size_t i = 0; i < count; ++i)
			order[i] = i;
		std::stable_sort(order.begin(), order.begin() + count, [&](std::size_t a, std::size_t b) {
			const auto& l = a_registered[a];
			const auto& r = a_registered[b];
			return l.stage != r.stage ? l.stage < r.stage : l.order < r.order;
		});
		std::uint32_t words = 0;
		for (std::size_t c = 0; c < count; ++c) {
			auto& entry = table.entries[c];
			entry = a_registered[order[c]];
			entry.firstWord = words;
			words += entry.witnessWords;
			entry.classIndex = entry.materialClass.owns ? static_cast<std::uint8_t>(c + 1) : kNoMaterialClass;
			table.signatureBits |= entry.signatureBits;
			table.classes = table.classes || entry.classIndex != kNoMaterialClass;
		}
		table.count = count;
		table.witnessWords = words;
		// The slots: every register in ascending order from slot 0 (the registry refused overlapping ranges, so each contributor's are
		// consecutive slots from its first register's).
		std::uint32_t slots = 0;
		for (std::size_t c = 0; c < count; ++c)
			for (const std::uint32_t r : table.entries[c].registers)
				table.featureTextureRegisters[slots++] = r;
		std::sort(table.featureTextureRegisters.begin(), table.featureTextureRegisters.begin() + slots);
		table.featureTextures = slots;
		for (std::size_t c = 0; c < count; ++c) {
			auto& entry = table.entries[c];
			if (!entry.registers.empty())
				entry.firstSlot = static_cast<std::uint32_t>(FeatureTextureSlotOf(entry.registers.front()));
		}
	}

	const MaterialContributorTable& MaterialContributors() { return Table(); }

	std::uint8_t MaterialClassOf(const void* a_vtable)
	{
		if (!Table().classes)
			return kNoMaterialClass;
		for (const auto& entry : MaterialContributorList())
			if (entry.classIndex != kNoMaterialClass && entry.materialClass.owns(a_vtable))
				return entry.classIndex;
		return kNoMaterialClass;
	}

	std::uint32_t MaterialClassBytes(std::uint8_t a_class, const RE::BSShaderMaterial& a_material)
	{
		const auto* entry = ClassEntry(a_class);
		return entry ? entry->materialClass.bytes(a_material) : 0u;
	}

	void MaterialClassTextureFields(std::uint8_t a_class, const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count)
	{
		a_count = 0;
		if (const auto* entry = ClassEntry(a_class))
			entry->materialClass.textureFields(a_material, a_out, a_count);
	}

	bool MaterialClassVanillaAsBase(std::uint8_t a_class)
	{
		const auto* entry = ClassEntry(a_class);
		return entry && entry->materialClass.vanillaAsBase;
	}

	void SampleMaterialContributors(const MaterialPort::HookFrame& a_hook, MaterialContributorSamples& a_out)
	{
		const auto contributors = MaterialContributorList();
		for (std::size_t c = 0; c < contributors.size(); ++c) {
			const auto& entry = contributors[c];
			auto& sample = a_out.contributors[c];
			sample.active = false;
			sample.held.reset();
			const auto words = std::span(a_out.words).subspan(entry.firstWord, entry.witnessWords);
			std::fill(words.begin(), words.end(), 0u);
			Api::MaterialContributorFrame frame{ a_hook, sample, words };
			entry.sample(frame);
		}
	}

	Api::MaterialOutcome EvaluateReplace(const MaterialContributorInputs& a_inputs, MaterialRecord& a_out)
	{
		const auto contributors = MaterialContributorList();
		for (std::size_t c = 0; c < contributors.size(); ++c) {
			const auto& entry = contributors[c];
			if (entry.stage != Api::MaterialStage::Replace || !a_inputs.samples.contributors[c].active)
				continue;
			Api::MaterialContributorEvaluation evaluation{ a_inputs, c, &a_out };
			if (entry.takesPass(evaluation))
				return entry.evaluate(evaluation);
		}
		return Api::MaterialOutcome::HandBack;
	}

	bool MaterialRetryable(const MaterialContributorInputs& a_inputs)
	{
		const auto contributors = MaterialContributorList();
		for (std::size_t c = 0; c < contributors.size(); ++c) {
			const auto& entry = contributors[c];
			if (entry.stage != Api::MaterialStage::After || !a_inputs.samples.contributors[c].active)
				continue;
			if (!entry.covers(Api::MaterialContributorEvaluation{ a_inputs, c, nullptr }))
				return true;
		}
		return false;
	}

	void EvaluateAfter(const MaterialContributorInputs& a_inputs, MaterialRecord& a_out)
	{
		// An inactive one is skipped: its writes would be nulls into entries the covered record's reset already cleared.
		const auto contributors = MaterialContributorList();
		for (std::size_t c = 0; c < contributors.size(); ++c) {
			const auto& entry = contributors[c];
			if (entry.stage != Api::MaterialStage::After || !a_inputs.samples.contributors[c].active)
				continue;
			Api::MaterialContributorEvaluation evaluation{ a_inputs, c, &a_out };
			entry.evaluate(evaluation);
		}
	}
}

namespace DCLF::Api
{
	MaterialContributorEvaluation::MaterialContributorEvaluation(const Extensions::MaterialContributorInputs& a_inputs, std::size_t a_contributor, MaterialRecord* a_out) :
		snapshot(a_inputs.snapshot), hook(a_inputs.hook), inputs(a_inputs), contributor(a_contributor), out(a_out)
	{}

	bool MaterialContributorEvaluation::Active() const { return inputs.samples.contributors[contributor].active; }
	const void* MaterialContributorEvaluation::HeldPointer() const { return inputs.samples.contributors[contributor].held.get(); }
	std::uint32_t MaterialContributorEvaluation::Feature() const { return snapshot.feature; }
	std::uint32_t MaterialContributorEvaluation::Size() const { return snapshot.size; }
	ID3D11ShaderResourceView* MaterialContributorEvaluation::View(std::uint16_t a_offset) const { return snapshot.View(a_offset); }

	bool MaterialContributorEvaluation::OwnClass() const
	{
		const auto own = Extensions::MaterialContributors().entries[contributor].classIndex;
		return own != Extensions::kNoMaterialClass && inputs.materialClass == own;
	}

	void MaterialContributorEvaluation::Read(std::size_t a_offset, void* a_out, std::size_t a_bytes) const
	{
		if (a_offset + a_bytes <= snapshot.size)
			std::memcpy(a_out, snapshot.bytes.data() + a_offset, a_bytes);
	}

	void MaterialContributorEvaluation::ResetRecord()
	{
		// A record with nothing written: unwritten constants, no textures, unwritten filter modes.
		out->vs.Reset();
		out->ps.Reset();
		out->textures = {};
		out->addressModes = {};
		out->filterModes.fill(kUnwrittenFilterMode);
		out->textureWritten = 0;
		out->featureTextures = {};
	}

	float* MaterialContributorEvaluation::VSFloats() { return out->vs.floats.data(); }
	float* MaterialContributorEvaluation::PSFloats() { return out->ps.floats.data(); }
	void MaterialContributorEvaluation::SetFilterMode(std::uint32_t a_slot, std::uint32_t a_mode) { out->filterModes[a_slot] = a_mode; }

	void MaterialContributorEvaluation::BindTexture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_addressMode)
	{
		out->textures[a_slot] = a_view;
		out->addressModes[a_slot] = a_addressMode;
		out->textureWritten |= 1u << a_slot;
	}

	void MaterialContributorEvaluation::FeatureTexture(std::uint32_t a_local, ID3D11ShaderResourceView* a_view)
	{
		const auto& entry = Extensions::MaterialContributors().entries[contributor];
		if (a_local < entry.registers.size())
			out->featureTextures[entry.firstSlot + a_local] = a_view;
	}
}
