#pragma once

// Core's fixed slot per program family (Api::ProgramFamily): what Core reserves for each, whichever feature takes it. Its key bit in the
// raster flags (Records.h's PipelineRasterFlags: kRasterEffect for the Effect family; KeyFamily tests it at compile time), the three spare
// key bits a family may use (21-23, a main Lighting key's translucency bits: kRasterEffectBillboardShift), its bit in the stage cache's key
// (ShaderPrograms::StageCache::Request) and in its programs' ids (ShaderPrograms::FamilyProgramId: distinct from every Lighting program's),
// and the salt of its pipelines' Z key (DrawPipelines' BuildForwardOrDecal: never a Lighting pipeline's). Its counters are DrawPipelines'
// (Stats::families). A new family is the enum value and its slot here, then the feature's registration (Extensions::ProgramFamilies).
// ShaderPrograms.h checks the key bits against Records.h.

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace DCLF
{
	/** @brief A program family's slot. */
	struct ProgramFamilySlot
	{
		Api::ProgramFamily family;
		std::uint32_t keyBit;        // its raster key bit
		std::uint32_t spareShift;    // its spare key bits' shift (kProgramFamilySpareBits of them)
		std::uint64_t stageKeyBit;   // its stages' bit in the stage cache's key
		std::uint64_t programIdBit;  // its programs' bit in their ids
		std::uint64_t zSalt;         // XORed into its pipelines' Z key's vertex hash
	};
	/** @brief The spare key bits a family may use, at its spareShift. */
	inline constexpr std::uint32_t kProgramFamilySpareBits = 7u;

	inline constexpr std::array<ProgramFamilySlot, static_cast<std::size_t>(Api::ProgramFamily::Count)> kProgramFamilySlots = { {
		{ Api::ProgramFamily::Effect, 1u << 17, 21, 1ull << 37, 1ull << 63, 0xEFFEC7EFFEC7EFFEull },
	} };
	static_assert(kProgramFamilySlots[static_cast<std::size_t>(Api::ProgramFamily::Effect)].family == Api::ProgramFamily::Effect, "kProgramFamilySlots is by family");

	constexpr const ProgramFamilySlot& ProgramFamilySlotOf(Api::ProgramFamily a_family) { return kProgramFamilySlots[static_cast<std::size_t>(a_family)]; }

	/** @brief The family whose key bit a_rasterFlags carry (a key's: PipelineKey::rasterFlags), or Count for a Lighting key. */
	constexpr Api::ProgramFamily KeyFamily(std::uint32_t a_rasterFlags)
	{
		for (const auto& slot : kProgramFamilySlots)
			if (a_rasterFlags & slot.keyBit)
				return slot.family;
		return Api::ProgramFamily::Count;
	}
}
