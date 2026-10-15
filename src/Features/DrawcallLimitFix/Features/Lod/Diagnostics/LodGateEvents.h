#pragma once

// The LOD gates' hook points (Diagnostics/Hooks.h's mechanism), announced by the gates' publication and adoption (LodGateAdoption.cpp)
// for their observers (LodGateObservers.cpp).

#include <cstdint>
#include <span>
#include <utility>

namespace RE
{
	class BSGeometry;
}

namespace DCLF::Features::Lod
{
	/**
	 * @brief The scene lane, taking a publication's deltas (the gates' published hook): the gates its commits flipped (the gates'
	 * component's deltas pending: Gates::Flips), against its claims.
	 */
	struct GateFlipsPublished
	{
	};

	/**
	 * @brief Render thread, the frame's start (the gates' adoption, Gates::Adopt): a LOD gate flipped by an installed publication, about to
	 * be released, and the claims the frame installed (opaque: read through Diagnostics::SceneView::ClaimedPhases).
	 */
	struct GateReleasing
	{
		const void* claims;
		std::uint64_t token;
		std::span<const std::pair<const RE::BSGeometry*, std::uint8_t>> incoming;  // the incoming base geometries claimed, with the flip's phases
		std::span<const RE::BSGeometry* const> outgoing;                          // the outgoing base geometries dropped
		bool released;                                                            // false: refused (forced or unknown by then; the engine moved on)
	};
}
