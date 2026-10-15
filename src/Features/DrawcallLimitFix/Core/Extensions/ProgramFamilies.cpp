// The program families' entries and rules (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"

namespace DCLF::Extensions
{
	namespace
	{
		struct Resolved
		{
			ProgramFamilySet families;
			ProgramFamilyRulesTable rules;
			std::array<std::size_t, kMaterialKinds> kindRules{};  // by material kind: its rules' index (kProgramFamilies: a Lighting key's)
		};

		Resolved MakeInitial()
		{
			Resolved initial;
			initial.kindRules.fill(kProgramFamilies);
			return initial;
		}

		// Written only by ResolveProgramFamilies (InstallFeatures, before DCLF's threads exist), read-only after. A namespace-scope object,
		// not a function's static: the per-object reads (FamilyRules) take no initialization guard.
		Resolved familyTable = MakeInitial();

		Resolved& Table() { return familyTable; }
	}

	void ResolveProgramFamilies(const std::array<Api::ProgramFamilyContribution, kProgramFamilies>& a_registered)
	{
		auto& table = Table();
		const auto& ports = MaterialPorts();
		// A Lighting key's rules, and a family no feature takes: kind 0's ProjectedUV bit.
		auto& lighting = table.rules[kProgramFamilies];
		lighting.projectedPassBit = ports[0].rules.projectedPassBit;
		for (std::size_t f = 0; f < kProgramFamilies; ++f)
			table.rules[f] = lighting;
		for (std::size_t f = 0; f < kProgramFamilies; ++f) {
			const auto& registered = a_registered[f];
			if (registered.family == Api::ProgramFamily::Count)
				continue;
			// Its keys' material kind: the port of its shader.
			std::uint8_t kind = 0;
			for (std::uint8_t k = 1; k < kMaterialKinds; ++k)
				if (ports[k].shaderType == registered.shaderType)
					kind = k;
			if (!kind)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: program family {} takes shader type {}, which no material port has", registered.name,
					static_cast<std::uint32_t>(registered.shaderType)));
			auto& entry = table.families[f];
			entry.taken = true;
			entry.name = registered.name;
			entry.keyName = registered.keyName;
			entry.sourcePath = registered.sourcePath;
			entry.nativeNote = registered.nativeNote;
			entry.kind = kind;
			// The contribution's strings, the entry's own (the table never moves).
			entry.contribution = registered;
			entry.contribution.name = entry.name;
			entry.contribution.keyName = entry.keyName;
			entry.contribution.sourcePath = entry.sourcePath;
			entry.contribution.nativeNote = entry.nativeNote;
			const auto& slot = kProgramFamilySlots[f];
			auto& rules = table.rules[f];
			rules.keyBit = slot.keyBit;
			rules.spareShift = slot.spareShift;
			rules.kind = kind;
			rules.rowBlocks = registered.rowBlocks;
			rules.depthSegment = registered.build != Api::ProgramBuild::ForwardOrDecal;
			rules.rootViewsForwardOnly = registered.rootViewsForwardOnly;
			rules.turnedKeyBits = registered.turnedKeyBits;
			rules.projectedPassBit = ports[kind].rules.projectedPassBit;
			rules.records = registered.records;
			table.kindRules[kind] = f;
		}
	}

	const ProgramFamilySet& ProgramFamilies() { return Table().families; }

	Api::ProgramFamily FamilyOfShader(std::uint8_t a_shaderType)
	{
		const auto& families = Table().families;
		for (std::size_t f = 0; f < kProgramFamilies; ++f)
			if (families[f].taken && families[f].contribution.shaderType == a_shaderType)
				return static_cast<Api::ProgramFamily>(f);
		return Api::ProgramFamily::Count;
	}

	const ProgramFamilyRulesTable& FamilyRulesTable() { return Table().rules; }

	const ProgramFamilyRules& FamilyRulesOfKind(std::uint8_t a_kind)
	{
		const auto& table = Table();
		return table.rules[a_kind < kMaterialKinds ? table.kindRules[a_kind] : kProgramFamilies];
	}
}
