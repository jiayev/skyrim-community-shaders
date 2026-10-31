#pragma once

// The pipeline lane's part of Core's fixed slot per draw family (Api::DrawFamily; the rest of the slot is
// Core/Render/DrawFamilies/FamilySlots.h): its stages' bit in the stage cache's key and whether they are pulled builds
// (ShaderPrograms::StageCache::Request), its forward program's id (distinct from every ShaderPrograms::ForwardProgramId the lane parks
// keys under) and a trees view's forward key flags for it (ForwardPipelineKey: DrawPipelines.h checks them). Here, below the render
// layer, so ShaderPrograms and DrawPipelines read it.

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace DCLF
{
	inline constexpr std::size_t kDrawFamilyLanes = static_cast<std::size_t>(Api::DrawFamily::Count);

	/** @brief A draw family's lane slot. */
	struct DrawFamilyLaneSlot
	{
		Api::DrawFamily family;
		std::uint32_t forwardFlags;       // a trees view's forward key flags for it (ForwardPipelineKey::kForwardTreeLod; 0: no faces)
		std::uint64_t stageKeyBit;        // its stages' bit in the stage cache's key (36 up; 37 is the Effect program family's)
		bool pulled;                      // its stages are pulled builds (DCLF_PULLED)
		std::uint64_t forwardProgramId;   // its forward program's id (DrawPipelines' forward program waiters): ~0 - family
	};
	inline constexpr std::array<DrawFamilyLaneSlot, kDrawFamilyLanes> kDrawFamilyLaneSlots = { {
		{ Api::DrawFamily::TreeInstances, 2u, 1ull << 36, true, ~0ull - 0 },
		{ Api::DrawFamily::Grass, 0u, 1ull << 38, true, ~0ull - 1 },
	} };
	static_assert(kDrawFamilyLaneSlots[static_cast<std::size_t>(Api::DrawFamily::TreeInstances)].family == Api::DrawFamily::TreeInstances &&
	                  kDrawFamilyLaneSlots[static_cast<std::size_t>(Api::DrawFamily::Grass)].family == Api::DrawFamily::Grass,
		"kDrawFamilyLaneSlots is by family");

	constexpr const DrawFamilyLaneSlot& DrawFamilyLaneSlotOf(Api::DrawFamily a_family) { return kDrawFamilyLaneSlots[static_cast<std::size_t>(a_family)]; }
}
