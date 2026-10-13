#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

// The LOD gates' observers (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY; the normal path trusts the commit): a flip's incoming
// geometries claimed with the flip's phases and no outgoing one claimed - in the publication its commit made (the coordinator's side of
// <- GATE HOLE) and in the claims the frame that releases it installed.

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		void OnFlipsPublished(const GateFlipsPublished& a_event)
		{
			// A hole here is the commit's or the revocation's, one only at the release the hand-over's.
			auto& g = SceneDiagnostics().gates;
			const auto* claims = V::publicationClaims(a_event.store);
			for (const auto& flip : V::gateFlipsMade(a_event.store))
				for (const auto& [geometry, phases] : flip.incoming)
					if (const std::uint8_t claimed = claims ? claims->PhasesOf(geometry) : std::uint8_t{ 0 }; claimed != phases) {
						++g.publishHoles;
						if (g.firstPublishHole.empty())
							g.firstPublishHole = fmt::format("gate {}: geometry {} claimed {:#x}, flipped {:#x}", flip.token, static_cast<const void*>(geometry), claimed, phases);
					}
		}

		void OnReleasing(const GateReleasing& a_event)
		{
			// Every incoming geometry claimed with the flip's phases, no outgoing claimed. A later publication's legitimate change of an
			// incoming member reads as a hole here too.
			auto& rs = SceneDiagnostics().gateReleases;
			++rs.checked;
			for (const auto& [geometry, phases] : a_event.incoming)
				if (const std::uint8_t installedPhases = a_event.claims.PhasesOf(geometry); installedPhases != phases) {
					++rs.holes;
					if (rs.firstHole.empty())
						rs.firstHole = fmt::format("gate {}: geometry {} claimed {:#x}, flipped {:#x}", a_event.token, static_cast<const void*>(geometry), installedPhases, phases);
				}
			for (const auto* geometry : a_event.outgoing)
				if (const std::uint8_t installedPhases = a_event.claims.PhasesOf(geometry)) {
					++rs.coverage;
					if (rs.firstCoverage.empty())
						rs.firstCoverage = fmt::format("gate {}: geometry {} claimed {:#x}", a_event.token, static_cast<const void*>(geometry), installedPhases);
				}
		}
	}

	void InstallGateObservers()
	{
		Subscribe<GateFlipsPublished>(OnFlipsPublished);
		Subscribe<GateReleasing>(OnReleasing);
	}
}
