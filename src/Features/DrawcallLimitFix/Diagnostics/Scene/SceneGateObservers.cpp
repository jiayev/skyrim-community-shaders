// The LOD gates' observers (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY; the normal path trusts the commit): a flip's incoming
// geometries claimed with the flip's phases and no outgoing one claimed - in the publication its commit made (the coordinator's side of
// <- GATE HOLE) and in the claims the frame that releases it installed.

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Gates.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;
		namespace G = Scene::Gates;

		void OnFlipsPublished(const On<ScenePoint::GateFlipsPublished>&)
		{
			// A hole here is the commit's or the revocation's, one only at the release the hand-over's.
			const auto& store = Scene::Coordinator::Get();
			auto* gates = G::StateOf(V::componentState(store, G::component));
			if (!gates)
				return;
			auto& g = gates->observed;
			const auto* claims = V::publicationClaims(store);
			for (const auto& flip : G::Flips(V::componentDeltas(store, G::component)))
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
			auto& rs = G::Releases().observed;
			++rs.checked;
			for (const auto& [geometry, phases] : a_event.incoming)
				if (const std::uint8_t installedPhases = a_event.claims.PhasesOf(geometry); installedPhases != phases) {
					// A refused flip's engine already moved on (forced, or a later gate superseded it), so a later publication may have
					// retired its incoming members: counted apart, not a hole.
					if (!a_event.released) {
						++rs.refusedHoles;
						continue;
					}
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
		Subscribe<ScenePoint::GateFlipsPublished>(OnFlipsPublished);
		Subscribe<GateReleasing>(OnReleasing);
	}
}
