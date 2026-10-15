#pragma once

// The LOD gates' publication and adoption (GateAdoption.cpp): their adoption guard (the forced releases), their flips published as their
// component's deltas (CoordinatorComponentContext::PublishDelta) and released by the frame adopting them, and the releases' stats.
// Registered by Core's built-in gates (GateComponent.cpp).

#include <cstdint>
#include <memory>
#include <vector>

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"

namespace DCLF::Api
{
	class CoordinatorComponentContext;
}

namespace DCLF::Scene::GateAdoption
{
	/** @brief The LOD gates' coordinator component (GateComponent.cpp's registration, at InstallFeatures; none while they are disabled). */
	inline Api::CoordinatorComponentId component;
	/** @brief The LOD gates' SceneStack channel (GateComponent.cpp's registration): the adoption wakes it for the outcomes the flips posted. */
	inline Api::EngineChannelId channel;

	/** @brief The gates' adoption guard (Api::AdoptionGuard::generation): LodGates::ForcedGeneration. */
	std::uint64_t ForcedGeneration();
	/** @brief The component's state (none of its own yet: the gates' coordinator side is still the coordinator's). */
	std::shared_ptr<void> MakeState();
	/** @brief Commit::published: the GateFlipsPublished observers, against the publication's claims. */
	void Published(const Api::CoordinatorComponentContext& a_context);
	/**
	 * @brief Adoption::adopt (render thread, the frame's start, after InstallClaims or WithdrawSet, before the culls): releases the flips
	 * the installed publications carried (LodGates::Flip: incoming shown, outgoing hidden, in the first post-swap frame), then wakes the
	 * coordinator. Under set or persistent parity, checks the claims against each flip (<- GATE HOLE, <- GATE COVERAGE).
	 */
	void Adopt(const Api::AdoptionContext& a_context);

	/** @brief Render thread: the releases since the last report (Reports::GateReleases takes them). */
	struct ReleaseStats
	{
		std::uint64_t released = 0, refused = 0, withdrawn = 0;
		std::vector<double> flipToReleaseMs, requestToReleaseMs;
		std::vector<std::uint32_t> flipToReleaseFrames;
		bool any = false;
	};
	ReleaseStats& Releases();
}
