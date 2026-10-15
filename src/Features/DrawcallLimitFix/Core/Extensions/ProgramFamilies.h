#pragma once

// The program family contract's Core side (Api/DrawFamilies.h, ProgramFamilyContribution): each family's entry, resolved once at
// InstallFeatures (ResolveProgramFamilies, after the material ports) from Core's built-ins (CoreProgramFamilies) and the features'
// registrations, and the rules Core reads per object per frame as data (FamilyRules: by a key's family, KeyFamily; FamilyRulesOfKind: by a
// record's material kind), never calling a family. Core's slot per family is Core/Gpu/Pipelines/ProgramFamilySlots.h; its programs are
// ShaderPrograms::FindFamily's, its pipelines DrawPipelines'.

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ProgramFamilySlots.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace DCLF::Extensions
{
	inline constexpr std::size_t kProgramFamilies = static_cast<std::size_t>(Api::ProgramFamily::Count);

	/**
	 * @brief What Core reads of a key's family per object per frame (the family's registration with its slot's bits and its port's kind).
	 * FamilyRules' entry kProgramFamilies (and a family no feature takes) is a Lighting key's: no key bit, kind 0, its rows' blocks, a depth
	 * segment input, Lighting's ProjectedUV bit, no record bits.
	 */
	struct ProgramFamilyRules
	{
		std::uint32_t keyBit = 0;      // the slot's raster key bit (0: a Lighting key's)
		std::uint32_t spareShift = 0;  // the slot's spare key bits (turnedKeyBits)
		std::uint8_t kind = 0;         // the material kind of its keys' shader (MaterialSpaceOf, PortRules)
		bool rowBlocks = true;
		bool depthSegment = true;      // false: ProgramBuild::ForwardOrDecal's: no depth segment input, no Z-prepass bucket
		bool rootViewsForwardOnly = false;
		bool turnedKeyBits = false;
		std::uint32_t projectedPassBit = 0;  // its port's rules.projectedPassBit (0: no ProjectedUV binding)
		Api::ProgramFamilyContribution::Records records;
	};
	using ProgramFamilyRulesTable = std::array<ProgramFamilyRules, kProgramFamilies + 1>;

	/** @brief A family's entry: its registration (taken: Core's built-in or a feature's), with its strings owned. */
	struct ProgramFamilyEntry
	{
		bool taken = false;
		Api::ProgramFamilyContribution contribution;
		std::string name, keyName, sourcePath, nativeNote;
		std::uint8_t kind = 0;  // its port's material kind
	};
	using ProgramFamilySet = std::array<ProgramFamilyEntry, kProgramFamilies>;

	/**
	 * @brief Core's own families, registered ahead of the features' (Registry's InstallFeatures): the Effect family, until Features/Effects
	 * registers it (Scene/Ports/EffectPrograms.cpp).
	 */
	std::span<const Api::ProgramFamilyContribution> CoreProgramFamilies();
	/**
	 * @brief Main thread, the end of InstallFeatures, after ResolveMaterialPorts: the families' entries and rules from their registrations
	 * (family Count: none). Fails the install for a family whose shader type no material port has.
	 */
	void ResolveProgramFamilies(const std::array<Api::ProgramFamilyContribution, kProgramFamilies>& a_registered);
	/** @brief Any thread, after InstallFeatures (none taken before): the families' entries, by family. */
	const ProgramFamilySet& ProgramFamilies();
	inline const ProgramFamilyEntry& ProgramFamilyOf(Api::ProgramFamily a_family) { return ProgramFamilies()[static_cast<std::size_t>(a_family)]; }
	/** @brief The taken family whose programs are of shader type a_shaderType (RE::BSShader::Type), or Count. */
	Api::ProgramFamily FamilyOfShader(std::uint8_t a_shaderType);

	/** @brief Any thread, after InstallFeatures: the rules by family, then a Lighting key's (kProgramFamilies). */
	const ProgramFamilyRulesTable& FamilyRulesTable();
	/** @brief The rules of a key with a_rasterFlags (PipelineKey::rasterFlags): its family's, or a Lighting key's. */
	inline const ProgramFamilyRules& FamilyRules(std::uint32_t a_rasterFlags)
	{
		return FamilyRulesTable()[static_cast<std::size_t>(KeyFamily(a_rasterFlags))];
	}
	/** @brief The rules of a record of material kind a_kind (LightingDescriptors::port): the family of its port's shader, or a Lighting key's. */
	const ProgramFamilyRules& FamilyRulesOfKind(std::uint8_t a_kind);
}
