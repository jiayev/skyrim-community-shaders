#pragma once

#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <cstdint>
#include <string>

namespace DCLF::Diagnostics
{
	/**
	 * @brief The reflection residue (SceneRegistrations.cpp): what the engine registered into the reflection faces under the LOD land and
	 * objects roots that is no reflection member, classified by the scene pass that takes the frame's drain. Watched under the set or
	 * persistent parity (IndirectDraws::ViewLodRootsOwned); the reflection report (Features/Reflection) takes the classes.
	 */
	inline constexpr std::size_t kResidueUntracked = static_cast<std::size_t>(Ineligible::Count), kResidueUnbound = kResidueUntracked + 1,
								 kResidueNotReflection = kResidueUntracked + 2, kResidueReflection = kResidueUntracked + 3, kResidueKinds = kResidueUntracked + 4;
	struct ResidueClasses
	{
		std::array<std::uint64_t, kResidueKinds> counts{};
		std::array<std::string, kResidueKinds> first;
		struct Seen
		{
			std::uint32_t frame = ~0u, frames = 0;
		};
		ankerl::unordered_dense::map<const void*, Seen> seen;
		std::uint64_t persistent = 0;
	};
	/** @brief Render thread, the reflection report: the classes since the last call. */
	ResidueClasses TakeResidueClasses();
}
